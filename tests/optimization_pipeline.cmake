# Prove that -O2 is not merely a command-line spelling: compare emitted IR
# with -O0, then execute both optimized JIT and AOT paths.  O3 is also built
# to keep the public optimization-level surface covered.

if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()
include("${LUNA_SOURCE_DIR}/tests/aot_package_fixture.cmake")

set(source_path "${LUNA_SOURCE_DIR}/tests/fixtures/optimization_constant_fold.luna")
luna_stage_aot_application("${source_path}" "optimization_constant_fold")
set(ir_path "${LUNA_AOT_IR_PATH}")
set(executable_path "${LUNA_AOT_EXECUTABLE_PATH}")
set(object_path "${executable_path}.o")
set(package_path "${LUNA_AOT_PACKAGE_DIR}")

function(cleanup_outputs)
    file(REMOVE "${ir_path}" "${object_path}" "${executable_path}")
endfunction()

function(build_and_read level output_var)
    cleanup_outputs()
    execute_process(
        COMMAND "${LUNA_EXECUTABLE}" build "${package_path}" "${level}"
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
    string(FIND "${build_output}\n${build_error}"
        "-Xclang -disable-llvm-passes" disable_llvm_passes_at)
    string(FIND "${build_output}\n${build_error}"
        "Emitting native object:" native_object_at)
    if(NOT disable_llvm_passes_at EQUAL -1 OR native_object_at EQUAL -1)
        cleanup_outputs()
        message(FATAL_ERROR
            "${level} did not use Luna's direct native-object path.\n"
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
# The structured path names locals by source name (%left); the canonical
# CFG path names them by LocalId (%local.0.left). Both retain an alloca
# for the local at -O0.
string(FIND "${o0_ir}" "alloca i32" o0_stack_slot)
if(o0_stack_slot EQUAL -1)
    cleanup_outputs()
    message(FATAL_ERROR "expected unoptimized IR to retain the local stack slot.\nIR:\n${o0_ir}")
endif()

build_and_read(-O2 o2_ir)
string(FIND "${o2_ir}" "alloca i32" o2_stack_slot)
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
string(FIND "${o3_ir}" "rt_install_application_host_services_v1"
    pure_host_install_at)
if(NOT pure_host_install_at EQUAL -1)
    cleanup_outputs()
    message(FATAL_ERROR
        "pure application retained the heavyweight host-service profile.\nIR:\n${o3_ir}")
endif()
cleanup_outputs()
string(FIND "${o3_ir}" "ret i32 42" o3_constant_return)
if(o3_constant_return EQUAL -1)
    message(FATAL_ERROR "-O3 did not preserve the expected constant result.\nIR:\n${o3_ir}")
endif()

# Small straight-line, call-free while loops receive a conservative four-way
# O3 unroll hint. The reduction workload keeps the recurrence live and makes
# the selected count directly visible in optimized IR.
set(loop_source_path "${LUNA_SOURCE_DIR}/benchmarks/luna_cpu_reduction.luna")
luna_stage_aot_application("${loop_source_path}" "optimization_reduction")
set(loop_ir_path "${LUNA_AOT_IR_PATH}")
set(loop_executable_path "${LUNA_AOT_EXECUTABLE_PATH}")
set(loop_package_path "${LUNA_AOT_PACKAGE_DIR}")
file(REMOVE "${loop_ir_path}" "${loop_executable_path}")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${loop_package_path}" -O3
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
        "canonical O3 did not apply the expected four-way straight-line loop unroll.\n"
        "IR:\n${loop_ir}")
endif()

# A tiny single-recurrence inner loop is intentionally below Luna's explicit
# unroll heuristic. A target-aware LLVM cost model may still choose its own
# profitable factor; Luna must not force the heuristic's four-way count.
set(nested_source_path "${LUNA_SOURCE_DIR}/benchmarks/luna_cpu_nested.luna")
luna_stage_aot_application("${nested_source_path}" "optimization_nested")
set(nested_ir_path "${LUNA_AOT_IR_PATH}")
set(nested_executable_path "${LUNA_AOT_EXECUTABLE_PATH}")
set(nested_package_path "${LUNA_AOT_PACKAGE_DIR}")
file(REMOVE "${nested_ir_path}" "${nested_executable_path}")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${nested_package_path}" -O3
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
    "add [^\n]*i32 %local\\.[0-9]+\\.column[^,\n]*, 4"
    nested_forced_increment "${nested_ir}")
