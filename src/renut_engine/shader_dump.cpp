#include <rex/ppc.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hash.h>
#include <rex/runtime.h>
#include "renut_logging.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

#if defined(_MSC_VER)
#include <stdlib.h>
#define RENUT_BSWAP32(x) _byteswap_ulong(x)
#else
#define RENUT_BSWAP32(x) __builtin_bswap32(x)
#endif

REXCVAR_DEFINE_BOOL(dump_guest_shaders, false, "Nuts&Bolts/Graphics",
	"Dump guest shader blobs to a 'shaders' folder next to the exe as they are "
	"created. Writes <vs|ps>_<hash>.bin (the whole pFunction) and "
	"<vs|ps>_<hash>.ucode (microcode only). Each unique shader is written once.");

namespace {

constexpr uint32_t kMaxBlobBytes = 1u << 20;
constexpr uint32_t kMinHeaderBytes = 12;

std::mutex g_dumpMutex;
std::unordered_set<uint64_t> g_dumped;

bool writeFile(const std::filesystem::path& path, const uint8_t* data, size_t size)
{
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	if (!file) {
		RNUT_ERROR("shader dump: could not open {}", path.string());
		return false;
	}
	file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
	if (!file) {
		RNUT_ERROR("shader dump: write failed for {}", path.string());
		return false;
	}
	return true;
}

void dumpShaderBlob(PPCRegister& r3, const char* tag)
{
	if (!REXCVAR_GET(dump_guest_shaders)) return;

	const uint32_t pFunction = r3.u32;
	if (!pFunction) return;

	const uint8_t* base = rex::Runtime::instance()->virtual_membase();
	const uint8_t* blob = base + pFunction;
	const uint32_t* words = reinterpret_cast<const uint32_t*>(blob);

	const uint32_t flags = RENUT_BSWAP32(words[0]);
	const uint32_t headerSize = RENUT_BSWAP32(words[1]);
	const uint32_t ucodeSize = RENUT_BSWAP32(words[2]);

	if (headerSize < kMinHeaderBytes || headerSize > kMaxBlobBytes ||
		ucodeSize == 0 || ucodeSize > kMaxBlobBytes) {
		RNUT_WARN("shader dump: implausible {} blob at {:#x} (header {}, ucode {}) - skipped",
			tag, pFunction, headerSize, ucodeSize);
		return;
	}

	const uint8_t* ucode = blob + headerSize;
	const size_t total = static_cast<size_t>(headerSize) + ucodeSize;

	const uint64_t hash = XXH3_64bits(blob, total);
	{
		std::lock_guard<std::mutex> lock(g_dumpMutex);
		if (!g_dumped.insert(hash).second) return;
	}

	const std::filesystem::path outDir =
		rex::filesystem::GetExecutableFolder() / "shaders";

	std::error_code ec;
	std::filesystem::create_directories(outDir, ec);
	if (ec) {
		RNUT_ERROR("shader dump: cannot create {} ({})", outDir.string(), ec.message());
		return;
	}

	char stem[32];
	std::snprintf(stem, sizeof(stem), "%s_%016llx", tag,
		static_cast<unsigned long long>(hash));

	if (!writeFile(outDir / (std::string(stem) + ".bin"), blob, total)) return;
	if (!writeFile(outDir / (std::string(stem) + ".ucode"), ucode, ucodeSize)) return;

	RNUT_INFO("shader dump: {} (flags {:#x}, header {} B, ucode {} B) from {:#x}",
		stem, flags, headerSize, ucodeSize, pFunction);
}

}  // namespace

void dumpCodeBytesOnce();

void dumpVertexShader_hook(PPCRegister& r3)
{
	dumpShaderBlob(r3, "vs");
	dumpCodeBytesOnce();
}

void dumpPixelShader_hook(PPCRegister& r3)
{
	dumpShaderBlob(r3, "ps");
}

void dumpVertexShaderSet_hook(PPCRegister& r3, PPCRegister& r4);
void dumpVertexDeclarationSet_hook(PPCRegister& r3, PPCRegister& r4);

