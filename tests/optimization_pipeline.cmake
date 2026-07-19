# Prove that -O2 is not merely a command-line spelling: compare emitted IR
# with -O0, then execute both optimized JIT and AOT paths.  O3 is also built
# to keep the public optimization-level surface covered.

if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

set(source_path "${LUNA_SOURCE_DIR}/tests/fixtures/optimization_constant_fold.luna")
set(ir_path "${source_path}.ll")
set(executable_path "${LUNA_SOURCE_DIR}/tests/fixtures/optimization_constant_fold")
if(WIN32)
    set(executable_path "${executable_path}.exe")
endif()

function(cleanup_outputs)
    file(REMOVE "${ir_path}" "${executable_path}")
endfunction()

function(build_and_read level output_var)
    cleanup_outputs()
    execute_process(
        COMMAND "${LUNA_EXECUTABLE}" build "${source_path}" "${level}"
        RESULT_VARIABLE build_result
        OUTPUT_VARIABLE build_output
        ERROR_VARIABLE build_error
    )
    if(NOT build_result EQUAL 0 OR NOT EXISTS "${ir_path}" OR NOT EXISTS "${executable_path}")
        cleanup_outputs()
        message(FATAL_ERROR
            "optimization build ${level} failed.\nResult: ${build_result}\n"
            "Output:\n${build_output}\n${build_error}")
    endif()
    file(READ "${ir_path}" ir)
    set(${output_var} "${ir}" PARENT_SCOPE)
endfunction()

cleanup_outputs()
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" run "${source_path}" -O2
    RESULT_VARIABLE jit_result
    OUTPUT_VARIABLE jit_output
    ERROR_VARIABLE jit_error
)
string(FIND "${jit_output}\n${jit_error}" "error[" jit_error_at)
string(FIND "${jit_output}" "Program exited with code: 42" jit_exit_at)
if(NOT jit_result EQUAL 42 OR NOT jit_error_at EQUAL -1 OR jit_exit_at EQUAL -1)
    message(FATAL_ERROR
        "optimized JIT execution failed.\nResult: ${jit_result}\n"
        "Output:\n${jit_output}\n${jit_error}")
endif()

build_and_read(-O0 o0_ir)
string(FIND "${o0_ir}" "%left = alloca i32" o0_stack_slot)
if(o0_stack_slot EQUAL -1)
    cleanup_outputs()
    message(FATAL_ERROR "expected unoptimized IR to retain the local stack slot.\nIR:\n${o0_ir}")
endif()

build_and_read(-O2 o2_ir)
string(FIND "${o2_ir}" "%left = alloca i32" o2_stack_slot)
string(FIND "${o2_ir}" "ret i32 42" o2_constant_return)
if(NOT o2_stack_slot EQUAL -1 OR o2_constant_return EQUAL -1)
    cleanup_outputs()
    message(FATAL_ERROR
        "-O2 did not apply the expected stack promotion and constant propagation.\nIR:\n${o2_ir}")
endif()
execute_process(
    COMMAND "${executable_path}"
    RESULT_VARIABLE aot_result
    OUTPUT_VARIABLE aot_output
    ERROR_VARIABLE aot_error
)
if(NOT aot_result EQUAL 42)
    cleanup_outputs()
    message(FATAL_ERROR
        "optimized AOT executable returned ${aot_result}, expected 42.\n"
        "Output:\n${aot_output}\n${aot_error}")
endif()

build_and_read(-O3 o3_ir)
cleanup_outputs()
string(FIND "${o3_ir}" "ret i32 42" o3_constant_return)
if(o3_constant_return EQUAL -1)
    message(FATAL_ERROR "-O3 did not preserve the expected constant result.\nIR:\n${o3_ir}")
endif()
