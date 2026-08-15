// Standalone helper for tools/xenos_batch_convert.py.
//
// XenosRecomp's directory-mode output (see XenosRecomp/main.cpp) is a C++
// source defining a ShaderCacheEntry table plus one ZSTD-compressed,
// smol-v-encoded SPIR-V blob shared by all shaders (offsets/sizes into it
// live in each entry). That's convenient for a project that already links
// zstd+smol-v into its runtime (as XenosRecomp's own upstream consumers do),
// but reNut's Vulkan GPU plugin (rexgpu-renut) is a separate CMake build
// from reNut's own executable (which is the only place XenosRecomp's
// FetchContent-provided zstd/smol-v targets exist) -- wiring that dependency
// across the plugin boundary is real, avoidable complexity for what this
// needs.
//
// So: decompress everything at BUILD TIME instead. This tool takes
// XenosRecomp's own generated cache .cpp, extracts+decompresses+decodes the
// real per-shader SPIR-V bytes, and re-emits a plain, self-contained C++
// source with one flat uint8_t array holding every shader's real (already
// decoded, ready-to-pass-to-vkCreateShaderModule) SPIR-V back to back, plus
// an entry table of (hash, offset, size) pairs -- zero runtime dependencies
// beyond what's already linked into rexgpu-renut.
//
// This is a source-to-source rewrite: it does NOT re-invoke XenosRecomp or
// touch shader semantics, only decodes the compression XenosRecomp itself
// applied. Parses the exact textual format XenosRecomp's main.cpp emits
// (fmt::println calls with fixed field order) rather than a general C++
// parser -- brittle by nature of matching one specific generator's output,
// documented and isolated on purpose so if XenosRecomp's output format ever
// changes, only this one file needs updating.

#include <zstd.h>

#include "smolv.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Entry {
  uint64_t hash = 0;
  size_t spirvOffset = 0;
  size_t spirvSize = 0;
  uint32_t specConstantsMask = 0;
};

std::string ReadFile(const char* path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    std::fprintf(stderr, "xenos_cache_unpack: cannot open %s\n", path);
    std::exit(1);
  }
  std::ostringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

// Parses one "{ 0xHASH, dxilOff, dxilSize, spirvOff, spirvSize, specMask },"
// line, matching XenosRecomp main.cpp's exact fmt::println format string:
//   "\t{{ 0x{:X}, {}, {}, {}, {}, {} }},"
std::vector<Entry> ParseEntries(const std::string& source) {
  std::vector<Entry> entries;
  size_t pos = source.find("g_shaderCacheEntries[]");
  if (pos == std::string::npos) {
    std::fprintf(stderr, "xenos_cache_unpack: g_shaderCacheEntries not found in input\n");
    std::exit(1);
  }
  size_t braceOpen = source.find('{', pos);
  size_t arrayEnd = source.find("};", braceOpen);
  std::string body = source.substr(braceOpen + 1, arrayEnd - braceOpen - 1);

  size_t i = 0;
  while (true) {
    size_t entryOpen = body.find('{', i);
    if (entryOpen == std::string::npos) break;
    size_t entryClose = body.find('}', entryOpen);
    std::string entryText = body.substr(entryOpen + 1, entryClose - entryOpen - 1);

    Entry e;
    // hash, dxilOffset, dxilSize, spirvOffset, spirvSize, specConstantsMask
    unsigned long long hash = 0, dxilOff = 0, dxilSize = 0, spirvOff = 0, spirvSize = 0, specMask = 0;
    if (std::sscanf(entryText.c_str(), " 0x%llX , %llu , %llu , %llu , %llu , %llu", &hash, &dxilOff,
                     &dxilSize, &spirvOff, &spirvSize, &specMask) != 6) {
      std::fprintf(stderr, "xenos_cache_unpack: failed to parse entry '%s'\n", entryText.c_str());
      std::exit(1);
    }
    e.hash = uint64_t(hash);
    e.spirvOffset = size_t(spirvOff);
    e.spirvSize = size_t(spirvSize);
    e.specConstantsMask = uint32_t(specMask);
    entries.push_back(e);

    i = entryClose + 1;
  }
  return entries;
}

// Parses "const uint8_t g_compressedSpirvCache[] = {n,n,n,...};" into raw
// bytes.
std::vector<uint8_t> ParseByteArray(const std::string& source, const char* varName) {
  size_t pos = source.find(varName);
  if (pos == std::string::npos) {
    std::fprintf(stderr, "xenos_cache_unpack: %s not found in input\n", varName);
    std::exit(1);
  }
  size_t braceOpen = source.find('{', pos);
  size_t braceClose = source.find('}', braceOpen);
  std::string body = source.substr(braceOpen + 1, braceClose - braceOpen - 1);

  std::vector<uint8_t> bytes;
  bytes.reserve(body.size() / 3);
  size_t i = 0;
  while (i < body.size()) {
    while (i < body.size() && (body[i] == ',' || body[i] == ' ' || body[i] == '\n' || body[i] == '\t')) ++i;
    if (i >= body.size()) break;
    size_t start = i;
    while (i < body.size() && body[i] != ',') ++i;
    if (i > start) {
      bytes.push_back(uint8_t(std::strtoul(body.substr(start, i - start).c_str(), nullptr, 10)));
    }
  }
  return bytes;
}

