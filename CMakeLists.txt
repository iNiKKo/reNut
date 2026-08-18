
# renut - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(renut LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# src/core/memory.cpp (byte-swap intrinsics, in rexglue-sdk) uses
# _mm_shuffle_epi8 unconditionally, with no runtime CPU-feature dispatch --
# it has always required SSSE3 at compile time. The prebuilt/extracted SDK
# package this project normally uses was built on a toolchain whose default
# target baseline already included SSSE3, which silently masked the missing
# flag; building the SDK from source directly on a baseline x86-64 target
# (no implied SSSE3) surfaces it as a real "always_inline function requires
# target feature" compile error. Set from here (before add_subdirectory()
# pulls the SDK in below) rather than editing rexglue-sdk's own
# CMakeLists.txt, since add_compile_options() propagates into subdirectories
# added afterward.
if(NOT WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
    add_compile_options(-mssse3)
endif()

include(generated/rexglue.cmake)

# Native-renderer shader pipeline (see PLAN_native_renderer.md). Fetches
# XenosRecomp and, if a guest shader dump already exists, regenerates the
# recompiled shader cache at configure time. Must come after rexglue.cmake so
# the SDK's own fmt::fmt / xxHash::xxhash targets exist first and XenosRecomp
# reuses them instead of redefining them.
include(cmake/rexglue_xenosrecomp.cmake)

# Sources
set(RENUT_SOURCES
    src/main.cpp
    src/renut_engine/hooks.cpp
    src/renut_engine/StopNSwap.cpp
	src/renut_engine/frameHooks.cpp
    src/renut_engine/FPS.cpp
    src/renut_engine/game_activity_stats.cpp
    src/renut_engine/render_hooks_stub.cpp
    src/renut_engine/trace_stats.cpp
    src/renut_engine/cvar_menu.cpp
    src/renut_engine/fixes/mullhwucrash.cpp
    src/renut_engine/frontend_debug_menu.cpp
    src/renut_engine/mnk_controls.cpp
    src/renut_engine/shader_dump.cpp
    src/renut_engine/townsfolk.cpp
)

if(UNIX AND NOT APPLE)
    list(APPEND RENUT_SOURCES
        src/renut_engine/linuxfixes/audio_client_guard.cpp
        src/renut_engine/linuxfixes/audio_backend.cpp
        src/renut_engine/linuxfixes/posix_file_access.cpp
    )
endif()

if(WIN32)
list(APPEND RENUT_SOURCES icon/app.rc)
    add_executable(renut WIN32 ${RENUT_SOURCES})
else()
    add_executable(renut ${RENUT_SOURCES})
endif()

# Native-renderer: convert ONE known-good real Banjo shader to HLSL via
# XenosRecomp (see PLAN_native_renderer.md Phase 0 item 3/4 and Phase 1).
# Deliberately NOT the whole shader dump -- XenosRecomp's directory-scan mode
# hard-aborts (some shaders even segfault it) on real Banjo shaders it can't
# fully handle yet, and Phase 1 only needs one working shader. No-ops until a
# `shaders/` guest dump exists next to the exe (produced by the
# `dump_guest_shaders` cvar at runtime).
renut_xenos_shader(renut
    INPUT "${CMAKE_BINARY_DIR}/shaders/vs_67021cae68a26b46.bin"
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/renut_xenos_shader_vs_67021cae68a26b46.hlsl"
)

# Real constant-table synthesis (2026-08-16, see
# renut_synthesize_constant_tables()'s own comment in
# cmake/rexglue_xenosrecomp.cmake for the full story): most real captured
# Banjo-Kazooie shaders are missing the D3DX9 constant-reflection table
# XenosRecomp needs (a real upstream XenosRecomp limitation, not a reNut
# capture bug), so this step patches a build-tree COPY of the shader dump
# with synthesized tables before the batch converter below ever sees it --
# raises real conversion success from 36/253 to 101/253 shaders. Must run
# before renut_xenos_shader_batch() so its SHADER_DUMP_DIR can point at the
# patched copy instead of the raw dump.
renut_synthesize_constant_tables(
    SHADER_DUMP_DIR "${CMAKE_BINARY_DIR}/shaders"
    REXSDK_DIR "${REXSDK_DIR}"
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders_synthesized"
)

# Real container synthesis from ucode-only captures (2026-08-16, see
# renut_synthesize_containers_from_ucode()'s own comment in
# cmake/rexglue_xenosrecomp.cmake for the full story): this game's actual
# MOST-USED shaders never call D3D9's CreateVertexShader/CreatePixelShader
# at all (loaded via GPU command-stream IM_LOAD packets instead), so
# shader_dump.cpp's capture above structurally can never see them --
# confirmed 0% overlap between that capture and real live draw identities.
# renut_dump_ucode_hash's raw microcode captures (shaders_ucode_hash/) DO
# cover them; this step rebuilds full containers for those and adds them
# into the SAME shaders_synthesized/ directory the constant-table step
# above just populated (that step clears+repopulates the directory from
# shaders/ FIRST, this step only adds -- must run in exactly this order,
# hence the explicit dependency below, or the ucode-derived containers
# would get wiped). Verified end-to-end: unlocks 1492 additional real,
# actually-drawn shaders (vs 101 from the D3D9-hook path alone).
renut_synthesize_containers_from_ucode(
    UCODE_DIR "${CMAKE_BINARY_DIR}/shaders_ucode_hash"
    REXSDK_DIR "${REXSDK_DIR}"
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders_synthesized"
)
if(TARGET renut_synthesize_constant_tables_target AND TARGET renut_synthesize_containers_from_ucode_target)
    add_dependencies(renut_synthesize_containers_from_ucode_target renut_synthesize_constant_tables_target)
endif()

# "Convert the important/common shaders" effort (see PLAN_native_renderer.md):
# batch-converts the WHOLE real guest shader dump (all shaders captured by
# `dump_guest_shaders`, hundreds of real Banjo shaders) to compiled SPIR-V,
# isolating crash-prone shaders per-subprocess so they're skipped instead of
# aborting the whole cache (see tools/xenos_batch_convert.py). Consumed by
# rexgpu-nativevk (src/graphics/CMakeLists.txt in the SDK checkout) via a
# deterministic path check, NOT a normal target_sources() call -- that SDK
# CMakeLists is processed by include(generated/rexglue.cmake) ABOVE, i.e.
# before this custom command exists in the build graph, so a completely
# fresh checkout needs ONE EXTRA reconfigure after the cache is first
# generated (`cmake --build . --target renut_xenos_shader_batch` or just a
# normal build followed by re-running cmake) before rexgpu-nativevk picks it up
# -- same two-step pattern renut_xenos_shader() above already requires for
# its single-shader HLSL output.
#
# SHADER_DUMP_DIR points at the constant-table-patched COPY (see above), not
# the raw ${CMAKE_BINARY_DIR}/shaders capture -- the real, unpatched dump is
# left untouched.
renut_xenos_shader_batch(renut
    SHADER_DUMP_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders_synthesized"
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/renut_xenos_shader_cache.cpp"
    MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/generated/renut_xenos_shader_cache_manifest.txt"
)
if(TARGET renut_synthesize_constant_tables_target AND TARGET renut_xenos_shader_batch)
    add_dependencies(renut_xenos_shader_batch renut_synthesize_constant_tables_target)
endif()
if(TARGET renut_synthesize_containers_from_ucode_target AND TARGET renut_xenos_shader_batch)
    add_dependencies(renut_xenos_shader_batch renut_synthesize_containers_from_ucode_target)
endif()

# Real bug found+fixed 2026-08-16: renut_xenos_shader_batch() above only wired
# its DEPENDS onto `renut` (the top-level executable target), not onto
# `rexgpu-nativevk` (the SDK-side plugin .so that actually #includes the
# generated cache .cpp -- confirmed via renut_xenos_pipeline_cache.cpp's own
# __has_include check). Real, reproduced symptom: after regenerating the
# cache with genuinely new/changed content, `renut` relinks (its own real
# dependency is satisfied) but `rexgpu-nativevk` reports "no work to do" and
# keeps shipping the STALE cache baked into its already-built .so -- confirmed
# via a real stale-vs-fresh timestamp/content mismatch, not a guess. Fixed by
# adding the same dependency directly onto rexgpu-nativevk, once it exists (it's
# defined inside the SDK's own add_subdirectory(), which already ran by this
# point in the file).
if(TARGET rexgpu-nativevk AND TARGET renut_xenos_shader_batch)
    add_dependencies(rexgpu-nativevk renut_xenos_shader_batch)
endif()

# renut's own sources (FPS.cpp, mnk_controls.cpp, renut_logging_overlay.h,
# etc.) #include "imgui.h" directly. In the prebuilt/extracted SDK package
# this normally worked because `make install` flattens ALL SDK+thirdparty
# headers into one include/ dir that rex::runtime's PUBLIC include path
# already reaches -- rex::runtime itself never actually depended on imgui.
# Building the SDK from source (add_subdirectory/REXSDK_DIR mode) skips that
# install step, so that flattening never happens and the true, previously-
# masked missing dependency surfaces as `imgui.h` not found. Real fix: link
# imgui directly, since renut is the real consumer, not through an
# rexruntime/rexui detour that doesn't reflect renut's actual architecture.
target_link_libraries(renut PRIVATE imgui::imgui)

rexglue_setup_target(renut GPU_PLUGINS xenos nativevk)
target_compile_definitions(renut PRIVATE VK_ENABLE_BETA_EXTENSIONS)
if(UNIX AND NOT APPLE)
    target_compile_definitions(renut PRIVATE VK_USE_PLATFORM_XCB_KHR)
endif()

if(UNIX AND NOT APPLE)
    # src/renut_engine/linuxfixes/posix_file_access.cpp redefines
    # rex::filesystem::FileHandle::OpenExisting to fix the broken O_RDONLY/
    # O_WRONLY/O_RDWR bitwise-OR in the SDK's POSIX backend. Every caller lives
    # inside librexruntimerd.so, so our definition only wins if it is in renut's
    # dynamic symbol table -- the executable is searched first when the library's
    # PLT entry is resolved. Export just that one symbol rather than using
    # -rdynamic, which would export everything.
    #
    # If a toolchain change ever alters this mangled name, the override silently
    # stops applying, so the build checks for it below.
    set(RENUT_OPENEXISTING_SYMBOL
        "_ZN3rex10filesystem10FileHandle12OpenExistingERKNSt10filesystem7__cxx114pathEjb")
    target_link_options(renut PRIVATE
        "-Wl,--export-dynamic-symbol=${RENUT_OPENEXISTING_SYMBOL}")

    # Fail loudly if the symbol did not make it into .dynsym.
    add_custom_command(TARGET renut POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DRENUT_BINARY=$<TARGET_FILE:renut>
                -DRENUT_SYMBOL=${RENUT_OPENEXISTING_SYMBOL}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_exported_symbol.cmake
        VERBATIM)
endif()
