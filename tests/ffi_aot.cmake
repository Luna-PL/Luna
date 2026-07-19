# Exercise the same C ABI declaration through the AOT linker rather than only
# through ORC's process-symbol lookup used by the JIT regression suite.

if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

set(source_path "${LUNA_SOURCE_DIR}/examples/ffi.luna")
set(ir_path "${source_path}.ll")
set(executable_path "${LUNA_SOURCE_DIR}/examples/ffi")
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
        "FFI AOT build failed.\nResult: ${build_result}\n"
        "Output:\n${build_output}\n${build_error}")
endif()

execute_process(
    COMMAND "${executable_path}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error
)
cleanup_outputs()
string(FIND "${run_output}\n${run_error}" "hello from C FFI" greeting_at)
if(NOT run_result EQUAL 0 OR greeting_at EQUAL -1)
    message(FATAL_ERROR
        "FFI AOT executable did not call the expected C symbol.\nResult: ${run_result}\n"
        "Output:\n${run_output}\n${run_error}")
endif()
