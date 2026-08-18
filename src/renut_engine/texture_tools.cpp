// =============================================================================
// Guest texture dumping and replacement.
//
// Both features hang off a single midasm hook on D3DDevice_SetTexture
// (0x8222F410), which is the one place every texture the game actually draws
// with passes through, whatever its origin (CAFF bundle, XUI, procedural).
// The hook sits on the function's first instruction (`mflr r12`), so r5 still
// holds the incoming pTexture argument.
//
// A D3DBaseTexture on Xenon is seven header dwords followed by the six-dword
// GPU texture fetch constant:
//
//   +0x00 Common  +0x04 ReferenceCount  +0x08 Fence   +0x0C ReadFence
//   +0x10 Identifier  +0x14 BaseFlush   +0x18 MipFlush
//   +0x1C GPUTEXTURE_FETCH_CONSTANT[6]
//
// Confirmed against the decompiled SetTexture, which reads Format.dword[1] from
// 0x20(r5) and Format.dword[5] from 0x30(r5), and whose whole body is a
// masked copy of those six dwords into pDevice->m_Constants.Fetch[Sampler].
// That fetch constant is the same structure the host GPU backend consumes, so
// rex::graphics::xenos::xe_gpu_texture_fetch_t describes it exactly -- after a
// byteswap, since the guest holds it big-endian.
//
// Because SetTexture's only job is to propagate Format into the device's fetch
// slot, editing Format in place before the copy is all that replacement needs:
// point base_address at our own texel data and correct the size/pitch/tiling
// fields to match. No SDK code is touched and no D3D call is redirected.
//
// Two different guest mappings are in play, and mixing them up yields garbage
// rather than a crash. The texture object arrives as a virtual pointer and is
// read with TranslateVirtual; the base_address/mip_address inside its fetch
// constant are *physical* and must go through TranslatePhysical, which is a
// separate host base entirely (virtual is around 0x100000000, physical around
// 0x200000000) and masks the address to 0x1FFFFFFF.
//
// Dumped textures are untiled but otherwise bit-exact -- native format, native
// mips, no decode -- so a dump fed straight back in as a replacement is a
// no-op. Layout and tiled addressing come from the SDK's texture_util rather
// than being reimplemented here; it already handles mips, packed mip tails and
// the 256-byte linear row alignment that hand-rolled untilers get wrong.
//
// A texture is captured only after its memory has held still across a few
// binds and is not all zeros. Static textures pass that immediately; it is
// there to keep render targets and not-yet-written pages out of the dump.
//
// Both features are off by default. SetTexture is a per-draw path, so when the
// cvars are off this costs one bool test; when they are on, the per-bind cost
// is the sparse probe, and the full untile-and-write happens at most once per
// texture.
// =============================================================================

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include "renut_logging.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_MSC_VER)
#include <stdlib.h>
#define RENUT_BSWAP32(x) _byteswap_ulong(x)
#define RENUT_BSWAP16(x) _byteswap_ushort(x)
#else
#define RENUT_BSWAP32(x) __builtin_bswap32(x)
#define RENUT_BSWAP16(x) __builtin_bswap16(x)
#endif

REXCVAR_DEFINE_BOOL(dump_textures, false, "Nuts&Bolts/Graphics",
	"Dump every texture the game binds to a 'textures/dump' folder next to the "
	"exe, as untiled .dds in the texture's native format with all its mips. "
	"Surfaces whose contents change every frame, such as render targets, are "
	"skipped. The file is named after the content hash that texture "
	"replacement matches on.");

REXCVAR_DEFINE_BOOL(dump_textures_raw, false, "Nuts&Bolts/Graphics",
	"Alongside each dumped .dds, write a .raw holding the untouched guest bytes "
	"it was decoded from (the base level's extent, still tiled and byte-swapped) "
	"and record the fetch constant in _index.csv. Diagnostic aid for textures "
	"that come out wrong; costs disk, so leave it off for normal dumping.");

REXCVAR_DEFINE_BOOL(replace_textures, false, "Nuts&Bolts/Graphics",
	"Replace textures at bind time from the packs named by texture_packs. A "
	"file named <hash>.dds overrides the texture whose dump has that hash. The "
	"replacement must use the same format as the original but may be any size "
	"and supply its own mips.");

REXCVAR_DEFINE_STRING(texture_packs, "replace", "Nuts&Bolts/Graphics",
	"Which texture replacement packs to load, as a ';'-separated list applied "
	"in order -- when two packs provide the same texture, the one listed later "
	"wins. Each entry is either a folder name under 'textures' next to the exe "
	"(the default, 'replace', means textures/replace) or an absolute path. "
	"Changing this reloads the packs; textures already swapped this session "
	"keep the pack they were swapped with until a restart.");