namespace {

REXCVAR_DEFINE_BOOL(renut_dump_vertex_decl, false, "Nuts&Bolts/Graphics",
	"Dump real D3DVERTEXELEMENT9 arrays passed to CreateVertexDeclaration, for "
	"native-renderer Phase 1 shader-container reconstruction.");

std::mutex g_declMutex;
std::unordered_set<uint32_t> g_declLogged;

std::mutex g_vsMutex;
uint32_t g_lastBoundVertexShaderBlob = 0;
std::unordered_set<uint64_t> g_vertDeclDumped;

}  // namespace

void dumpVertexDeclaration_hook(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5,
	PPCRegister& r6, PPCRegister& r7, PPCRegister& r8, PPCRegister& r9, PPCRegister& r10)
{
	(void)r4; (void)r5; (void)r6; (void)r7; (void)r8; (void)r9; (void)r10;
	if (!REXCVAR_GET(renut_dump_vertex_decl)) return;
	dumpCodeBytesOnce();

	const uint32_t pElements = r3.u32;
	if (pElements < 0x10000000u || pElements >= 0x90000000u) {
		return;
	}

	{
		std::lock_guard<std::mutex> lock(g_declMutex);
		if (!g_declLogged.insert(pElements).second) return;
	}

	const uint8_t* base = rex::Runtime::instance()->virtual_membase();
	constexpr uint32_t kStride = 12;

	std::string line = "renut vertex decl:";
	uint32_t count = 0;
	for (; count < 32; ++count) {
		const uint8_t* e = base + pElements + count * kStride;
		const uint16_t streamField = (uint16_t(e[0]) << 8) | e[1];
		if (streamField == 0x00FFu || (streamField & 0xFF) == 0xFFu) {
			break;
		}
		const uint16_t offset = (uint16_t(e[2]) << 8) | e[3];
		const uint32_t type = (uint32_t(e[5]) << 16) | (uint32_t(e[6]) << 8) | e[7];
		const uint8_t usage = e[9];
		const uint8_t usageIndex = e[10];

		char entry[64];
		std::snprintf(entry, sizeof(entry), " off=%u type=%06x usage=%u usageIdx=%u",
			offset, type, usage, usageIndex);
		line += entry;
	}

	RNUT_INFO("{} (from {:#x}, {} elements)", line, pElements, count);
}

void dumpVertexDeclUsageForBoundShader(uint32_t pElements)
{
	if (pElements < 0x10000000u || pElements >= 0x90000000u) return;

	uint32_t pFunction;
	{
		std::lock_guard<std::mutex> lock(g_vsMutex);
		pFunction = g_lastBoundVertexShaderBlob;
	}
	if (!pFunction) return;

	const uint8_t* base = rex::Runtime::instance()->virtual_membase();

	const uint8_t* blob = base + pFunction;
	const uint32_t* words = reinterpret_cast<const uint32_t*>(blob);
	const uint32_t headerSize = RENUT_BSWAP32(words[1]);
	const uint32_t ucodeSize = RENUT_BSWAP32(words[2]);
	if (headerSize < kMinHeaderBytes || headerSize > kMaxBlobBytes ||
		ucodeSize == 0 || ucodeSize > kMaxBlobBytes) {
		return;
	}
	const uint8_t* ucode = blob + headerSize;
	const uint64_t ucodeHash = XXH3_64bits(ucode, ucodeSize);

	{
		std::lock_guard<std::mutex> lock(g_declMutex);
		if (!g_vertDeclDumped.insert(ucodeHash ^ (uint64_t(pElements) << 32)).second) return;
	}

	constexpr uint32_t kStride = 12;
	struct Element { uint16_t offset; uint32_t type; uint8_t usage; uint8_t usageIndex; };
	std::vector<Element> elements;
	for (uint32_t count = 0; count < 32; ++count) {
		const uint8_t* e = base + pElements + count * kStride;
		const uint16_t streamField = (uint16_t(e[0]) << 8) | e[1];
		if (streamField == 0x00FFu || (streamField & 0xFF) == 0xFFu) break;
		elements.push_back({
			uint16_t((uint16_t(e[2]) << 8) | e[3]),
			(uint32_t(e[5]) << 16) | (uint32_t(e[6]) << 8) | e[7],
			e[9],
			e[10],
		});
	}
	if (elements.empty()) return;

	const std::filesystem::path outDir =
		rex::filesystem::GetExecutableFolder() / "shaders_vertdecl";
	std::error_code ec;
	std::filesystem::create_directories(outDir, ec);
	if (ec) return;

	char stem[48];
	std::snprintf(stem, sizeof(stem), "vs_%016llx", static_cast<unsigned long long>(ucodeHash));
	std::ofstream file(outDir / (std::string(stem) + ".vertdecl.txt"), std::ios::trunc);
	if (!file) return;

	for (const Element& e : elements) {
		file << "offset=" << e.offset << " type=" << std::hex << e.type << std::dec
			<< " usage=" << int(e.usage) << " usageIndex=" << int(e.usageIndex) << "\n";
	}

	RNUT_INFO("vertex decl usage: vs_{:016x} <- decl {:#x} ({} elements)",
		ucodeHash, pElements, elements.size());
}

