if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" repl
    INPUT_FILE "${LUNA_SOURCE_DIR}/tests/fixtures/repl_session.txt"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
)
set(transcript "${output}\n${errors}")
foreach(expected
        "Multiline input is not supported"
        "declaration stored"
        "= 42"
        "7"
        "ok"
        "declarations reset")
    string(FIND "${transcript}" "${expected}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR
            "REPL transcript did not contain '${expected}'.\n${transcript}")
    endif()
endforeach()
if(NOT result EQUAL 0 OR transcript MATCHES "error\\[")
    message(FATAL_ERROR
        "REPL smoke session failed (${result}).\n${transcript}")
endif()
