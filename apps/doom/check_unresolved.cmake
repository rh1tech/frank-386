if(NOT DEFINED NM OR NOT DEFINED INPUT)
    message(FATAL_ERROR "check_unresolved.cmake requires NM and INPUT")
endif()

execute_process(
    COMMAND "${NM}" -u "${INPUT}"
    RESULT_VARIABLE NM_RESULT
    OUTPUT_VARIABLE NM_OUTPUT
    ERROR_VARIABLE NM_ERROR
)

if(NOT NM_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Failed to inspect unresolved symbols in ${INPUT}:\n${NM_ERROR}")
endif()

set(ALLOWED_UNRESOLVED)
if(DEFINED ALLOW_UNRESOLVED AND NOT ALLOW_UNRESOLVED STREQUAL "")
    string(REPLACE "," ";" ALLOWED_UNRESOLVED "${ALLOW_UNRESOLVED}")
endif()

# nm -u prints one unresolved symbol per line. Keep the check strict, but allow
# explicitly named pseudo-linker symbols which elf2ez synthesizes later.
string(REPLACE "\r\n" "\n" NM_OUTPUT "${NM_OUTPUT}")
string(REPLACE "\r" "\n" NM_OUTPUT "${NM_OUTPUT}")
string(REPLACE "\n" ";" NM_LINES "${NM_OUTPUT}")

set(UNEXPECTED_UNRESOLVED "")
foreach(LINE IN LISTS NM_LINES)
    string(STRIP "${LINE}" LINE)
    if(LINE STREQUAL "")
        continue()
    endif()

    string(REGEX MATCH "[^ \t]+$" SYMBOL "${LINE}")
    list(FIND ALLOWED_UNRESOLVED "${SYMBOL}" ALLOWED_INDEX)
    if(ALLOWED_INDEX EQUAL -1)
        string(APPEND UNEXPECTED_UNRESOLVED "${LINE}\n")
    endif()
endforeach()

if(UNEXPECTED_UNRESOLVED)
    message(FATAL_ERROR
        "Native DOS image still contains unresolved symbols:\n${UNEXPECTED_UNRESOLVED}")
endif()
