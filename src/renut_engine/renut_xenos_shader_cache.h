#pragma once
// Real-data shape for the batch-converted native-renderer shader cache (see
// PLAN_native_renderer.md "convert the important/common shaders"). Populated
// by generated/renut_xenos_shader_cache.cpp, produced at build time by
// tools/xenos_batch_convert.py + tools/xenos_cache_unpack.cpp from the
// user's real captured guest shader dump.
//
// spirvOffset/spirvSize index into g_renutXenosSpirvWords (uint32_t words,
// i.e. already real SPIR-V ready for vkCreateShaderModule -- ZSTD/smol-v
// decoding already happened at build time, not here).

#include <cstddef>
#include <cstdint>

struct RenutXenosShaderCacheEntry {
	uint64_t hash;
	size_t spirvOffset;
	size_t spirvSize;
	uint32_t specConstantsMask;
};

extern const uint32_t g_renutXenosSpirvWords[];
extern const RenutXenosShaderCacheEntry g_renutXenosShaderCacheEntries[];
extern const size_t g_renutXenosShaderCacheEntryCount;
