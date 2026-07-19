# This test is intentionally opt-in: it requires a usable AMD GPU, ROCm HIP
# runtime, and the matching kernel driver. It verifies the same source through
# both JIT and AOT so the device module embedding boundary is exercised too.

if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

set(source "${LUNA_SOURCE_DIR}/examples/heterogeneous.luna")
set(ir "${source}.ll")
set(executable "${LUNA_SOURCE_DIR}/examples/heterogeneous")
file(REMOVE "${ir}" "${executable}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env LUNA_GPU_BACKEND=rocm
            "${LUNA_EXECUTABLE}" run "${source}" -O2
    RESULT_VARIABLE jit_result
    OUTPUT_VARIABLE jit_output
    ERROR_VARIABLE jit_error
)
string(FIND "${jit_output}\n${jit_error}" "Program exited with code: 0" jit_ok)
string(FIND "${jit_output}" "42" jit_value_ok)
if(NOT jit_result EQUAL 0 OR jit_ok EQUAL -1 OR jit_value_ok EQUAL -1)
    file(REMOVE "${ir}" "${executable}")
    message(FATAL_ERROR "ROCm JIT smoke test failed.\n${jit_output}\n${jit_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env LUNA_GPU_BACKEND=rocm
            "${LUNA_EXECUTABLE}" build "${source}" -O2
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0 OR NOT EXISTS "${executable}")
    file(REMOVE "${ir}" "${executable}")
    message(FATAL_ERROR "ROCm AOT build smoke test failed.\n${build_output}\n${build_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env LUNA_GPU_BACKEND=rocm "${executable}"
    RESULT_VARIABLE aot_result
    OUTPUT_VARIABLE aot_output
    ERROR_VARIABLE aot_error
)
file(REMOVE "${ir}" "${executable}")
string(FIND "${aot_output}" "42" aot_value_ok)
if(NOT aot_result EQUAL 0 OR aot_value_ok EQUAL -1)
    message(FATAL_ERROR "ROCm AOT smoke test failed or returned an unexpected device result.\n${aot_output}\n${aot_error}")
endif()
