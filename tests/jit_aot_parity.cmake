# A JIT success alone does not establish that textual IR, system linking, and
# executable startup preserve the language result.  Compare both paths using
# one fixture with stdout and a deliberately non-zero return value.

if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

set(source_path "${LUNA_SOURCE_DIR}/tests/fixtures/jit_aot_parity.luna")
set(ir_path "${source_path}.ll")
set(executable_path "${LUNA_SOURCE_DIR}/tests/fixtures/jit_aot_parity")
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
set(jit_transcript "${jit_output}\n${jit_error}")
string(FIND "${jit_transcript}" "error[" jit_error_at)
string(FIND "${jit_output}" "Program exited with code: ${jit_result}" jit_marker)
if(NOT jit_error_at EQUAL -1 OR jit_marker EQUAL -1)
    cleanup_outputs()
    message(FATAL_ERROR
        "JIT parity fixture did not complete normally.\nResult: ${jit_result}\n"
        "Output:\n${jit_transcript}")
endif()
string(SUBSTRING "${jit_output}" 0 ${jit_marker} jit_program_output)

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${source_path}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0 OR NOT EXISTS "${ir_path}" OR NOT EXISTS "${executable_path}")
    cleanup_outputs()
    message(FATAL_ERROR
        "AOT parity build failed.\nResult: ${build_result}\n"
        "Output:\n${build_output}\n${build_error}")
endif()

execute_process(
    COMMAND "${executable_path}"
    RESULT_VARIABLE aot_result
    OUTPUT_VARIABLE aot_output
    ERROR_VARIABLE aot_error
)
cleanup_outputs()
if(NOT jit_result EQUAL aot_result OR NOT "${jit_program_output}" STREQUAL "${aot_output}")
    message(FATAL_ERROR
        "JIT and AOT behavior diverged.\n"
        "JIT result: ${jit_result}\nJIT stdout:\n${jit_program_output}\n"
        "AOT result: ${aot_result}\nAOT stdout:\n${aot_output}\nAOT stderr:\n${aot_error}")
endif()