if(NOT nested_forced_increment STREQUAL "")
    message(FATAL_ERROR
        "canonical O3 forced four-way unrolling on the small nested recurrence.\n"
        "IR:\n${nested_ir}")
endif()

# Safe fixed-array access must not force an out-of-line Runtime call through a
# hot loop. Direct range proofs cover masked/constant indices; the inline
# fallback lets LLVM use dominating loop and short-circuit conditions while
# retaining the original Runtime diagnostic on the failure edge.
function(assert_no_optimized_array_guard source_path fixture_name)
    luna_stage_aot_application("${source_path}" "${fixture_name}")
    set(array_ir_path "${LUNA_AOT_IR_PATH}")
    set(array_executable_path "${LUNA_AOT_EXECUTABLE_PATH}")
    execute_process(
        COMMAND "${LUNA_EXECUTABLE}" build "${LUNA_AOT_PACKAGE_DIR}" -O3
        RESULT_VARIABLE array_build_result
        OUTPUT_VARIABLE array_build_output
        ERROR_VARIABLE array_build_error
    )
    if(NOT array_build_result EQUAL 0 OR NOT EXISTS "${array_ir_path}")
        file(REMOVE "${array_ir_path}" "${array_executable_path}")
        message(FATAL_ERROR
            "O3 array-guard regression build failed for ${fixture_name}.\n"
            "Result: ${array_build_result}\n"
            "Output:\n${array_build_output}\n${array_build_error}")
    endif()
    file(READ "${array_ir_path}" array_ir)
    if(ARGC GREATER 2)
        file(SIZE "${array_ir_path}" array_ir_size)
        if(array_ir_size GREATER ARGV2)
            file(REMOVE "${array_ir_path}" "${array_executable_path}")
            message(FATAL_ERROR
                "O3 array IR for ${fixture_name} grew to ${array_ir_size} bytes; "
                "limit is ${ARGV2}")
        endif()
    endif()
    file(REMOVE "${array_ir_path}" "${array_executable_path}")
    string(REGEX MATCH
        "call [^\n]*@rt_array_index_or_abort"
        remaining_array_guard "${array_ir}")
    if(NOT remaining_array_guard STREQUAL "")
        message(FATAL_ERROR
            "O3 retained a hot array Runtime guard for ${fixture_name}.\n"
            "IR:\n${array_ir}")
    endif()
    if(ARGC GREATER 2)
        string(REGEX MATCH "target triple = \"x86_64-" x86_host "${array_ir}")
        if(NOT x86_host STREQUAL "")
            string(REGEX MATCH
                "array\\.load[^\n]*\\.3 = load"
                target_cost_unroll "${array_ir}")
            if(target_cost_unroll STREQUAL "")
                message(FATAL_ERROR
                    "O3 did not use the x86-64 target cost model for ${fixture_name}; "
                    "the four-way search-loop unroll is missing.\nIR:\n${array_ir}")
            endif()
        endif()
    endif()
endfunction()

assert_no_optimized_array_guard(
    "${LUNA_SOURCE_DIR}/benchmarks/luna_cpu_array.luna"
    "optimization_array_mask")
assert_no_optimized_array_guard(
    "${LUNA_SOURCE_DIR}/benchmarks/luna_cpu_find.luna"
    "optimization_array_dominating_bound"
    131072)

# Contextually typed unsigned literals must survive semantic lowering and
# select unsigned LLVM operations. This is both a correctness contract and a
# prerequisite for target rotate/bit-manipulation combines.
set(unsigned_source_path
    "${LUNA_SOURCE_DIR}/tests/fixtures/unsigned_integer_codegen.luna")
