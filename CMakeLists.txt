# renut - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(renut LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)



set(RENUT_SOURCES
    src/main.cpp
    src/renut_engine/hooks.cpp
    src/renut_engine/StopNSwap.cpp
	src/renut_engine/frameHooks.cpp
    src/renut_engine/fixes/mullhwucrash.cpp
	src/renut_engine/cvar_menu.cpp
	src/renut_engine/FPS.cpp
	src/renut_engine/game_activity_stats.cpp
	src/renut_engine/game_activity_hooks.cpp
	src/renut_engine/d3d9_draw_stats.cpp
	src/renut_engine/trace_stats.cpp
	src/renut_engine/shader_dump.cpp
	src/renut_engine/texture_tools.cpp
	src/renut_engine/frontend_debug_menu.cpp
	src/renut_engine/townsfolk.cpp
	src/renut_engine/mnk_controls.cpp

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

rexglue_setup_target(renut)

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

    # Same mechanism, for RenutEmitTraceRow (src/renut_engine/trace_stats.cpp):
    # the GPU plugin calls this weak hook (see
    # src/renut_engine/renut_trace_hook.h -- reNut's own copy of the SDK-side
    # header of the same name, kept byte-identical; see that file's own
    # comment for why it isn't just #include'd from the SDK checkout) once
    # per frame with timing data for the "Renderer performance" overlay, IF
    # the plugin was built from an SDK checkout with the native-renderer
    # patch applied. Without that patch (the normal case), the export below
    # is inert -- nothing calls it -- and the Performance tab just shows
    # "GPU trace unavailable" instead of failing. -Wl,--export-dynamic-symbol
    # is a GNU ld/lld ELF-linker flag with no Windows/macOS equivalent
    # wired up here, hence UNIX AND NOT APPLE; the symbol simply stays
    # unexported (same inert-until-patched behavior) on those platforms.
    target_link_options(renut PRIVATE
        "-Wl,--export-dynamic-symbol=RenutEmitTraceRow")

    # Fail loudly if the OpenExisting override did not make it into .dynsym.
    add_custom_command(TARGET renut POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DRENUT_BINARY=$<TARGET_FILE:renut>
                -DRENUT_SYMBOL=${RENUT_OPENEXISTING_SYMBOL}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_exported_symbol.cmake
        VERBATIM)
endif()
