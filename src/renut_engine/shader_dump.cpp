// Shader dumping.
//
// Hook point: the entry of the game's D3DDevice_CreateVertexShader (0x8264E8B0)
// and D3DDevice_CreatePixelShader (0x8264E6A8). D3D9 is statically linked into
// the XEX, so these are ordinary recompiled guest functions (renut_gpu_funcs.toml
// only renames them; the bodies still live in generated/renut_recomp.42.cpp) and
// a midasm hook at the first instruction fires normally with r3 = pFunction.
//
// Creation is the right chokepoint: every shader the game ever uses passes
// through it exactly once at load time, whereas SetVertexShader/SetPixelShader
// run thousands of times per frame for the same handful of shaders. Hooking here
// costs nothing per draw.
//
// pFunction is self-describing, so the whole shader is reachable from that one
// pointer with no extra state -- see the guest code at 0x8264E8B0:
//   [0] flags
//   [1] header size   (copied into the shader object)
//   [2] microcode size
//   header at pFunction, microcode at pFunction + [1].
// We dump [0 .. [1]+[2]) verbatim: the header carries the constant/interpolator
// declarations that the raw microcode alone does not, so the blob stays
// self-contained and re-parsable.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "rex_macros.h"

// Name = "Dump Shaders"
// Named renut_dump_shaders, not dump_shaders: the xenos GPU plugin already
// registers that name, and rex keeps the first registration and drops the
// second -- so a plain dump_shaders here silently bound to the plugin's cvar
// and this hook could never be switched on.
REXCVAR_DEFINE_BOOL(renut_dump_shaders, false, "Nuts&Bolts/Debug",
                    "Dumps every vertex/pixel shader the game creates to dumps/shaders/ as raw "
                    "microcode blobs. Each unique shader is written once.");

namespace {

// Guest memory is big-endian.
inline uint32_t ReadGuestBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// Sanity ceiling. A real Xenos shader blob is a few KB; anything past this means
// we were handed a bogus pointer and should not touch it.
constexpr uint32_t kMaxShaderBytes = 1u << 20;

std::mutex g_mutex;
std::unordered_set<uint64_t> g_seen;
bool g_dirReady = false;

uint64_t HashFNV1a(const uint8_t* data, size_t len) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= 0x100000001B3ull;
  }
  return h;
}

void DumpShader(PPCRegister& r3, const char* kind) {
  if (!REXCVAR_GET(renut_dump_shaders))
    return;
  const uint32_t guestAddr = r3.u32;
  if (!guestAddr)
    return;

  REX_PPC_MEMBASE_PTR(membase);
  const uint8_t* blob = membase + guestAddr;

  const uint32_t headerSize = ReadGuestBE32(blob + 4);
  const uint32_t ucodeSize = ReadGuestBE32(blob + 8);
  const uint64_t total = uint64_t(headerSize) + uint64_t(ucodeSize);
  if (total == 0 || total > kMaxShaderBytes) {
    REXKRNL_WARN("shader_dump: {} at {:#x} has implausible size {} -- skipped", kind, guestAddr,
                 total);
    return;
  }

  // Hash first: shaders are created once, but the game recreates them on a
  // device reset, and dedupe keeps that from rewriting the same files.
  const uint64_t hash = HashFNV1a(blob, size_t(total));

  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_seen.insert(hash).second)
    return;

  std::error_code ec;
  const std::filesystem::path dir = "dumps/shaders";
  if (!g_dirReady) {
    std::filesystem::create_directories(dir, ec);
    if (ec) {
      REXKRNL_WARN("shader_dump: cannot create {}: {}", dir.string(), ec.message());
      return;
    }
    g_dirReady = true;
  }

  char name[64];
  std::snprintf(name, sizeof(name), "%s_%016llx.bin", kind, static_cast<unsigned long long>(hash));

  const std::filesystem::path path = dir / name;
  FILE* f = std::fopen(path.string().c_str(), "wb");
  if (!f) {
    REXKRNL_WARN("shader_dump: cannot open {}", path.string());
    return;
  }
  std::fwrite(blob, 1, size_t(total), f);
  std::fclose(f);

  REXKRNL_INFO("shader_dump: wrote {} ({} header + {} microcode bytes)", path.string(), headerSize,
               ucodeSize);
}

}  // namespace

// Midasm hook: D3DDevice_CreateVertexShader entry (guest 0x8264E8B0), r3 = pFunction.
void dumpVertexShader_hook(PPCRegister& r3) { DumpShader(r3, "vs"); }

// Midasm hook: D3DDevice_CreatePixelShader entry (guest 0x8264E6A8), r3 = pFunction.
void dumpPixelShader_hook(PPCRegister& r3) { DumpShader(r3, "ps"); }
