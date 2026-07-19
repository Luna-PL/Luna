if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at luna")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

set(source "${LUNA_SOURCE_DIR}/tests/fixtures/fragment_contracts.luna")
set(ir "${source}.ll")
set(executable "${LUNA_SOURCE_DIR}/tests/fixtures/fragment_contracts")
file(REMOVE "${ir}" "${executable}")

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${source}" -O2
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0 OR NOT EXISTS "${ir}")
    file(REMOVE "${ir}" "${executable}")
    message(FATAL_ERROR "fragment lowering build failed:\n${build_output}\n${build_error}")
endif()

file(READ "${ir}" lowered_ir)
foreach(forbidden IN ITEMS
        "rt_dynamic_fragment_select"
        "rt_dynamic_fragment_matches"
        "continuation.frame"
        "fragment.resume"
        "@rt_malloc")
    string(FIND "${lowered_ir}" "${forbidden}" found)
    if(NOT found EQUAL -1)
        file(REMOVE "${ir}" "${executable}")
        message(FATAL_ERROR
            "static interceptor/context lowering retained forbidden overhead '${forbidden}'.\nIR:\n${lowered_ir}")
    endif()
endforeach()

execute_process(
    COMMAND "${executable}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error
)
file(REMOVE "${ir}" "${executable}")
string(FIND "${run_output}" "99" skipped_continuation)
string(FIND "${run_output}" "41" reached_after_abort)
if(NOT run_result EQUAL 0 OR NOT skipped_continuation EQUAL -1 OR reached_after_abort EQUAL -1)
    message(FATAL_ERROR
        "static fragment behavior changed. result=${run_result}\n${run_output}\n${run_error}")
endif()