size_t ParseSizeT(const std::string& source, const char* varName) {
  size_t pos = source.find(varName);
  if (pos == std::string::npos) {
    std::fprintf(stderr, "xenos_cache_unpack: %s not found in input\n", varName);
    std::exit(1);
  }
  size_t eq = source.find('=', pos);
  size_t semi = source.find(';', eq);
  return size_t(std::strtoull(source.substr(eq + 1, semi - eq - 1).c_str(), nullptr, 10));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <xenos-recomp-generated-cache.cpp> <output.cpp>\n", argv[0]);
    return 1;
  }

  std::string source = ReadFile(argv[1]);
  std::vector<Entry> entries = ParseEntries(source);
  std::vector<uint8_t> compressed = ParseByteArray(source, "g_compressedSpirvCache");
  size_t decompressedSize = ParseSizeT(source, "g_spirvCacheDecompressedSize");

  std::vector<uint8_t> smolvBlob(decompressedSize);
  size_t zstdResult = ZSTD_decompress(smolvBlob.data(), smolvBlob.size(), compressed.data(), compressed.size());
  if (ZSTD_isError(zstdResult) || zstdResult != decompressedSize) {
    std::fprintf(stderr, "xenos_cache_unpack: ZSTD_decompress failed (%s)\n", ZSTD_getErrorName(zstdResult));
    return 1;
  }

  // Decode each shader's smol-v-encoded SPIR-V slice back to real SPIR-V
  // (uint32_t words), and lay them all out contiguously in one flat buffer.
  std::vector<uint32_t> flatSpirv;
  std::vector<Entry> outEntries;
  for (const Entry& e : entries) {
    if (e.spirvSize == 0 || e.spirvOffset + e.spirvSize > smolvBlob.size()) {
      std::fprintf(stderr, "xenos_cache_unpack: entry 0x%llX has an invalid SPIR-V slice, skipping\n",
                   (unsigned long long)e.hash);
      continue;
    }
    size_t decodedSpirvSize = smolv::GetDecodedBufferSize(smolvBlob.data() + e.spirvOffset, e.spirvSize);
    if (decodedSpirvSize == 0) {
      std::fprintf(stderr, "xenos_cache_unpack: entry 0x%llX smol-v size query failed, skipping\n",
                   (unsigned long long)e.hash);
      continue;
    }
    std::vector<uint32_t> spirv(decodedSpirvSize / sizeof(uint32_t));
    if (!smolv::Decode(smolvBlob.data() + e.spirvOffset, e.spirvSize, spirv.data(), decodedSpirvSize)) {
      std::fprintf(stderr, "xenos_cache_unpack: entry 0x%llX smol-v decode failed, skipping\n",
                   (unsigned long long)e.hash);
      continue;
    }

    Entry outEntry = e;
    outEntry.spirvOffset = flatSpirv.size();
    outEntry.spirvSize = spirv.size();
    outEntries.push_back(outEntry);
    flatSpirv.insert(flatSpirv.end(), spirv.begin(), spirv.end());
  }

  std::ofstream out(argv[2]);
  out << "// Generated by tools/xenos_cache_unpack.cpp from XenosRecomp's real compiled shader cache.\n";
  out << "// Plain, decompressed SPIR-V words -- no zstd/smol-v dependency needed to consume this.\n";
  out << "#include \"renut_engine/renut_xenos_shader_cache.h\"\n\n";
  out << "const uint32_t g_renutXenosSpirvWords[] = {\n";
  for (size_t i = 0; i < flatSpirv.size(); ++i) {
    out << flatSpirv[i] << ",";
    if ((i % 16) == 15) out << "\n";
  }
  out << "\n};\n\n";
  out << "const RenutXenosShaderCacheEntry g_renutXenosShaderCacheEntries[] = {\n";
  for (const Entry& e : outEntries) {
    out << "\t{ 0x" << std::hex << e.hash << std::dec << "ULL, " << e.spirvOffset << ", " << e.spirvSize
        << ", " << e.specConstantsMask << " },\n";
  }
  out << "};\n\n";
  out << "const size_t g_renutXenosShaderCacheEntryCount = " << outEntries.size() << ";\n";

  std::fprintf(stderr, "xenos_cache_unpack: wrote %zu shaders (%zu SPIR-V words) to %s\n", outEntries.size(),
               flatSpirv.size(), argv[2]);
  return 0;
}