luna_stage_aot_application("${unsigned_source_path}" "optimization_unsigned")
set(unsigned_ir_path "${LUNA_AOT_IR_PATH}")
set(unsigned_executable_path "${LUNA_AOT_EXECUTABLE_PATH}")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${LUNA_AOT_PACKAGE_DIR}" -O0
    RESULT_VARIABLE unsigned_build_result
    OUTPUT_VARIABLE unsigned_build_output
    ERROR_VARIABLE unsigned_build_error)
if(NOT unsigned_build_result EQUAL 0 OR NOT EXISTS "${unsigned_ir_path}")
    file(REMOVE "${unsigned_ir_path}" "${unsigned_executable_path}")
    message(FATAL_ERROR
        "unsigned integer regression build failed.\n"
        "Output:\n${unsigned_build_output}\n${unsigned_build_error}")
endif()
file(READ "${unsigned_ir_path}" unsigned_ir)
foreach(unsigned_instruction
        "udiv i32" "urem i32" "lshr i32" "icmp ugt i32"
        "call i32 @llvm.ctpop.i32" "call void @rt_print_u32")
    string(FIND "${unsigned_ir}" "${unsigned_instruction}"
        unsigned_instruction_at)
    if(unsigned_instruction_at EQUAL -1)
        file(REMOVE "${unsigned_ir_path}" "${unsigned_executable_path}")
        message(FATAL_ERROR
            "unsigned source did not lower '${unsigned_instruction}'.\n"
            "IR:\n${unsigned_ir}")
    endif()
endforeach()
execute_process(
    COMMAND "${unsigned_executable_path}"
    RESULT_VARIABLE unsigned_run_result
    OUTPUT_VARIABLE unsigned_run_output
    ERROR_VARIABLE unsigned_run_error)
file(REMOVE "${unsigned_ir_path}" "${unsigned_executable_path}")
if(NOT unsigned_run_result EQUAL 0 OR
   NOT unsigned_run_output STREQUAL
       "1431655765\n0\n1\n32\n4294967295\n" OR
   NOT unsigned_run_error STREQUAL "")
    message(FATAL_ERROR
        "unsigned AOT execution disagrees with u32 semantics.\n"
        "stdout=${unsigned_run_output}\nstderr=${unsigned_run_error}")
endif()

# The rotate workload previously repeated a signed arithmetic shift and
# reached x86 as SHLD. Unsigned right-shift semantics expose a canonical rotate
# to LLVM, which the target backend can select as one ROL instruction.
set(rotate_source_path "${LUNA_SOURCE_DIR}/benchmarks/luna_cpu_rotate.luna")
luna_stage_aot_application("${rotate_source_path}" "optimization_rotate")
set(rotate_ir_path "${LUNA_AOT_IR_PATH}")
set(rotate_executable_path "${LUNA_AOT_EXECUTABLE_PATH}")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${LUNA_AOT_PACKAGE_DIR}" -O3
    RESULT_VARIABLE rotate_build_result
    OUTPUT_VARIABLE rotate_build_output
    ERROR_VARIABLE rotate_build_error)
if(NOT rotate_build_result EQUAL 0 OR NOT EXISTS "${rotate_ir_path}")
    file(REMOVE "${rotate_ir_path}" "${rotate_executable_path}")
    message(FATAL_ERROR
        "O3 rotate regression build failed.\n"
        "Output:\n${rotate_build_output}\n${rotate_build_error}")
endif()
file(READ "${rotate_ir_path}" rotate_ir)
file(REMOVE "${rotate_ir_path}" "${rotate_executable_path}")
string(FIND "${rotate_ir}"
    "@llvm.fshl.i32(i32 %bitxortmp, i32 %bitxortmp, i32 7)"
    canonical_rotate_at)
if(canonical_rotate_at EQUAL -1)
    message(FATAL_ERROR
        "O3 did not expose the unsigned rotate as canonical fshl.\n"
        "IR:\n${rotate_ir}")
endif()
