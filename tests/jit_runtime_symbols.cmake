# JIT must resolve Luna runtime helpers from its explicit ORC symbol table,
# while preserving the pay-only-for-used kernel entry boundary.

if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at the built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

set(cpu_source "${LUNA_SOURCE_DIR}/tests/fixtures/jit_aot_parity.luna")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env LUNA_GPU_BACKEND=invalid-backend
            "${LUNA_EXECUTABLE}" run "${cpu_source}" -O2
    RESULT_VARIABLE cpu_result
    OUTPUT_VARIABLE cpu_output
    ERROR_VARIABLE cpu_error
)
string(FIND "${cpu_output}" "Program exited with code: ${cpu_result}" cpu_exit)
string(FIND "${cpu_output}\n${cpu_error}" "GPU backend initialization failed" cpu_gpu_error)
if(cpu_exit EQUAL -1 OR NOT cpu_gpu_error EQUAL -1)
    message(FATAL_ERROR
        "CPU-only JIT unexpectedly paid a GPU runtime-entry cost.\n"
        "Result: ${cpu_result}\nOutput:\n${cpu_output}\n${cpu_error}")
endif()

set(kernel_source "${LUNA_SOURCE_DIR}/examples/heterogeneous.luna")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env LUNA_GPU_BACKEND=sim
            "${LUNA_EXECUTABLE}" run "${kernel_source}" -O2
    RESULT_VARIABLE sim_result
    OUTPUT_VARIABLE sim_output
    ERROR_VARIABLE sim_error
)
string(FIND "${sim_output}" "42" sim_value)
string(FIND "${sim_output}" "Program exited with code: 0" sim_exit)
if(NOT sim_result EQUAL 0 OR sim_value EQUAL -1 OR sim_exit EQUAL -1)
    message(FATAL_ERROR
        "JIT could not resolve or execute the explicit GPU runtime symbols.\n"
        "Result: ${sim_result}\nOutput:\n${sim_output}\n${sim_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env LUNA_GPU_BACKEND=invalid-backend
            "${LUNA_EXECUTABLE}" run "${kernel_source}" -O2
    RESULT_VARIABLE invalid_result
    OUTPUT_VARIABLE invalid_output
    ERROR_VARIABLE invalid_error
)
string(FIND "${invalid_output}\n${invalid_error}"
       "GPU backend initialization failed for 'invalid-backend'" invalid_boundary)
if(NOT invalid_result EQUAL 1 OR invalid_boundary EQUAL -1)
    message(FATAL_ERROR
        "Kernel JIT did not preserve its runtime initialization boundary.\n"
        "Result: ${invalid_result}\nOutput:\n${invalid_output}\n${invalid_error}")
endif()
