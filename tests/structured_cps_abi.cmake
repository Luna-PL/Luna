if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at luna")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

set(source "${LUNA_SOURCE_DIR}/tests/fixtures/context_continuation_return_valid.luna")
set(ir "${source}.ll")
set(executable "${LUNA_SOURCE_DIR}/tests/fixtures/context_continuation_return_valid")
file(REMOVE "${ir}" "${executable}")

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${source}" -O0
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0 OR NOT EXISTS "${ir}")
    file(REMOVE "${ir}" "${executable}")
    message(FATAL_ERROR "structured CPS build failed:\n${build_output}\n${build_error}")
endif()

file(READ "${ir}" lowered_ir)
# The structured CPS continuation-frame IR patterns are specific to the
# structured-body path. Under the canonical CFG gate, continuation control
# flow uses cfg.N blocks instead of continuation.* labels. Skip the IR
# pattern checks when the canonical gate is active.
if(NOT DEFINED ENV{LUNA_SEAL_CANONICAL} OR NOT "$ENV{LUNA_SEAL_CANONICAL}" STREQUAL "1")
foreach(required IN ITEMS
        "luna.continuation.frame"
        "continuation.entry"
        "continuation.return.dispatch"
        "continuation.return")
    string(FIND "${lowered_ir}" "${required}" found)
    if(found EQUAL -1)
        file(REMOVE "${ir}" "${executable}")
        message(FATAL_ERROR "structured CPS lowering omitted '${required}'.\nIR:\n${lowered_ir}")
    endif()
endforeach()
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env LUNA_GPU_BACKEND=sim "${executable}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error
)
file(REMOVE "${ir}" "${executable}")
if(NOT run_result EQUAL 1)
    message(FATAL_ERROR
        "structured CPS continuation return produced the wrong result.\n"
        "Expected process code 1, got ${run_result}.\n${run_output}\n${run_error}")
endif()
string(FIND "${run_output}" "2" post_resume_output)
if(NOT post_resume_output EQUAL -1)
    message(FATAL_ERROR "context post-resume code ran after continuation return.\n${run_output}")
endif()
