if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at luna")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR OR NOT DEFINED LUNA_PLUGIN OR NOT EXISTS "${LUNA_PLUGIN}")
    message(FATAL_ERROR "LUNA_SOURCE_DIR and LUNA_PLUGIN are required")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "LUNA_FRAGMENT_PIPELINE=external_trace"
        "LUNA_FRAGMENT_PLUGIN=${LUNA_PLUGIN}"
        "${LUNA_EXECUTABLE}" run
        "${LUNA_SOURCE_DIR}/tests/fixtures/external_fragment_dispatch.luna"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
set(transcript "${output}\n${error}")
string(FIND "${transcript}" "Program exited with code: 0" completed)
string(FIND "${transcript}" "42" continuation)
string(FIND "${transcript}" "external fragment plugin error" plugin_error)
if(NOT result EQUAL 0 OR completed EQUAL -1 OR continuation EQUAL -1 OR NOT plugin_error EQUAL -1)
    message(FATAL_ERROR "external fragment dispatch failed.\n${transcript}")
endif()
