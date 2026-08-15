# rexglue_xenosrecomp.cmake — build-time Xenos shader microcode -> HLSL/SPIR-V
# for the native-renderer effort (see PLAN_native_renderer.md Phase 0 item 3/4
# and Phase 1).
#
# Fetches hedge-dev/XenosRecomp as a host tool. Two entry points:
#   - renut_xenos_shader(): single named shader -> plain HLSL text, for
#     hand-inspection/Phase-1-style one-off integration.
#   - renut_xenos_shader_batch(): a whole guest shader dump directory -> one
#     combined, real, compiled (SPIR-V) shader cache .cpp, for the
#     "convert all the important shaders" effort. See tools/xenos_batch_convert.py
#     for why this isn't just XenosRecomp's own directory-scan mode: 26/748
#     real Banjo shaders segfault it outright (confirmed 2026-08-17), and its
#     directory driver has no per-shader crash recovery -- the batch script
#     isolates each shader in its own subprocess so one crasher doesn't take
#     down the whole cache.

include(FetchContent)

FetchContent_Declare(
    xenosrecomp
    GIT_REPOSITORY https://github.com/hedge-dev/XenosRecomp.git
    GIT_TAG main
    GIT_SUBMODULES_RECURSE ON
)
FetchContent_MakeAvailable(xenosrecomp)

# xenos_cache_unpack: decompresses XenosRecomp's own generated shader-cache
# .cpp (ZSTD-compressed, smol-v-encoded SPIR-V) into plain uint32_t SPIR-V
# words at BUILD TIME, so rexgpu-renut (a separate CMake build from reNut's
# own executable, the only place these FetchContent-provided zstd/smol-v
# targets exist) never needs to link zstd or smol-v itself. See
# tools/xenos_cache_unpack.cpp for the full rationale.
if(NOT TARGET xenos_cache_unpack)
    # smol-v has no CMake target of its own -- XenosRecomp's own build
    # compiles smolv.cpp directly into its executable's sources (see
    # XenosRecomp/CMakeLists.txt's SMOLV_SOURCE_DIR use), so this does the
    # same rather than inventing a target that doesn't exist upstream.
    add_executable(xenos_cache_unpack
        "${CMAKE_SOURCE_DIR}/tools/xenos_cache_unpack.cpp"
        "${xenosrecomp_SOURCE_DIR}/thirdparty/smol-v/source/smolv.cpp"
    )
    target_include_directories(xenos_cache_unpack PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${xenosrecomp_SOURCE_DIR}/thirdparty/smol-v/source"
    )
    target_link_libraries(xenos_cache_unpack PRIVATE libzstd_static)
endif()

# renut_xenos_shader(<target> INPUT <shader.bin> OUTPUT <shader.hlsl>)
#
# Wires a custom command that runs XenosRecomp in single-file mode against
# INPUT (one shader's .bin dump, e.g. from shader_dump.cpp's
# `dump_guest_shaders` capture) and produces OUTPUT as plain HLSL text (NOT a
# compiled shader-cache .cpp — this is source for inspection/Phase-1 hand
# integration, not something to add to <target>'s sources directly). No-ops
# (with a status message, not an error) if INPUT does not exist at configure
# time, since the guest shader dump this depends on is opt-in runtime capture,
# never checked into the repo.
function(renut_xenos_shader target_name)
    cmake_parse_arguments(ARG "" "INPUT;OUTPUT" "" ${ARGN})

    if(NOT EXISTS "${ARG_INPUT}")
        message(STATUS
            "renut_xenos_shader: '${ARG_INPUT}' not found, skipping "
            "(run with -Ddump_guest_shaders=true once to produce a shader "
            "dump, then reconfigure).")
        return()
    endif()

    set(_common_header "${xenosrecomp_SOURCE_DIR}/XenosRecomp/shader_common.h")

    add_custom_command(
        OUTPUT "${ARG_OUTPUT}"
        COMMAND $<TARGET_FILE:XenosRecomp>
                "${ARG_INPUT}"
                "${ARG_OUTPUT}"
                "${_common_header}"
        DEPENDS XenosRecomp "${_common_header}" "${ARG_INPUT}"
        COMMENT "Recompiling guest shader ${ARG_INPUT} via XenosRecomp"
        VERBATIM
    )

    add_custom_target(${target_name}_xenos_shader DEPENDS "${ARG_OUTPUT}")
    add_dependencies(${target_name} ${target_name}_xenos_shader)
