if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

set(source_path
    "${LUNA_SOURCE_DIR}/tests/fixtures/iterator_materialized.luna")
set(ir_path "${source_path}.ll")
set(executable_path
    "${LUNA_SOURCE_DIR}/tests/fixtures/iterator_materialized")
if(WIN32)
    string(APPEND executable_path ".exe")
endif()
set(expected_output "20\n30\n1\n12\n7\n8\n")

file(REMOVE "${ir_path}" "${executable_path}")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" run "${source_path}"
    RESULT_VARIABLE jit_result
    OUTPUT_VARIABLE jit_output
    ERROR_VARIABLE jit_error
)
set(expected_jit
    "${expected_output}Program exited with code: 0\n")
if(NOT jit_result EQUAL 0 OR
   NOT "${jit_output}" STREQUAL "${expected_jit}" OR
   NOT "${jit_error}" STREQUAL "")
    file(REMOVE "${ir_path}" "${executable_path}")
    message(FATAL_ERROR
        "materialized iterator JIT behavior diverged.\n"
        "stdout:\n${jit_output}\nstderr:\n${jit_error}")
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${source_path}" -O0
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0 OR
   NOT EXISTS "${ir_path}" OR
   NOT EXISTS "${executable_path}")
    file(REMOVE "${ir_path}" "${executable_path}")
    message(FATAL_ERROR
        "materialized iterator AOT build failed.\n"
        "${build_output}\n${build_error}")
endif()

file(READ "${ir_path}" generated_ir)
foreach(forbidden_ir IN ITEMS
        "call ptr @rt_alloc"
        "@rt_iterator")
    string(FIND "${generated_ir}" "${forbidden_ir}" forbidden_ir_at)
    if(NOT forbidden_ir_at EQUAL -1)
        file(REMOVE "${ir_path}" "${executable_path}")
        message(FATAL_ERROR
            "materialized iterator unexpectedly uses '${forbidden_ir}'")
    endif()
endforeach()

execute_process(
    COMMAND "${executable_path}"
    RESULT_VARIABLE aot_result
    OUTPUT_VARIABLE aot_output
    ERROR_VARIABLE aot_error
)
file(REMOVE "${ir_path}" "${executable_path}")
if(NOT aot_result EQUAL 0 OR
   NOT "${aot_output}" STREQUAL "${expected_output}" OR
   NOT "${aot_error}" STREQUAL "")
    message(FATAL_ERROR
        "materialized iterator JIT/AOT behavior diverged.\n"
        "stdout:\n${aot_output}\nstderr:\n${aot_error}")
endif()
