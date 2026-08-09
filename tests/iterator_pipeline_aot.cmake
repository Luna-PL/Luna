# Iterator adapters must preserve JIT/AOT behavior and remain allocation-free.

if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

set(source_path "${LUNA_SOURCE_DIR}/tests/fixtures/iterator_pipeline.luna")
set(ir_path "${source_path}.ll")
set(executable_path "${LUNA_SOURCE_DIR}/tests/fixtures/iterator_pipeline")
if(WIN32)
    set(executable_path "${executable_path}.exe")
endif()

function(cleanup_outputs)
    file(REMOVE "${ir_path}" "${executable_path}")
endfunction()

cleanup_outputs()
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" run "${source_path}"
    RESULT_VARIABLE jit_result
    OUTPUT_VARIABLE jit_output
    ERROR_VARIABLE jit_error
)
string(FIND "${jit_output}\n${jit_error}" "error[" jit_error_at)
string(FIND "${jit_output}" "Program exited with code: ${jit_result}" jit_marker)
if(NOT jit_result EQUAL 14 OR NOT jit_error_at EQUAL -1 OR jit_marker EQUAL -1)
    cleanup_outputs()
    message(FATAL_ERROR
        "iterator JIT execution failed.\nResult: ${jit_result}\n"
        "Output:\n${jit_output}\n${jit_error}")
endif()
string(SUBSTRING "${jit_output}" 0 ${jit_marker} jit_program_output)

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${source_path}" -O0
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0 OR NOT EXISTS "${ir_path}" OR
   NOT EXISTS "${executable_path}")
    cleanup_outputs()
    message(FATAL_ERROR
        "iterator AOT build failed.\nResult: ${build_result}\n"
        "Output:\n${build_output}\n${build_error}")
endif()

file(READ "${ir_path}" generated_ir)
string(FIND "${generated_ir}" "call ptr @rt_alloc" heap_alloc_at)
string(FIND "${generated_ir}" "call ptr @rt_rc_allocate_v1" rc_alloc_at)
string(FIND "${generated_ir}" "call ptr @rt_arc_allocate_v1" arc_alloc_at)
string(FIND "${generated_ir}" "@rt_iterator" runtime_iterator_at)
if(NOT heap_alloc_at EQUAL -1 OR NOT rc_alloc_at EQUAL -1 OR
   NOT arc_alloc_at EQUAL -1 OR NOT runtime_iterator_at EQUAL -1)
    cleanup_outputs()
    message(FATAL_ERROR
        "iterator lowering allocated or escaped to a runtime iterator ABI.\n"
        "${generated_ir}")
endif()

execute_process(
    COMMAND "${executable_path}"
    RESULT_VARIABLE aot_result
    OUTPUT_VARIABLE aot_output
    ERROR_VARIABLE aot_error
)
cleanup_outputs()
if(NOT aot_result EQUAL jit_result OR
   NOT "${aot_output}" STREQUAL "${jit_program_output}")
    message(FATAL_ERROR
        "iterator JIT/AOT behavior diverged.\n"
        "JIT result: ${jit_result}\nJIT stdout:\n${jit_program_output}\n"
        "AOT result: ${aot_result}\nAOT stdout:\n${aot_output}\n"
        "AOT stderr:\n${aot_error}")
endif()
