# Verifies that a symbol reached the built binary's dynamic symbol table.
#
# Used for the rex::filesystem::FileHandle::OpenExisting override in
# src/renut_engine/linuxfixes/posix_file_access.cpp: that fix only takes effect
# if the symbol is exported, and a silent failure would quietly bring back the
# save-data corruption. Better to break the build than to ship a no-op fix.

find_program(RENUT_NM NAMES nm llvm-nm)
if(NOT RENUT_NM)
    message(WARNING "nm not found - skipping exported symbol check for ${RENUT_SYMBOL}")
    return()
endif()

execute_process(
    COMMAND ${RENUT_NM} -D --defined-only ${RENUT_BINARY}
    OUTPUT_VARIABLE nm_output
    ERROR_VARIABLE nm_error
    RESULT_VARIABLE nm_result)

if(NOT nm_result EQUAL 0)
    message(WARNING "nm failed on ${RENUT_BINARY}: ${nm_error}")
    return()
endif()

if(NOT nm_output MATCHES "${RENUT_SYMBOL}")
    message(FATAL_ERROR
        "${RENUT_SYMBOL} is missing from the dynamic symbol table of ${RENUT_BINARY}.\n"
        "The FileHandle::OpenExisting override in "
        "src/renut_engine/linuxfixes/posix_file_access.cpp will NOT take effect, "
        "and save data will be corrupted again.\n"
        "The mangled name has most likely changed - re-derive it with:\n"
        "  nm -D --defined-only <path to librexruntimerd.so> | grep FileHandle12OpenExisting\n"
        "and update RENUT_OPENEXISTING_SYMBOL in CMakeLists.txt.")
endif()

message(STATUS "Verified exported override symbol: ${RENUT_SYMBOL}")
