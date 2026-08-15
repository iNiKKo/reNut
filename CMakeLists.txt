
# renut - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(renut LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

# "Convert the important/common shaders" effort (see PLAN_native_renderer.md):
# batch-converts the WHOLE real guest shader dump (all shaders captured by
# `dump_guest_shaders`, hundreds of real Banjo shaders) to compiled SPIR-V,
# isolating crash-prone shaders per-subprocess so they're skipped instead of
# aborting the whole cache (see tools/xenos_batch_convert.py). Consumed by
# rexgpu-renut (src/graphics/CMakeLists.txt in the SDK checkout) via a
# deterministic path check, NOT a normal target_sources() call -- that SDK
# CMakeLists is processed by include(generated/rexglue.cmake) ABOVE, i.e.
# before this custom command exists in the build graph, so a completely
# fresh checkout needs ONE EXTRA reconfigure after the cache is first
# generated (`cmake --build . --target renut_xenos_shader_batch` or just a
# normal build followed by re-running cmake) before rexgpu-renut picks it up
# -- same two-step pattern renut_xenos_shader() above already requires for
# its single-shader HLSL output.
renut_xenos_shader_batch(renut
    SHADER_DUMP_DIR "${CMAKE_BINARY_DIR}/shaders"
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/renut_xenos_shader_cache.cpp"
    MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/generated/renut_xenos_shader_cache_manifest.txt"
)

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

rexglue_setup_target(renut GPU_PLUGINS xenos renut)
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
