if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at luna")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR is required")
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" run
        "${LUNA_SOURCE_DIR}/tests/fixtures/external_fragment_dispatch.luna"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
set(transcript "${output}\n${error}")
string(FIND "${transcript}" "`dynamic slot` and `dynamic apply` were removed in Luna 0.3" removed)
if(result EQUAL 0 OR removed EQUAL -1)
    message(FATAL_ERROR "legacy external fragment source was not rejected.\n${transcript}")
endif()