void dumpVertexShaderSet_hook(PPCRegister& r3, PPCRegister& r4)
{
	if (!REXCVAR_GET(renut_dump_vertex_decl)) return;
	static std::once_flag entered;
	std::call_once(entered, [&] {
		RNUT_INFO("renut SetVertexShader hook ENTERED: r3={:#x} r4={:#x}", r3.u32, r4.u32);
	});
	dumpCodeBytesOnce();
	std::lock_guard<std::mutex> lock(g_vsMutex);
	g_lastBoundVertexShaderBlob = r4.u32;
}

void dumpVertexDeclarationSet_hook(PPCRegister& r3, PPCRegister& r4)
{
	if (!REXCVAR_GET(renut_dump_vertex_decl)) return;
	static std::once_flag entered;
	std::call_once(entered, [&] {
		RNUT_INFO("renut SetVertexDeclaration hook ENTERED: r3={:#x} r4={:#x}", r3.u32, r4.u32);
	});
	dumpCodeBytesOnce();
	dumpVertexDeclUsageForBoundShader(r4.u32);
}

REXCVAR_DEFINE_BOOL(renut_dump_code, false, "Nuts&Bolts/Graphics",
	"One-shot dump of raw guest code bytes at known D3D9 function addresses, "
	"for offline PPC disassembly (native-renderer vertex-declaration work).");

void dumpCodeBytesOnce()
{
	static std::once_flag dumped;
	if (!REXCVAR_GET(renut_dump_code)) return;

	std::call_once(dumped, [] {
		const uint8_t* base = rex::Runtime::instance()->virtual_membase();
		const std::filesystem::path outDir = rex::filesystem::GetExecutableFolder() / "code_dump";
		std::error_code ec;
		std::filesystem::create_directories(outDir, ec);
		if (ec) {
			RNUT_ERROR("code dump: cannot create {} ({})", outDir.string(), ec.message());
			return;
		}

		struct Target {
			uint32_t address;
			const char* name;
		};
		static constexpr Target kTargets[] = {
			{0x8264EA90u, "CreateVertexDeclaration"},
			{0x8264E8B0u, "CreateVertexShader"},
			{0x8264E6A8u, "CreatePixelShader"},
			{0x8222A0A8u, "SetVertexShader"},
			{0x82205700u, "SetVertexDeclaration"},
		};
		constexpr size_t kDumpBytes = 512;

		for (const Target& t : kTargets) {
			std::ofstream file(outDir / (std::string(t.name) + ".bin"), std::ios::binary | std::ios::trunc);
			if (!file) {
				RNUT_ERROR("code dump: could not open output for {}", t.name);
				continue;
			}
			file.write(reinterpret_cast<const char*>(base + t.address),
				static_cast<std::streamsize>(kDumpBytes));
			RNUT_INFO("code dump: wrote {} bytes for {} @ {:#x} -> code_dump/{}.bin",
				kDumpBytes, t.name, t.address, t.name);
		}
	});
}