endfunction()

# renut_xenos_shader_batch(<target> SHADER_DUMP_DIR <dir> OUTPUT <cache.cpp>
#                           [MANIFEST <manifest.txt>])
#
# Converts every real guest shader in SHADER_DUMP_DIR (reNut's
# `dump_guest_shaders` output) to compiled SPIR-V via XenosRecomp, using
# tools/xenos_batch_convert.py to isolate each shader in its own subprocess
# so crashing shaders are skipped instead of aborting the whole run. OUTPUT
# is XenosRecomp's own generated shader-cache C++ (ShaderCacheEntry table +
# compressed SPIR-V blob) built from only the shaders that converted
# successfully -- add it to <target>'s sources yourself if/when a consumer
# for the combined cache exists; this function only produces the file.
#
# No-ops (status message, not an error) if SHADER_DUMP_DIR does not exist at
# configure time, matching renut_xenos_shader()'s behavior -- the shader dump
# is opt-in runtime capture, never checked into the repo.
function(renut_xenos_shader_batch target_name)
    cmake_parse_arguments(ARG "" "SHADER_DUMP_DIR;OUTPUT;MANIFEST" "" ${ARGN})

    if(NOT EXISTS "${ARG_SHADER_DUMP_DIR}")
        message(STATUS
            "renut_xenos_shader_batch: '${ARG_SHADER_DUMP_DIR}' not found, "
            "skipping (run with -Ddump_guest_shaders=true and play for a "
            "while to produce a real shader dump, then reconfigure).")
        return()
    endif()

    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    set(_common_header "${xenosrecomp_SOURCE_DIR}/XenosRecomp/shader_common.h")
    set(_batch_script "${CMAKE_SOURCE_DIR}/tools/xenos_batch_convert.py")

    set(_extra_args "")
    if(ARG_MANIFEST)
        set(_extra_args "${ARG_MANIFEST}")
    endif()

    # Stage 1: xenos_batch_convert.py produces XenosRecomp's OWN generated
    # cache format (ShaderCacheEntry table + ZSTD-compressed smol-v SPIR-V) —
    # written to a scratch file, not ARG_OUTPUT directly, since stage 2
    # rewrites it into the plain/decompressed form ARG_OUTPUT actually is.
    set(_compressed_cache "${ARG_OUTPUT}.compressed.cpp")

    add_custom_command(
        OUTPUT "${_compressed_cache}"
        COMMAND ${Python3_EXECUTABLE} "${_batch_script}"
                $<TARGET_FILE:XenosRecomp>
                "${ARG_SHADER_DUMP_DIR}"
                "${_common_header}"
                "${_compressed_cache}"
                ${_extra_args}
        DEPENDS XenosRecomp "${_common_header}" "${_batch_script}"
        COMMENT "Batch-converting guest shaders in ${ARG_SHADER_DUMP_DIR} via XenosRecomp (crash-isolated per shader)"
        VERBATIM
    )

    # Stage 2: xenos_cache_unpack decompresses the stage-1 output into plain
    # uint32_t SPIR-V words, so <target> (rexgpu-renut, a separate CMake
    # build with no zstd/smol-v of its own) can consume ARG_OUTPUT directly.
    add_custom_command(
        OUTPUT "${ARG_OUTPUT}"
        COMMAND $<TARGET_FILE:xenos_cache_unpack> "${_compressed_cache}" "${ARG_OUTPUT}"
        DEPENDS xenos_cache_unpack "${_compressed_cache}"
        COMMENT "Decompressing native-renderer shader cache -> ${ARG_OUTPUT}"
        VERBATIM
    )

    add_custom_target(${target_name}_xenos_shader_batch DEPENDS "${ARG_OUTPUT}")
    add_dependencies(${target_name} ${target_name}_xenos_shader_batch)
endfunction()
