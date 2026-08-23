if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at luna")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

set(source "${LUNA_SOURCE_DIR}/tests/fixtures/context_continuation_return_valid.luna")
include("${LUNA_SOURCE_DIR}/tests/aot_package_fixture.cmake")
luna_stage_aot_application("${source}" "structured_cps_abi")
set(ir "${LUNA_AOT_IR_PATH}")
set(executable "${LUNA_AOT_EXECUTABLE_PATH}")
file(REMOVE "${ir}" "${executable}")

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${LUNA_AOT_PACKAGE_DIR}" -O0
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0 OR NOT EXISTS "${ir}")
    file(REMOVE "${ir}" "${executable}")
    message(FATAL_ERROR "structured CPS build failed:\n${build_output}\n${build_error}")
endif()

file(READ "${ir}" lowered_ir)
# Production continuation control flow is canonical CFG, not the retired
# structured stack-frame lowering.
string(FIND "${lowered_ir}" "cfg." canonical_cfg_at)
string(FIND "${lowered_ir}" "luna.continuation.frame" structured_frame_at)
if(canonical_cfg_at EQUAL -1 OR NOT structured_frame_at EQUAL -1)
    file(REMOVE "${ir}" "${executable}")
    message(FATAL_ERROR
        "continuation return did not use sole canonical CFG lowering.\nIR:\n${lowered_ir}")
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
