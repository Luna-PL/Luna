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

# Small straight-line, call-free while loops receive a conservative four-way
# O3 unroll hint. The reduction workload keeps the recurrence live and makes
# the selected count directly visible in optimized IR.
set(loop_source_path "${LUNA_SOURCE_DIR}/benchmarks/luna_cpu_reduction.luna")
set(loop_ir_path "${loop_source_path}.ll")
set(loop_executable_path "${LUNA_SOURCE_DIR}/benchmarks/luna_cpu_reduction")
if(WIN32)
    set(loop_executable_path "${loop_executable_path}.exe")
endif()
file(REMOVE "${loop_ir_path}" "${loop_executable_path}")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${loop_source_path}" -O3
    RESULT_VARIABLE loop_build_result
    OUTPUT_VARIABLE loop_build_output
    ERROR_VARIABLE loop_build_error
)
if(NOT loop_build_result EQUAL 0 OR NOT EXISTS "${loop_ir_path}")
    file(REMOVE "${loop_ir_path}" "${loop_executable_path}")
    message(FATAL_ERROR
        "O3 loop-unroll regression build failed.\nResult: ${loop_build_result}\n"
        "Output:\n${loop_build_output}\n${loop_build_error}")
endif()
file(READ "${loop_ir_path}" loop_ir)
file(REMOVE "${loop_ir_path}" "${loop_executable_path}")
string(REGEX MATCH
    "%addeqtmp\\.3 = add [^\n]*i32 [^\n]*, 4"
    loop_unroll_increment "${loop_ir}")
if(loop_unroll_increment STREQUAL "")
    message(FATAL_ERROR
        "-O3 did not apply the expected four-way straight-line loop unroll.\n"
        "IR:\n${loop_ir}")
endif()

# A tiny single-recurrence inner loop is intentionally below the heuristic's
# lower bound: forcing it to four-way unroll lengthens the dependency chain.
set(nested_source_path "${LUNA_SOURCE_DIR}/benchmarks/luna_cpu_nested.luna")
set(nested_ir_path "${nested_source_path}.ll")
set(nested_executable_path "${LUNA_SOURCE_DIR}/benchmarks/luna_cpu_nested")
if(WIN32)
    set(nested_executable_path "${nested_executable_path}.exe")
endif()
file(REMOVE "${nested_ir_path}" "${nested_executable_path}")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${nested_source_path}" -O3
    RESULT_VARIABLE nested_build_result
    OUTPUT_VARIABLE nested_build_output
    ERROR_VARIABLE nested_build_error
)
if(NOT nested_build_result EQUAL 0 OR NOT EXISTS "${nested_ir_path}")
    file(REMOVE "${nested_ir_path}" "${nested_executable_path}")
    message(FATAL_ERROR
        "O3 nested-loop regression build failed.\nResult: ${nested_build_result}\n"
        "Output:\n${nested_build_output}\n${nested_build_error}")
endif()
file(READ "${nested_ir_path}" nested_ir)
file(REMOVE "${nested_ir_path}" "${nested_executable_path}")
string(REGEX MATCH
    "add [^\n]*i32 %column[^,\n]*, 1"
    nested_scalar_increment "${nested_ir}")
string(REGEX MATCH
    "add [^\n]*i32 %column[^,\n]*, 4"
    nested_forced_increment "${nested_ir}")
if(nested_scalar_increment STREQUAL "" OR
   NOT nested_forced_increment STREQUAL "")
    message(FATAL_ERROR
        "-O3 forced four-way unrolling on the small nested recurrence.\n"
        "IR:\n${nested_ir}")
endif()
