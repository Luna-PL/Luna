# Result propagation must preserve the same tagged ABI, cleanup behavior, and
# observable output in JIT and AOT compilation.

if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

set(source_path "${LUNA_SOURCE_DIR}/tests/fixtures/result_propagation.luna")
set(ir_path "${source_path}.ll")
set(executable_path "${LUNA_SOURCE_DIR}/tests/fixtures/result_propagation")
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
set(expected_output "70\n43\n70\n7\n")
string(FIND "${jit_output}" "${expected_output}" jit_output_at)
if(NOT jit_result EQUAL 0 OR jit_output_at EQUAL -1 OR NOT "${jit_error}" STREQUAL "")
    cleanup_outputs()
    message(FATAL_ERROR
        "Result JIT fixture failed.\nResult: ${jit_result}\n"
        "Output:\n${jit_output}\n${jit_error}")
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${source_path}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0 OR NOT EXISTS "${ir_path}" OR NOT EXISTS "${executable_path}")
    cleanup_outputs()
    message(FATAL_ERROR
        "Result AOT build failed.\nResult: ${build_result}\n"
        "Output:\n${build_output}\n${build_error}")
endif()

file(READ "${ir_path}" generated_ir)
string(FIND "${generated_ir}" "try.error" try_error_at)
string(FIND "${generated_ir}" "call void @rt_dealloc" cleanup_at)
string(FIND "${generated_ir}" "call void @rt_panic_cstr" panic_at)
if(try_error_at EQUAL -1 OR cleanup_at EQUAL -1 OR panic_at EQUAL -1)
    cleanup_outputs()
    message(FATAL_ERROR
        "Result AOT IR lost propagation, cleanup, or unwrap panic boundaries.\n"
        "${generated_ir}")
endif()

execute_process(
    COMMAND "${executable_path}"
    RESULT_VARIABLE aot_result
    OUTPUT_VARIABLE aot_output
    ERROR_VARIABLE aot_error
)
cleanup_outputs()
if(NOT aot_result EQUAL 0 OR
   NOT "${aot_output}" STREQUAL "${expected_output}" OR
   NOT "${aot_error}" STREQUAL "")
    message(FATAL_ERROR
        "Result JIT/AOT behavior diverged.\n"
        "AOT result: ${aot_result}\nAOT stdout:\n${aot_output}\n"
        "AOT stderr:\n${aot_error}")
endif()