namespace {

namespace xenos = rex::graphics::xenos;
namespace texture_util = rex::graphics::texture_util;
using rex::graphics::FormatInfo;

// The fetch constant lives past the seven-dword D3DBaseTexture header.
constexpr uint32_t kFetchConstantOffset = 0x1C;

// Guest texture memory is addressed in 4 KB pages by the fetch constant, so
// every base/mip allocation has to start on one.
constexpr uint32_t kPageSize = 1u << 12;

// A sanity ceiling on how much we will read out of guest memory for one
// texture. The largest thing this game binds is a handful of MB; anything
// past this means we misread the fetch constant.
constexpr uint32_t kMaxTextureBytes = 64u << 20;

// The console's physical memory, which is also what TranslatePhysical masks
// addresses down to. Reads are checked against it so a bad fetch constant
// produces a skipped texture rather than an access violation.
constexpr uint32_t kPhysicalMemoryBytes = 0x20000000;

// The fetch constant's addresses are not what the GPU ends up reading.
// D3DDevice_SetTexture rewrites them as it copies the texture's fetch constant
// into the device's, and the dumper has to reproduce that or it reads the
// wrong memory:
//
//   v13 = dword & 0x1FFFFFFF;              // strip the 0xE0000000 mirror
//   v14 = ((dword >> 20) + 512) & 0x1000;  // set iff address >= 0xE0000000
//   result = v13 + v14;                    // +0x1000 for that region
//
// That is the 360's documented quirk for the 0xE0000000 physical mirror: it
// aliases physical memory with a one-page offset. The game stores most texture
// addresses there, and dword_5 (mip_address) gets the identical treatment.
//
// Skipping the +0x1000 reads every affected texture exactly one 4 KB page
// early, which shifts the whole surface and drops its last page.
uint32_t resolveGpuAddress(uint32_t guestAddress)
{
	uint32_t physical = guestAddress & (kPhysicalMemoryBytes - 1);
	if (guestAddress >= 0xE0000000) physical += kPageSize;
	return physical;
}

// Takes an address already put through resolveGpuAddress.
bool physicalRangeIsSane(uint32_t physicalAddress, uint32_t size)
{
	return size != 0 && size <= kMaxTextureBytes &&
	       uint64_t(physicalAddress) + size <= kPhysicalMemoryBytes;
}

// -----------------------------------------------------------------------------
// Format table
//
// Maps the Xenos storage formats this game actually uses onto their DDS
// equivalents. Only storage matters here: the fetch constant's swizzle and
// sign fields affect how a shader reads the texture, not how it is stored, so
// they are deliberately not baked into the dump. A dump is the bytes as the
// GPU would fetch them, just untiled.
// -----------------------------------------------------------------------------

constexpr uint32_t kDdpfAlphaPixels = 0x1;
constexpr uint32_t kDdpfFourCC = 0x4;
constexpr uint32_t kDdpfRgb = 0x40;
constexpr uint32_t kDdpfLuminance = 0x20000;

constexpr uint32_t makeFourCC(char a, char b, char c, char d)
{
	return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) |
	       (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
}

struct FormatMapping {
	xenos::TextureFormat format;
	uint32_t fourCC;  // 0 for uncompressed formats
	uint32_t pfFlags;
	uint32_t rgbBitCount;
	uint32_t rMask, gMask, bMask, aMask;
};

constexpr FormatMapping kFormatMappings[] = {
	{xenos::TextureFormat::k_DXT1,    makeFourCC('D','X','T','1'), kDdpfFourCC, 0, 0, 0, 0, 0},
	{xenos::TextureFormat::k_DXT2_3,  makeFourCC('D','X','T','3'), kDdpfFourCC, 0, 0, 0, 0, 0},
	{xenos::TextureFormat::k_DXT4_5,  makeFourCC('D','X','T','5'), kDdpfFourCC, 0, 0, 0, 0, 0},
	// DXN is two BC4 blocks, which is exactly ATI2/BC5.
	{xenos::TextureFormat::k_DXN,     makeFourCC('A','T','I','2'), kDdpfFourCC, 0, 0, 0, 0, 0},
	{xenos::TextureFormat::k_DXT5A,   makeFourCC('A','T','I','1'), kDdpfFourCC, 0, 0, 0, 0, 0},
	{xenos::TextureFormat::k_8_8_8_8, 0, kDdpfRgb | kDdpfAlphaPixels, 32,
	 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000},
	{xenos::TextureFormat::k_5_6_5,   0, kDdpfRgb, 16, 0xF800, 0x07E0, 0x001F, 0},
	{xenos::TextureFormat::k_1_5_5_5, 0, kDdpfRgb | kDdpfAlphaPixels, 16,
	 0x7C00, 0x03E0, 0x001F, 0x8000},
	{xenos::TextureFormat::k_4_4_4_4, 0, kDdpfRgb | kDdpfAlphaPixels, 16,
	 0x0F00, 0x00F0, 0x000F, 0xF000},
	{xenos::TextureFormat::k_8,       0, kDdpfLuminance, 8, 0xFF, 0, 0, 0},
	{xenos::TextureFormat::k_8_8,     0, kDdpfLuminance | kDdpfAlphaPixels, 16,
	 0x00FF, 0, 0, 0xFF00},
};

const FormatMapping* findMappingByFormat(xenos::TextureFormat format)
{
	for (const FormatMapping& m : kFormatMappings) {
		if (m.format == format) return &m;
	}
	return nullptr;
}

// -----------------------------------------------------------------------------
// DDS
// -----------------------------------------------------------------------------

constexpr uint32_t kDdsMagic = makeFourCC('D','D','S',' ');

constexpr uint32_t kDdsdCaps = 0x1;
constexpr uint32_t kDdsdHeight = 0x2;
constexpr uint32_t kDdsdWidth = 0x4;
constexpr uint32_t kDdsdPitch = 0x8;
constexpr uint32_t kDdsdPixelFormat = 0x1000;
constexpr uint32_t kDdsdMipMapCount = 0x20000;
constexpr uint32_t kDdsdLinearSize = 0x80000;

constexpr uint32_t kDdsCapsComplex = 0x8;
constexpr uint32_t kDdsCapsMipMap = 0x400000;
constexpr uint32_t kDdsCapsTexture = 0x1000;

#pragma pack(push, 1)
struct DdsPixelFormat {
	uint32_t size;
	uint32_t flags;
	uint32_t fourCC;
	uint32_t rgbBitCount;
	uint32_t rBitMask;
	uint32_t gBitMask;
	uint32_t bBitMask;
	uint32_t aBitMask;
};

struct DdsHeader {
	uint32_t size;
	uint32_t flags;
	uint32_t height;
	uint32_t width;
	uint32_t pitchOrLinearSize;
	uint32_t depth;
	uint32_t mipMapCount;
	uint32_t reserved1[11];
	DdsPixelFormat ddspf;
	uint32_t caps;
	uint32_t caps2;
	uint32_t caps3;
	uint32_t caps4;
	uint32_t reserved2;
};
#pragma pack(pop)

static_assert(sizeof(DdsPixelFormat) == 32, "DDS_PIXELFORMAT must be 32 bytes");
static_assert(sizeof(DdsHeader) == 124, "DDS_HEADER must be 124 bytes");

// One mip level, tightly packed with no row padding.
struct Level {
	uint32_t width;
	uint32_t height;
	std::vector<uint8_t> data;
};

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

std::mutex g_mutex;

// Not every texture has its full-resolution level in memory. When a fetch
// constant's mip address equals its base address, that single buffer holds the
// mip chain starting at level 1 and the base level is simply not resident, so
// the dump's top level is level 1 at half the nominal size. Reading such a
// texture as if the base were there smears the mip chain across a full-size
// surface and runs off into unrelated memory.
//
// Textures are captured only once their memory has held still for a couple of
// binds, and never while the sampled bytes are all zero. Static textures pass
// this immediately; it exists to keep two things out of the dump folder:
// render targets and other surfaces whose contents change every frame, and
// pages the game has reserved but not yet written.
constexpr uint32_t kMinStableBinds = 3;

// Dumping and replacing are tracked separately so that turning replacement on
// partway through a session still works: a texture already written to disk
// must not count as "handled" for the replace path that has not run yet.
struct TextureWatch {
	uint64_t probe = 0;
	uint32_t stableBinds = 0;
	bool dumped = false;
	bool replaceChecked = false;
};

// Keyed on the guest texture object paired with its base address, so a
// recycled D3DTexture object pointing at fresh data is watched anew.
std::unordered_map<uint64_t, TextureWatch> g_watched;

// Hashes already written to disk this launch.
std::unordered_set<uint64_t> g_dumped;

// Replacements we have already uploaded, so a texture rebound every frame is
// only patched once. Maps guest texture object -> the hash it was replaced
// with.
std::unordered_map<uint32_t, uint64_t> g_patched;

// Merged view of every configured pack, and the texture_packs value it was
// built from so a change to that cvar can be noticed and reloaded.
std::unordered_map<uint64_t, std::filesystem::path> g_replacements;
std::string g_scannedPacks;
bool g_replacementsScanned = false;

uint32_t alignUp(uint32_t value, uint32_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

// A cheap stand-in for the texture's contents, used to tell whether its memory
// is still being written. Sparse enough to run on every bind of every bound
// texture, dense enough to notice an upload landing.
//
// allZero is reported separately: memory that has not been written at all is
// perfectly "stable", so without this an unloaded texture would satisfy the
// residency gate and get captured as a black image.
uint64_t probeTexels(const uint8_t* data, uint32_t size, bool& allZero)
{
	constexpr uint32_t kProbeCount = 64;
	constexpr uint32_t kProbeBytes = 64;

	allZero = true;

	XXH3_state_t state;
	XXH3_64bits_reset(&state);
	XXH3_64bits_update(&state, &size, sizeof(size));

	auto consume = [&](const uint8_t* p, uint32_t n) {
		XXH3_64bits_update(&state, p, n);
		if (allZero) {
			for (uint32_t i = 0; i < n; ++i) {
				if (p[i]) {
					allZero = false;
					break;
				}
			}
		}
	};

	if (size <= kProbeCount * kProbeBytes) {
		consume(data, size);
	} else {
		// stride > kProbeBytes here, so the probes never run off the end.
		const uint32_t stride = size / kProbeCount;
		for (uint32_t i = 0; i < kProbeCount; ++i) {
			consume(data + size_t(i) * stride, kProbeBytes);
		}
	}

	return XXH3_64bits_digest(&state);
}

uint32_t log2Floor(uint32_t value)
{
	uint32_t result = 0;
	while (value > 1) {
		value >>= 1;
		++result;
	}
	return result;
}

std::filesystem::path textureFolder(const char* leaf)
{
	return rex::filesystem::GetExecutableFolder() / "textures" / leaf;
}

// Undo the fetch constant's endian swap over one row of freshly untiled data.
// The swap is defined on the memory stream in fixed-size units rather than per
// block, so it is applied to whole rows after the copy; within a row, source
// bytes are contiguous in both linear and tiled layouts.
void applyEndianSwap(uint8_t* data, size_t size, xenos::Endian endianness)
{
	switch (endianness) {
		case xenos::Endian::k8in16: {
			for (size_t i = 0; i + 2 <= size; i += 2) {
				std::swap(data[i], data[i + 1]);
			}
			break;
		}
		case xenos::Endian::k8in32: {
			for (size_t i = 0; i + 4 <= size; i += 4) {
				std::swap(data[i], data[i + 3]);
				std::swap(data[i + 1], data[i + 2]);
			}
			break;
		}
		case xenos::Endian::k16in32: {
			for (size_t i = 0; i + 4 <= size; i += 4) {
				std::swap(data[i], data[i + 2]);
				std::swap(data[i + 1], data[i + 3]);
			}
			break;
		}
		case xenos::Endian::kNone:
		default:
			break;
	}
}

// Copy one mip level out of guest memory into a tightly packed linear buffer,
// undoing tiling and the endian swap.
bool readLevel(const uint8_t* src, const texture_util::TextureGuestLayout::Level& layout,
               const FormatInfo* formatInfo, bool tiled, xenos::Endian endianness,
               uint32_t levelWidth, uint32_t levelHeight, Level& out)
{
	const uint32_t bytesPerBlock = formatInfo->bytes_per_block();
	const uint32_t bpbLog2 = log2Floor(bytesPerBlock);

	const uint32_t blocksWide =
		std::max(1u, (levelWidth + formatInfo->block_width - 1) / formatInfo->block_width);
	const uint32_t blocksHigh =
		std::max(1u, (levelHeight + formatInfo->block_height - 1) / formatInfo->block_height);

	const uint64_t total = uint64_t(blocksWide) * blocksHigh * bytesPerBlock;
	if (total == 0 || total > kMaxTextureBytes) return false;

	// Tiled addressing works in blocks, and each level carries its own pitch.
	const uint32_t pitchBlocks = layout.row_pitch_bytes >> bpbLog2;
	if (tiled && pitchBlocks == 0) return false;

	out.width = levelWidth;
	out.height = levelHeight;
	out.data.resize(static_cast<size_t>(total));

	uint8_t* dst = out.data.data();

	for (uint32_t y = 0; y < blocksHigh; ++y) {
		uint8_t* dstRow = dst + size_t(y) * blocksWide * bytesPerBlock;
		if (tiled) {
			for (uint32_t x = 0; x < blocksWide; ++x) {
				const int32_t srcOffset = texture_util::GetTiledOffset2D(
					int32_t(x), int32_t(y), pitchBlocks, bpbLog2);
				if (srcOffset < 0) return false;
				std::memcpy(dstRow + size_t(x) * bytesPerBlock, src + srcOffset, bytesPerBlock);
			}
		} else {
			std::memcpy(dstRow, src + size_t(y) * layout.row_pitch_bytes,
				size_t(blocksWide) * bytesPerBlock);
		}
		applyEndianSwap(dstRow, size_t(blocksWide) * bytesPerBlock, endianness);
	}

	return true;
}

// Records the fetch constant every dump came from, next to the dumps. Lets a
// bad dump be traced back to the exact guest state that produced it instead of
// being reverse-engineered from the pixels.
void appendIndexRow(const std::filesystem::path& dir, const char* stem,
                    const xenos::xe_gpu_texture_fetch_t& fetch, const uint32_t* rawWords,
                    const FormatInfo* formatInfo, uint32_t baseAddress, uint32_t mipAddress,
                    const texture_util::TextureGuestLayout& layout, uint32_t firstLevel,
                    size_t levelsWritten)
{
	// layout.base is only filled in when the base level is resident, so report
	// whichever level the dump actually starts from.
	const texture_util::TextureGuestLayout::Level& firstLayout =
		firstLevel == 0 ? layout.base : layout.mips[firstLevel];

	const std::filesystem::path path = dir / "_index.csv";

	std::error_code ec;
	const bool needHeader = !std::filesystem::exists(path, ec);

	std::ofstream file(path, std::ios::app);
	if (!file) return;

	if (needHeader) {
		file << "hash,format,width,height,tiled,pitch_div32,endian,swizzle,"
		        "fetch_base,fetch_mip,resolved_base,resolved_mip,"
		        "mip_min,mip_max,packed_mips,packed_level,dimension,stacked,request_size,"
		        "first_level,first_row_pitch,first_extent,mips_total_extent,levels_written,"
		        "dw0,dw1,dw2,dw3,dw4,dw5\n";
	}

	file << stem << ',' << formatInfo->name << ',' << (fetch.size_2d.width + 1) << ','
	     << (fetch.size_2d.height + 1) << ',' << fetch.tiled << ',' << fetch.pitch << ','
	     << uint32_t(fetch.endianness) << ',' << fetch.swizzle << ','
	     << std::hex << "0x" << (fetch.base_address << 12) << ",0x" << (fetch.mip_address << 12)
	     << ",0x" << baseAddress << ",0x" << mipAddress << std::dec << ','
	     << fetch.mip_min_level << ',' << fetch.mip_max_level << ',' << fetch.packed_mips << ','
	     << int64_t(layout.packed_level == UINT32_MAX ? -1 : int32_t(layout.packed_level)) << ','
	     << uint32_t(fetch.dimension) << ',' << fetch.stacked << ',' << fetch.request_size << ','
	     << firstLevel << ',' << firstLayout.row_pitch_bytes << ','
	     << firstLayout.level_data_extent_bytes << ','
	     << layout.mips_total_extent_bytes << ',' << levelsWritten << std::hex;

	for (int i = 0; i < 6; ++i) file << ",0x" << rawWords[i];
	file << std::dec << '\n';
}

bool writeDds(const std::filesystem::path& path, const FormatMapping& mapping,
              const FormatInfo* formatInfo, const std::vector<Level>& levels)
{
	if (levels.empty()) return false;

	const bool compressed = mapping.fourCC != 0;
	const uint32_t bytesPerBlock = formatInfo->bytes_per_block();
	const uint32_t blocksWide =
		std::max(1u, (levels[0].width + formatInfo->block_width - 1) / formatInfo->block_width);

	DdsHeader header{};
	header.size = sizeof(DdsHeader);
	header.flags = kDdsdCaps | kDdsdHeight | kDdsdWidth | kDdsdPixelFormat |
	               (compressed ? kDdsdLinearSize : kDdsdPitch);
	header.height = levels[0].height;
	header.width = levels[0].width;
	header.pitchOrLinearSize = compressed
		? static_cast<uint32_t>(levels[0].data.size())
		: blocksWide * bytesPerBlock;
	header.depth = 0;
	header.mipMapCount = static_cast<uint32_t>(levels.size());
	header.caps = kDdsCapsTexture;

	if (levels.size() > 1) {
		header.flags |= kDdsdMipMapCount;
		header.caps |= kDdsCapsComplex | kDdsCapsMipMap;
	}

	header.ddspf.size = sizeof(DdsPixelFormat);
	header.ddspf.flags = mapping.pfFlags;
	header.ddspf.fourCC = mapping.fourCC;
	header.ddspf.rgbBitCount = mapping.rgbBitCount;
	header.ddspf.rBitMask = mapping.rMask;
	header.ddspf.gBitMask = mapping.gMask;
	header.ddspf.bBitMask = mapping.bMask;
	header.ddspf.aBitMask = mapping.aMask;

	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	if (!file) {
		RNUT_ERROR("texture dump: could not open {}", path.string());
		return false;
	}

	const uint32_t magic = kDdsMagic;
	file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
	file.write(reinterpret_cast<const char*>(&header), sizeof(header));
	for (const Level& level : levels) {
		file.write(reinterpret_cast<const char*>(level.data.data()),
			static_cast<std::streamsize>(level.data.size()));
	}

	if (!file) {
		RNUT_ERROR("texture dump: write failed for {}", path.string());
		return false;
	}
	return true;
}

// Read a DDS back off disk into the same Level form the dumper produces.
bool readDds(const std::filesystem::path& path, xenos::TextureFormat& formatOut,
             const FormatInfo*& formatInfoOut, std::vector<Level>& levelsOut)
{
	std::ifstream file(path, std::ios::binary);
	if (!file) return false;

	uint32_t magic = 0;
	DdsHeader header{};
	file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
	file.read(reinterpret_cast<char*>(&header), sizeof(header));
	if (!file || magic != kDdsMagic || header.size != sizeof(DdsHeader)) {
		RNUT_WARN("texture replace: {} is not a DDS file", path.string());
		return false;
	}

	// Match on FourCC for compressed formats, and on bit depth plus channel
	// masks for uncompressed ones, so the file maps back onto exactly one
	// Xenos storage format.
	const FormatMapping* mapping = nullptr;
	for (const FormatMapping& m : kFormatMappings) {
		if (m.fourCC != 0) {
			if ((header.ddspf.flags & kDdpfFourCC) && header.ddspf.fourCC == m.fourCC) {
				mapping = &m;
				break;
			}
		} else if (!(header.ddspf.flags & kDdpfFourCC) &&
		           header.ddspf.rgbBitCount == m.rgbBitCount &&
		           header.ddspf.rBitMask == m.rMask && header.ddspf.gBitMask == m.gMask &&
		           header.ddspf.bBitMask == m.bMask && header.ddspf.aBitMask == m.aMask) {
			mapping = &m;
			break;
		}
	}

	if (!mapping) {
		RNUT_WARN("texture replace: {} uses a pixel format with no Xenos equivalent",
			path.string());
		return false;
	}

	const FormatInfo* formatInfo = FormatInfo::Get(mapping->format);
	if (!formatInfo) return false;

	const uint32_t mipCount = std::max(1u, header.mipMapCount);
	if (mipCount > xenos::kTextureMaxMips) {
		RNUT_WARN("texture replace: {} has {} mips, more than the {} the GPU can address",
			path.string(), mipCount, xenos::kTextureMaxMips);
		return false;
	}

	levelsOut.clear();
	levelsOut.reserve(mipCount);

	for (uint32_t level = 0; level < mipCount; ++level) {
		const uint32_t width = std::max(1u, header.width >> level);
		const uint32_t height = std::max(1u, header.height >> level);
		const uint32_t blocksWide =
			std::max(1u, (width + formatInfo->block_width - 1) / formatInfo->block_width);
		const uint32_t blocksHigh =
			std::max(1u, (height + formatInfo->block_height - 1) / formatInfo->block_height);

		Level out;
		out.width = width;
		out.height = height;
		out.data.resize(size_t(blocksWide) * blocksHigh * formatInfo->bytes_per_block());
		file.read(reinterpret_cast<char*>(out.data.data()),
			static_cast<std::streamsize>(out.data.size()));
		if (!file) {
			RNUT_WARN("texture replace: {} ends early at mip {}", path.string(), level);
			return false;
		}
		levelsOut.push_back(std::move(out));
	}

	formatOut = mapping->format;
	formatInfoOut = formatInfo;
	return true;
}

// Splits the texture_packs list on ';'. Semicolon rather than comma because it
// cannot occur in a Windows path, so absolute paths can be listed safely.
std::vector<std::string> splitPackList(const std::string& list)
{
	std::vector<std::string> out;
	for (size_t start = 0; start <= list.size();) {
		const size_t sep = list.find(';', start);
		const size_t stop = sep == std::string::npos ? list.size() : sep;

		const std::string entry = list.substr(start, stop - start);
		const size_t b = entry.find_first_not_of(" \t\"");
		const size_t e = entry.find_last_not_of(" \t\"");
		if (b != std::string::npos) out.push_back(entry.substr(b, e - b + 1));

		if (sep == std::string::npos) break;
		start = sep + 1;
	}
	return out;
}

// A bare name is a folder under 'textures' next to the exe; anything absolute
// is taken as given, so packs can live outside the build directory.
std::filesystem::path resolvePackPath(const std::string& entry)
{
	const std::filesystem::path path(entry);
	return path.is_absolute() ? path : textureFolder(entry.c_str());
}

// Rebuilds the merged replacement table. Must be called with g_mutex held.
void scanReplacements(const std::string& packList)
{
	g_replacements.clear();
	g_scannedPacks = packList;
	g_replacementsScanned = true;

	// Any earlier "this texture has no replacement" decision was made against
	// the previous pack list, so let every texture be looked at again. Already
	// swapped textures are held back by g_patched, not by this.
	for (auto& entry : g_watched) {
		entry.second.replaceChecked = false;
	}

	const std::vector<std::string> packs = splitPackList(packList);
	if (packs.empty()) {
		RNUT_WARN("texture replace: texture_packs is empty, nothing will be replaced");
		return;
	}

	for (const std::string& pack : packs) {
		const std::filesystem::path dir = resolvePackPath(pack);

		std::error_code ec;
		if (!std::filesystem::is_directory(dir, ec)) {
			RNUT_WARN("texture replace: pack '{}' not found at {}", pack, dir.string());
			continue;
		}

		size_t loaded = 0;
		size_t overridden = 0;
		for (const auto& file : std::filesystem::directory_iterator(dir, ec)) {
			if (ec) break;
			if (!file.is_regular_file()) continue;

			const std::filesystem::path& path = file.path();
			std::string ext = path.extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (ext != ".dds") continue;

			// The stem is the content hash the dumper named the original with.
			const std::string stem = path.stem().string();
			char* end = nullptr;
			const uint64_t hash = stem.size() == 16 ? std::strtoull(stem.c_str(), &end, 16) : 0;
			if (stem.size() != 16 || end != stem.c_str() + stem.size()) {
				RNUT_WARN("texture replace: skipping {}/{} (name is not a 16-digit texture hash)",
					pack, path.filename().string());
				continue;
			}

			// Later packs win, so a pack listed after another can override it.
			if (!g_replacements.insert_or_assign(hash, path).second) ++overridden;
			++loaded;
		}

		if (overridden) {
			RNUT_INFO("texture replace: pack '{}': {} texture(s), {} overriding an earlier pack",
				pack, loaded, overridden);
		} else {
			RNUT_INFO("texture replace: pack '{}': {} texture(s)", pack, loaded);
		}
	}

	RNUT_INFO("texture replace: {} replacement(s) from {} pack(s)",
		g_replacements.size(), packs.size());
}

// Write a replacement's texel data into freshly allocated guest physical
// memory and repoint the texture's fetch constant at it.
//
// The copy is stored untiled with kNone endianness, which is a layout the GPU
// reads natively and costs nothing to produce -- there is no reason to retile
// data we are generating ourselves. Everything the fetch constant says about
// the texture's shape has to be corrected to match.
bool applyReplacement(uint32_t* fetchWords, xenos::xe_gpu_texture_fetch_t& fetch,
                      const FormatInfo* formatInfo, const std::vector<Level>& levels)
{
	const uint32_t bytesPerBlock = formatInfo->bytes_per_block();
	const uint32_t width = levels[0].width;
	const uint32_t height = levels[0].height;
	const uint32_t maxLevel = static_cast<uint32_t>(levels.size()) - 1;

	// Linear textures need every block row 256-byte aligned, and the fetch
	// constant only stores pitch in units of 32 texels, so widen to the first
	// pitch that satisfies both.
	uint32_t pitchTexels = alignUp(std::max(width, 32u), 32u);
	while (((pitchTexels / formatInfo->block_width) * bytesPerBlock) %
	       xenos::kTextureLinearRowAlignmentBytes != 0) {
		pitchTexels += 32;
	}
	const uint32_t pitchDiv32 = pitchTexels >> 5;
	if (pitchDiv32 == 0 || pitchDiv32 >= (1u << 9)) return false;

	const texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
		xenos::DataDimension::k2DOrStacked, pitchDiv32, width, height, 1,
		/*is_tiled=*/false, fetch.format, /*has_packed_levels=*/false,
		/*has_base=*/true, maxLevel);

	const uint32_t baseBytes = alignUp(layout.base.level_data_extent_bytes, kPageSize);
	const uint32_t mipBytes =
		maxLevel > 0 ? alignUp(layout.mips_total_extent_bytes, kPageSize) : 0;
	const uint64_t totalBytes = uint64_t(baseBytes) + mipBytes;
	if (totalBytes == 0 || totalBytes > kMaxTextureBytes) return false;

	// Physical, because that is the address space the fetch constant's
	// base_address/mip_address fields index.
	rex::memory::Memory* memory = rex::Runtime::instance()->memory();
	const uint32_t allocation = memory->SystemHeapAlloc(
		static_cast<uint32_t>(totalBytes), kPageSize, rex::memory::kSystemHeapPhysical);
	if (!allocation) {
		RNUT_ERROR("texture replace: could not allocate {} bytes of guest memory", totalBytes);
		return false;
	}

	// SystemHeapAlloc hands back a guest virtual address in one of the physical
	// mirrors; the fetch constant wants the physical address behind it.
	const uint32_t physicalAddress = allocation & 0x1FFFFFFF;

	uint8_t* destBase = memory->TranslatePhysical<uint8_t*>(physicalAddress);
	std::memset(destBase, 0, static_cast<size_t>(totalBytes));

	// Base level, then each mip at the offset the layout puts it.
	for (uint32_t level = 0; level <= maxLevel; ++level) {
		const texture_util::TextureGuestLayout::Level& levelLayout =
			level == 0 ? layout.base : layout.mips[level];
		const Level& source = levels[level];

		const uint32_t blocksWide =
			std::max(1u, (source.width + formatInfo->block_width - 1) / formatInfo->block_width);
		const uint32_t blocksHigh =
			std::max(1u, (source.height + formatInfo->block_height - 1) / formatInfo->block_height);
		const uint32_t rowBytes = blocksWide * bytesPerBlock;

		uint8_t* levelData =
			level == 0 ? destBase : destBase + baseBytes + layout.mip_offsets_bytes[level];

		for (uint32_t y = 0; y < blocksHigh; ++y) {
			std::memcpy(levelData + size_t(y) * levelLayout.row_pitch_bytes,
				source.data.data() + size_t(y) * rowBytes, rowBytes);
		}
	}

	fetch.base_address = physicalAddress >> 12;
	fetch.mip_address = maxLevel > 0 ? (physicalAddress + baseBytes) >> 12 : 0;
	fetch.pitch = pitchDiv32;
	fetch.tiled = 0;
	fetch.packed_mips = 0;
	fetch.endianness = xenos::Endian::kNone;
	fetch.size_2d.width = width - 1;
	fetch.size_2d.height = height - 1;
	fetch.mip_min_level = 0;
	fetch.mip_max_level = maxLevel;

	// Back into the guest's big-endian copy, which SetTexture is about to
	// propagate into the device fetch slot.
	uint32_t words[6];
	std::memcpy(words, &fetch, sizeof(words));
	for (int i = 0; i < 6; ++i) {
		fetchWords[i] = RENUT_BSWAP32(words[i]);
	}

	return true;
}

void handleTexture(uint32_t pTexture)
{
	const bool dumping = REXCVAR_GET(dump_textures);
	const bool replacing = REXCVAR_GET(replace_textures);
	if (!dumping && !replacing) return;
	if (!pTexture) return;

	rex::memory::Memory* memory = rex::Runtime::instance()->memory();

	// pTexture came in as a guest virtual pointer, so the texture object -- and
	// the fetch constant inside it -- is read through the virtual mapping. The
	// texel data it points at is physical, and that is a different mapping
	// entirely (see the TranslatePhysical calls below).
	uint32_t* fetchWords =
		memory->TranslateVirtual<uint32_t*>(pTexture + kFetchConstantOffset);

	uint32_t words[6];
	for (int i = 0; i < 6; ++i) {
		words[i] = RENUT_BSWAP32(fetchWords[i]);
	}

	xenos::xe_gpu_texture_fetch_t fetch{};
	std::memcpy(&fetch, words, sizeof(words));

	if (fetch.type != xenos::FetchConstantType::kTexture) return;
	if (!fetch.base_address) return;

	// 2D only. Cube and volume textures need per-slice handling that no
	// texture in this game has asked for yet; log once rather than dumping
	// something subtly wrong.
	if (fetch.dimension != xenos::DataDimension::k2DOrStacked) return;
	// k2DOrStacked covers array textures too; those store slices within each
	// level, which the flat 2D path below would silently truncate.
	if (fetch.stacked) return;

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_patched.count(pTexture)) return;
	}

	const xenos::TextureFormat format = fetch.format;
	const FormatInfo* formatInfo = FormatInfo::Get(format);
	if (!formatInfo || formatInfo->bytes_per_block() == 0) return;

	const FormatMapping* mapping = findMappingByFormat(format);
	if (!mapping) {
		RNUT_WARN("texture: {}x{} format {} has no DDS equivalent - skipped",
			fetch.size_2d.width + 1, fetch.size_2d.height + 1, formatInfo->name);
		return;
	}

	const uint32_t width = fetch.size_2d.width + 1;
	const uint32_t height = fetch.size_2d.height + 1;
	const uint32_t maxLevel = fetch.mip_max_level;

	// A texture whose mip address is its base address does not have its
	// full-resolution level resident: that one buffer holds the mip chain
	// starting at level 1, which is why mip_offsets_bytes[1] is 0. Reading it
	// as the base level smears the whole chain across a full-size surface and
	// then runs off into unrelated memory.
	const bool hasBase =
		!(maxLevel != 0 && fetch.mip_address != 0 && fetch.mip_address == fetch.base_address);
	const uint32_t firstLevel = hasBase ? 0 : 1;

	const texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
		xenos::DataDimension::k2DOrStacked, fetch.pitch, width, height, 1, fetch.tiled != 0, format,
		fetch.packed_mips != 0, hasBase, maxLevel);

	const uint32_t baseAddress = resolveGpuAddress(fetch.base_address << 12);
	const uint32_t mipAddress = resolveGpuAddress(fetch.mip_address << 12);

	// What the dump's top level is: the base if it is resident, otherwise the
	// highest mip that actually is.
	const uint32_t topWidth = std::max(1u, width >> firstLevel);
	const uint32_t topHeight = std::max(1u, height >> firstLevel);

	auto levelLayoutFor = [&](uint32_t level) -> const texture_util::TextureGuestLayout::Level& {
		return level == 0 ? layout.base : layout.mips[level];
	};
	auto levelAddressFor = [&](uint32_t level) {
		return level == 0 ? baseAddress : mipAddress + layout.mip_offsets_bytes[level];
	};

	// Skip surfaces whose memory is still changing or was never written.
	bool doDump = false;
	bool doReplace = false;
	{
		const uint32_t probeAddress = levelAddressFor(firstLevel);
		const uint32_t probeExtent = levelLayoutFor(firstLevel).level_data_extent_bytes;
		if (!physicalRangeIsSane(probeAddress, probeExtent)) return;

		bool allZero = false;
		const uint64_t probe = probeTexels(
			memory->TranslatePhysical<const uint8_t*>(probeAddress), probeExtent, allZero);

		const uint64_t watchKey = (uint64_t(pTexture) << 32) | fetch.base_address;

		std::lock_guard<std::mutex> lock(g_mutex);
		TextureWatch& watch = g_watched[watchKey];

		doDump = dumping && !watch.dumped;
		doReplace = replacing && !watch.replaceChecked;
		if (!doDump && !doReplace) return;

		if (watch.probe != probe || watch.stableBinds == 0) {
			// First sight, or the contents just changed.
			watch.probe = probe;
			watch.stableBinds = 1;
			return;
		}

		// Untouched memory is perfectly stable, so it would otherwise sail
		// through the gate and be captured as a black image.
		if (allZero) return;

		if (++watch.stableBinds < kMinStableBinds) return;

		if (doDump) watch.dumped = true;
		if (doReplace) watch.replaceChecked = true;
	}

	// Only levels the texture actually stores separately; a packed mip tail is
	// read as part of the level that holds it.
	const uint32_t storedLevels = std::max(firstLevel,
		std::min(maxLevel, layout.packed_level == UINT32_MAX ? maxLevel : layout.packed_level));

	std::vector<Level> levels;
	levels.reserve(storedLevels - firstLevel + 1);

	for (uint32_t level = firstLevel; level <= storedLevels; ++level) {
		const texture_util::TextureGuestLayout::Level& levelLayout = levelLayoutFor(level);
		const uint32_t levelAddress = levelAddressFor(level);

		if (!physicalRangeIsSane(levelAddress, levelLayout.level_data_extent_bytes)) {
			RNUT_WARN("texture: mip {} of {}x{} {} runs outside physical memory at {:#x}",
				level, width, height, formatInfo->name, levelAddress);
			return;
		}

		// Texel data is addressed physically by the fetch constant, which is a
		// different mapping from the virtual pointer the texture object itself
		// came in on.
		const uint8_t* levelData = memory->TranslatePhysical<const uint8_t*>(levelAddress);

		Level out;
		if (!readLevel(levelData, levelLayout, formatInfo, fetch.tiled != 0,
		               fetch.endianness, std::max(1u, width >> level),
		               std::max(1u, height >> level), out)) {
			RNUT_WARN("texture: could not read mip {} of {}x{} {} at {:#x}",
				level, width, height, formatInfo->name, levelAddress);
			return;
		}
		levels.push_back(std::move(out));
	}

	// The identity a replacement matches on. Content plus shape, so two
	// textures that happen to share bytes but not dimensions stay distinct.
	// The shape is the dump's own top level, not the fetch constant's nominal
	// size, so a texture keeps one identity whether or not its base is
	// resident.
	uint64_t hash;
	{
		const uint32_t shape[3] = {topWidth, topHeight, static_cast<uint32_t>(format)};
		XXH3_state_t state;
		XXH3_64bits_reset(&state);
		XXH3_64bits_update(&state, shape, sizeof(shape));
		for (const Level& level : levels) {
			XXH3_64bits_update(&state, level.data.data(), level.data.size());
		}
		hash = XXH3_64bits_digest(&state);
	}

	char stem[24];
	std::snprintf(stem, sizeof(stem), "%016llx", static_cast<unsigned long long>(hash));

	if (doDump) {
		bool alreadyDumped;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			alreadyDumped = !g_dumped.insert(hash).second;
		}
		if (!alreadyDumped) {
			const std::filesystem::path outDir = textureFolder("dump");
			std::error_code ec;
			std::filesystem::create_directories(outDir, ec);
			if (ec) {
				RNUT_ERROR("texture dump: cannot create {} ({})", outDir.string(), ec.message());
			} else if (writeDds(outDir / (std::string(stem) + ".dds"), *mapping, formatInfo,
			                    levels)) {
				if (REXCVAR_GET(dump_textures_raw)) {
					appendIndexRow(outDir, stem, fetch, words, formatInfo, baseAddress,
						mipAddress, layout, firstLevel, levels.size());

					// Exactly the bytes readLevel decoded, so a bad decode can be
					// reproduced and re-tried offline without re-running the game.
					std::ofstream raw(outDir / (std::string(stem) + ".raw"),
						std::ios::binary | std::ios::trunc);
					raw.write(reinterpret_cast<const char*>(
							memory->TranslatePhysical<const uint8_t*>(levelAddressFor(firstLevel))),
						static_cast<std::streamsize>(
							levelLayoutFor(firstLevel).level_data_extent_bytes));
				}

				RNUT_INFO("texture dump: {} ({}x{} {}, {} level(s), {}{})",
					stem, topWidth, topHeight, formatInfo->name, levels.size(),
					fetch.tiled ? "tiled" : "linear",
					hasBase ? "" : ", base level not resident");
			}
		}
	}

	if (!doReplace) return;

	std::filesystem::path replacementPath;
	{
		std::lock_guard<std::mutex> lock(g_mutex);

		// Reload whenever the configured pack list differs from the one the
		// table was built from, so editing texture_packs takes effect without
		// a restart for textures that have not been swapped yet.
		const std::string packList = REXCVAR_GET(texture_packs);
		if (!g_replacementsScanned || packList != g_scannedPacks) {
			scanReplacements(packList);
		}

		auto it = g_replacements.find(hash);
		if (it == g_replacements.end()) return;
		replacementPath = it->second;
	}

	xenos::TextureFormat replacementFormat;
	const FormatInfo* replacementInfo = nullptr;
	std::vector<Level> replacementLevels;
	if (!readDds(replacementPath, replacementFormat, replacementInfo, replacementLevels)) return;

	// Changing the storage format would change how the shader's swizzle and
	// sign settings interpret the data, so the replacement has to keep it.
	if (replacementFormat != format) {
		RNUT_WARN("texture replace: {} is {} but the original is {} - skipped",
			replacementPath.filename().string(), replacementInfo->name, formatInfo->name);
		return;
	}

	if (!applyReplacement(fetchWords, fetch, replacementInfo, replacementLevels)) {
		RNUT_ERROR("texture replace: failed to apply {}", replacementPath.filename().string());
		return;
	}

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_patched.emplace(pTexture, hash);
	}

	RNUT_INFO("texture replace: {} -> {}x{} {}, {} level(s)",
		stem, replacementLevels[0].width, replacementLevels[0].height,
		replacementInfo->name, replacementLevels.size());
}

}  // namespace

// D3DDevice_SetTexture @ 0x8222F410 -- r5 = pTexture at entry.
void textureTools_SetTexture_hook(PPCRegister& r5)
{
	handleTexture(r5.u32);
}
