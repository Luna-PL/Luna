if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

set(workspace "${LUNA_SOURCE_DIR}/tests/fixtures/workspaces/std_io")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" run "${workspace}/app"
    INPUT_FILE "${workspace}/stdin.txt"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error
)
if(NOT run_result EQUAL 42)
    message(FATAL_ERROR
        "temporary Std IO fixture failed with ${run_result}.\n"
        "stdout:\n${run_output}\nstderr:\n${run_error}")
endif()
string(FIND "${run_output}" "std-io: 41\n42\n" stdout_at)
string(FIND "${run_error}" "std-io-stderr\n" stderr_at)
if(stdout_at EQUAL -1 OR stderr_at EQUAL -1)
    message(FATAL_ERROR
        "temporary Std IO output diverged.\n"
        "stdout:\n${run_output}\nstderr:\n${run_error}")
endif()
