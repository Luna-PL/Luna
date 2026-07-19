# Host IR must remain valid when every branch of an if returns.  This catches
# detached/unterminated merge blocks that a JIT may otherwise defer until a
# later optimization or link step.

if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

set(source_path "${LUNA_SOURCE_DIR}/tests/fixtures/ownership_all_return_paths_consume.luna")
set(ir_path "${source_path}.ll")
set(executable_path "${LUNA_SOURCE_DIR}/tests/fixtures/ownership_all_return_paths_consume")
if(WIN32)
    set(executable_path "${executable_path}.exe")
endif()

function(cleanup_outputs)
    file(REMOVE "${ir_path}" "${executable_path}")
endfunction()

cleanup_outputs()
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${source_path}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0 OR NOT EXISTS "${ir_path}" OR NOT EXISTS "${executable_path}")
    cleanup_outputs()
    message(FATAL_ERROR
        "all-return control-flow AOT build failed.\n"
        "Result: ${build_result}\n"
        "Output:\n${build_output}\n${build_error}")
endif()

execute_process(
    COMMAND "${executable_path}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error
)
cleanup_outputs()
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR
        "all-return AOT executable did not return zero.\n"
        "Result: ${run_result}\n"
        "Output:\n${run_output}\n${run_error}")
endif()
