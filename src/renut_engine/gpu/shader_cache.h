// Shader cache produced by XenosRecomp (see gpu/shader_cache.cpp).
//
// Each guest Xbox 360 shader is identified by an XXH3-64 hash of its microcode
// container. At load time the game's D3DDevice_CreateVertex/PixelShader hands us
// that container; we hash it, look up the matching entry here, and create a host
// shader from the embedded (zstd-compressed) DXIL / SPIR-V blob. Entries are
// sorted by hash so lookups can binary-search.

#pragma once

#include <cstddef>
#include <cstdint>

// Forward declaration: the renderer attaches its per-shader host object (Plume
// shader + metadata) to the cache entry the first time a guest shader is created,
// so subsequent creations of the same shader reuse it.
struct GuestShader;

struct ShaderCacheEntry {
    const uint64_t hash;
    const uint32_t dxilOffset;
    const uint32_t dxilSize;
    const uint32_t spirvOffset;
    const uint32_t spirvSize;
    const uint32_t specConstantsMask;
    GuestShader* guestShader;
};

extern ShaderCacheEntry g_shaderCacheEntries[];
extern const size_t g_shaderCacheEntryCount;

extern const uint8_t g_compressedDxilCache[];
extern const size_t g_dxilCacheCompressedSize;
extern const size_t g_dxilCacheDecompressedSize;

extern const uint8_t g_compressedSpirvCache[];
extern const size_t g_spirvCacheCompressedSize;
extern const size_t g_spirvCacheDecompressedSize;
