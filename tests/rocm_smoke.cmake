# This test is intentionally opt-in: it requires a usable AMD GPU, ROCm HIP
# runtime, and the matching kernel driver. It verifies the same source through
# both JIT and AOT so the device module embedding boundary is exercised too.

if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR OR NOT DEFINED LUNA_BINARY_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR and LUNA_BINARY_DIR are required")
endif()
if(NOT DEFINED LUNA_ROCM_ARCH OR LUNA_ROCM_ARCH STREQUAL "")
    set(LUNA_ROCM_ARCH gfx1101)
endif()
set(gpu_target "rocm:${LUNA_ROCM_ARCH}")

set(work_dir "${LUNA_BINARY_DIR}/rocm-smoke")
file(REMOVE_RECURSE "${work_dir}")
file(MAKE_DIRECTORY "${work_dir}")
file(COPY "${LUNA_SOURCE_DIR}/examples/heterogeneous.luna" DESTINATION "${work_dir}")
set(source "${work_dir}/heterogeneous.luna")
include("${LUNA_SOURCE_DIR}/tests/aot_package_fixture.cmake")
luna_stage_aot_application("${source}" "rocm_smoke")
set(ir "${LUNA_AOT_IR_PATH}")
set(executable "${LUNA_AOT_EXECUTABLE_PATH}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env LUNA_GPU_BACKEND=rocm
            "${LUNA_EXECUTABLE}" run "${source}" -O2
            "--gpu-target=${gpu_target}"
    RESULT_VARIABLE jit_result
    OUTPUT_VARIABLE jit_output
    ERROR_VARIABLE jit_error
)
string(FIND "${jit_output}\n${jit_error}" "Program exited with code: 0" jit_ok)
string(FIND "${jit_output}" "42" jit_value_ok)
if(NOT jit_result EQUAL 0 OR jit_ok EQUAL -1 OR jit_value_ok EQUAL -1)
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR "ROCm JIT smoke test failed.\n${jit_output}\n${jit_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env LUNA_GPU_BACKEND=rocm
            "${LUNA_EXECUTABLE}" build "${LUNA_AOT_PACKAGE_DIR}" -O2
            "--gpu-target=${gpu_target}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0 OR NOT EXISTS "${executable}")
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR "ROCm AOT build smoke test failed.\n${build_output}\n${build_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env LUNA_GPU_BACKEND=rocm "${executable}"
    RESULT_VARIABLE aot_result
    OUTPUT_VARIABLE aot_output
    ERROR_VARIABLE aot_error
)
file(REMOVE_RECURSE "${work_dir}")
string(FIND "${aot_output}" "42" aot_value_ok)
if(NOT aot_result EQUAL 0 OR aot_value_ok EQUAL -1)
    message(FATAL_ERROR "ROCm AOT smoke test failed or returned an unexpected device result.\n${aot_output}\n${aot_error}")
endif()
