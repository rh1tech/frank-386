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

string(STRIP "${NM_OUTPUT}" NM_OUTPUT)

if(NM_OUTPUT)
    message(FATAL_ERROR
        "Native DOS image still contains unresolved symbols:\n${NM_OUTPUT}")
endif()
