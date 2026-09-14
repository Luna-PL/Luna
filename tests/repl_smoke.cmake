if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()
if(NOT DEFINED LUNA_REPL_PROCESS_TREE_LIBRARY OR
   NOT EXISTS "${LUNA_REPL_PROCESS_TREE_LIBRARY}")
    message(FATAL_ERROR
        "LUNA_REPL_PROCESS_TREE_LIBRARY must point at the test helper")
endif()
if(NOT DEFINED LUNA_REPL_DESCENDANT_EXECUTABLE OR
   NOT EXISTS "${LUNA_REPL_DESCENDANT_EXECUTABLE}")
    message(FATAL_ERROR
        "LUNA_REPL_DESCENDANT_EXECUTABLE must point at the descendant helper")
endif()
if(NOT DEFINED LUNA_REPL_DELAYED_INPUT_EXECUTABLE OR
   NOT EXISTS "${LUNA_REPL_DELAYED_INPUT_EXECUTABLE}")
    message(FATAL_ERROR
        "LUNA_REPL_DELAYED_INPUT_EXECUTABLE must point at the delayed-input helper")
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" repl --no-prompt
    INPUT_FILE "${LUNA_SOURCE_DIR}/tests/fixtures/repl_session.txt"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
)
set(transcript "${output}\n${errors}")
foreach(expected
        "Multiline cells require explicit :paste"
        "declaration stored"
        "= 42"
        "7"
        "ok"
        "declarations reset")
    string(FIND "${transcript}" "${expected}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR
            "REPL transcript did not contain '${expected}'.\n${transcript}")
    endif()
endforeach()
if(NOT result EQUAL 0 OR transcript MATCHES "error\\[" OR
   transcript MATCHES "timing\\[repl\\]")
    message(FATAL_ERROR
        "REPL smoke session failed (${result}).\n${transcript}")
endif()

if(transcript MATCHES "luna> ")
    message(FATAL_ERROR "--no-prompt still emitted a REPL prompt.\n${transcript}")
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" repl --help
    RESULT_VARIABLE help_result
    OUTPUT_VARIABLE help_output
    ERROR_VARIABLE help_errors
)
if(NOT help_result EQUAL 0 OR
   NOT help_output MATCHES "Usage: luna repl \\[options\\]" OR
   NOT help_output MATCHES "--timeout" OR
   NOT help_output MATCHES "--memory-limit" OR
   NOT help_output MATCHES "--output-limit" OR
   NOT help_output MATCHES "--timings" OR
   NOT help_output MATCHES "--no-prompt" OR
   help_output MATCHES "Luna Alpha REPL")
    message(FATAL_ERROR
        "REPL CLI help contract failed (${help_result}).\n${help_output}\n${help_errors}")
endif()

set(timings_input_path "${CMAKE_CURRENT_BINARY_DIR}/repl-timings-input.txt")
file(WRITE "${timings_input_path}"
    ":type 1\n:type 1\n:decl fn timing_identity(value: i32) -> i32 { return value; }\n:type 1\n= 1 + 1\n:quit\n")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" repl --timings --no-prompt
    INPUT_FILE "${timings_input_path}"
    RESULT_VARIABLE timings_result
    OUTPUT_VARIABLE timings_output
    ERROR_VARIABLE timings_errors
)
string(REGEX MATCHALL
    "timing\\[repl\\]: operation=type cache=miss"
    timing_type_cache_misses "${timings_errors}")
list(LENGTH timing_type_cache_misses timing_type_cache_miss_count)
string(REGEX MATCHALL
    "timing\\[repl\\]: operation=type cache=hit prewarmed=unused"
    timing_type_cache_hits "${timings_errors}")
list(LENGTH timing_type_cache_hits timing_type_cache_hit_count)
if(NOT timings_result EQUAL 0 OR
   NOT timings_output MATCHES "= i32" OR
   NOT timings_output MATCHES "= 2" OR
   NOT timing_type_cache_miss_count EQUAL 2 OR
   NOT timing_type_cache_hit_count EQUAL 1 OR
   NOT timings_errors MATCHES
       "timing\\[repl\\]: operation=type cache=hit prewarmed=unused frontend=0us lexer=0us parser=0us semantic=0us traits=0us ownership=0us indexing=0us lowering=0us verification=0us sealing=0us moon-opt=0us codegen=0us codegen-setup=0us codegen-total=0us jit-materialize=0us jit-lookup=0us execution=0us jit-cleanup=0us jit-total=0us worker-request=0us worker-link=0us worker-running-publish=0us worker-other=0us worker-total=0us parent-cache=[0-9]+us parent-acquire=0us parent-submit=0us parent-roundtrip=0us parent-roundtrip-gap=0us parent-collect=0us parent-replenish=0us overhead=[0-9]+us total=[0-9]+us" OR
   NOT timings_errors MATCHES
       "timing\\[repl\\]: operation=type cache=miss prewarmed=(yes|no) frontend=[0-9]+us lexer=[0-9]+us parser=[0-9]+us semantic=[0-9]+us traits=[0-9]+us ownership=[0-9]+us indexing=[0-9]+us lowering=[0-9]+us verification=[0-9]+us sealing=[0-9]+us moon-opt=[0-9]+us codegen=0us codegen-setup=0us codegen-total=0us jit-materialize=0us jit-lookup=0us execution=0us jit-cleanup=0us jit-total=0us worker-request=[0-9]+us worker-link=[0-9]+us worker-running-publish=0us worker-other=[0-9]+us worker-total=[0-9]+us parent-cache=0us parent-acquire=[0-9]+us parent-submit=[0-9]+us parent-roundtrip=[0-9]+us parent-roundtrip-gap=[0-9]+us parent-collect=[0-9]+us parent-replenish=[0-9]+us overhead=[0-9]+us total=[0-9]+us" OR
   NOT timings_errors MATCHES
       "timing\\[repl\\]: operation=run cache=miss prewarmed=(yes|no) frontend=[0-9]+us lexer=[0-9]+us parser=[0-9]+us semantic=[0-9]+us traits=[0-9]+us ownership=[0-9]+us indexing=[0-9]+us lowering=[0-9]+us verification=[0-9]+us sealing=[0-9]+us moon-opt=[0-9]+us codegen=[0-9]+us codegen-setup=[0-9]+us codegen-total=[0-9]+us jit-materialize=[0-9]+us jit-lookup=[0-9]+us execution=[0-9]+us jit-cleanup=[0-9]+us jit-total=[0-9]+us worker-request=[0-9]+us worker-link=[0-9]+us worker-running-publish=[0-9]+us worker-other=[0-9]+us worker-total=[0-9]+us parent-cache=0us parent-acquire=[0-9]+us parent-submit=[0-9]+us parent-roundtrip=[0-9]+us parent-roundtrip-gap=[0-9]+us parent-collect=[0-9]+us parent-replenish=[0-9]+us overhead=[0-9]+us total=[0-9]+us")
    message(FATAL_ERROR
        "REPL timing report failed (${timings_result}).\n${timings_output}\n${timings_errors}")
endif()

execute_process(
    COMMAND "${LUNA_REPL_DELAYED_INPUT_EXECUTABLE}"
    COMMAND "${LUNA_EXECUTABLE}" repl --timings --no-prompt
    RESULT_VARIABLE prewarm_result
    OUTPUT_VARIABLE prewarm_output
    ERROR_VARIABLE prewarm_errors
)
if(NOT prewarm_result EQUAL 0 OR
   NOT prewarm_output MATCHES "= 23" OR
   NOT prewarm_errors MATCHES
       "timing\\[repl\\]: operation=run cache=miss prewarmed=yes")
    message(FATAL_ERROR
        "REPL did not use its idle-time prewarmed worker (${prewarm_result}).\n"
        "${prewarm_output}\n${prewarm_errors}")
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" repl --memory-limit=128 --no-prompt
    RESULT_VARIABLE invalid_memory_limit_result
    OUTPUT_VARIABLE invalid_memory_limit_output
    ERROR_VARIABLE invalid_memory_limit_errors
)
if(invalid_memory_limit_result EQUAL 0 OR
   NOT invalid_memory_limit_errors MATCHES "expected 256\\.\\.65536 MiB")
    message(FATAL_ERROR
        "invalid REPL memory limit was accepted.\n${invalid_memory_limit_output}\n${invalid_memory_limit_errors}")
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" repl --output-limit=0 --no-prompt
    RESULT_VARIABLE invalid_output_limit_result
    OUTPUT_VARIABLE invalid_output_limit_output
    ERROR_VARIABLE invalid_output_limit_errors
)
if(invalid_output_limit_result EQUAL 0 OR
   NOT invalid_output_limit_errors MATCHES "expected 1\\.\\.1024 MiB")
    message(FATAL_ERROR
        "invalid REPL output limit was accepted.\n${invalid_output_limit_output}\n${invalid_output_limit_errors}")
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" repl --timeout=0 --no-prompt
    RESULT_VARIABLE invalid_timeout_result
    OUTPUT_VARIABLE invalid_timeout_output
    ERROR_VARIABLE invalid_timeout_errors
)
if(invalid_timeout_result EQUAL 0 OR
   NOT invalid_timeout_errors MATCHES "expected 1\\.\\.3600 seconds")
    message(FATAL_ERROR
        "invalid REPL timeout was accepted.\n${invalid_timeout_output}\n${invalid_timeout_errors}")
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" repl --unknown-repl-option
    RESULT_VARIABLE invalid_option_result
    OUTPUT_VARIABLE invalid_option_output
    ERROR_VARIABLE invalid_option_errors
)
if(invalid_option_result EQUAL 0 OR
   NOT invalid_option_errors MATCHES "Unknown REPL option")
    message(FATAL_ERROR
        "invalid REPL CLI option was accepted.\n${invalid_option_output}\n${invalid_option_errors}")
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" __repl-worker --ready=not-a-signal
    RESULT_VARIABLE invalid_worker_signal_result
    OUTPUT_VARIABLE invalid_worker_signal_output
    ERROR_VARIABLE invalid_worker_signal_errors
)
if(invalid_worker_signal_result EQUAL 0 OR
   NOT invalid_worker_signal_errors MATCHES "Invalid REPL worker ready signal")
    message(FATAL_ERROR
        "invalid internal REPL worker signal was accepted.\n"
        "${invalid_worker_signal_output}\n${invalid_worker_signal_errors}")
endif()

set(boundary_input [=[
:decl
:bogus
:undo
:decl fn main() -> i32 { return 3; }
:decl fn keep(value: i32) -> i32 { return value; }
:type keep(1)
= keep(4)
:undo
= keep(4)
:paste decl
fn sum(
    left: i32,
    right: i32
) -> i32 {
    return left + right;
}
:end
:paste expr
sum(
    20,
    22
)
:end
:paste stmt
print(
    8
)
:end
= 6
:quit trailing
:quit
]=])
set(boundary_input_path "${CMAKE_CURRENT_BINARY_DIR}/repl-boundary-input.txt")
file(WRITE "${boundary_input_path}" "${boundary_input}")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" repl -O2 --no-prompt
    INPUT_FILE "${boundary_input_path}"
    RESULT_VARIABLE boundary_result
    OUTPUT_VARIABLE boundary_output
    ERROR_VARIABLE boundary_errors
)
set(boundary_transcript "${boundary_output}\n${boundary_errors}")
foreach(expected
        ":decl requires a declaration"
        "unknown command ':bogus'"
        "there is no declaration to undo"
        "more than one 'main'"
        "declaration stored"
        "= i32"
        "= 4"
        "last declaration discarded"
        "undefined name 'keep'"
        "= 42"
        "8"
        "ok"
        "= 6"
        ":quit does not accept arguments")
    string(FIND "${boundary_transcript}" "${expected}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR
            "REPL boundary transcript did not contain '${expected}'.\n${boundary_transcript}")
    endif()
endforeach()
if(NOT boundary_result EQUAL 0 OR boundary_output MATCHES "luna> ")
    message(FATAL_ERROR
        "REPL boundary session failed (${boundary_result}).\n${boundary_transcript}")
endif()

# Exercise request streaming beyond the native pipe buffer so the containment
# gate and parent/worker read-write ordering cannot regress into a deadlock.
string(REPEAT "x" 131072 large_request_padding)
set(large_request_input_path
    "${CMAKE_CURRENT_BINARY_DIR}/repl-large-request-input.txt")
file(WRITE "${large_request_input_path}"
    ":paste decl\n// ${large_request_padding}\nfn large_request_value() -> i32 { return 29; }\n:end\n= large_request_value()\n:quit\n")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" repl --no-prompt
    INPUT_FILE "${large_request_input_path}"
    RESULT_VARIABLE large_request_result
    OUTPUT_VARIABLE large_request_output
    ERROR_VARIABLE large_request_errors
    TIMEOUT 8
)
if(NOT large_request_result EQUAL 0 OR
   NOT large_request_output MATCHES "declaration stored" OR
   NOT large_request_output MATCHES "= 29" OR
   large_request_errors MATCHES "error\\[")
    message(FATAL_ERROR
        "large REPL data-channel request failed (${large_request_result}).\n"
        "${large_request_output}\n${large_request_errors}")
endif()

set(load_source_path "${CMAKE_CURRENT_BINARY_DIR}/repl-load-fixture.luna")
file(WRITE "${load_source_path}"
    "fn loaded(value: i32) -> i32 { return value + value; }\n")
file(TO_CMAKE_PATH "${load_source_path}" load_source_argument)
set(load_input_path "${CMAKE_CURRENT_BINARY_DIR}/repl-load-input.txt")
file(WRITE "${load_input_path}"
    ":load ${load_source_argument}\n= loaded(7)\n:quit\n")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" repl --no-prompt
    INPUT_FILE "${load_input_path}"
    RESULT_VARIABLE load_result
    OUTPUT_VARIABLE load_output
    ERROR_VARIABLE load_errors
)
if(NOT load_result EQUAL 0 OR
   NOT load_output MATCHES "declaration stored" OR
   NOT load_output MATCHES "= 14" OR
   load_errors MATCHES "error\\[")
    message(FATAL_ERROR
        "REPL :load session failed (${load_result}).\n${load_output}\n${load_errors}")
endif()

set(timeout_input_path "${CMAKE_CURRENT_BINARY_DIR}/repl-timeout-input.txt")
file(WRITE "${timeout_input_path}"
    "while true {}\n= 7\n= 2147483647\n= -2147483647 - 1\n:quit\n")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" repl --timeout 1 --memory-limit 256 --no-prompt
    INPUT_FILE "${timeout_input_path}"
    RESULT_VARIABLE timeout_result
    OUTPUT_VARIABLE timeout_output
    ERROR_VARIABLE timeout_errors
)
set(timeout_transcript "${timeout_output}\n${timeout_errors}")
if(NOT timeout_result EQUAL 0 OR
   NOT timeout_errors MATCHES "worker compilation or execution exceeded the 1 second timeout" OR
   NOT timeout_output MATCHES "= 7" OR
   NOT timeout_output MATCHES "= 2147483647" OR
   NOT timeout_output MATCHES "= -2147483648")
    message(FATAL_ERROR
        "REPL timeout recovery failed (${timeout_result}).\n${timeout_transcript}")
endif()

set(crash_input_path "${CMAKE_CURRENT_BINARY_DIR}/repl-crash-input.txt")
file(WRITE "${crash_input_path}"
    ":decl fn at(i: i32) -> i32 { let values = [1, 2]; return values[i]; }\n"
    "= at(9)\n= 11\n:quit\n")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" repl --timeout 5 --no-prompt
    INPUT_FILE "${crash_input_path}"
    RESULT_VARIABLE crash_result
    OUTPUT_VARIABLE crash_output
    ERROR_VARIABLE crash_errors
)
set(crash_transcript "${crash_output}\n${crash_errors}")
if(NOT crash_result EQUAL 0 OR
   NOT crash_errors MATCHES "Luna runtime error: array index 9 is outside length 2" OR
   NOT crash_errors MATCHES "error\\[repl\\]: worker" OR
   NOT crash_output MATCHES "= 11")
    message(FATAL_ERROR
        "REPL crash recovery failed (${crash_result}).\n${crash_transcript}")
endif()

set(output_limit_input_path "${CMAKE_CURRENT_BINARY_DIR}/repl-output-limit-input.txt")
file(WRITE "${output_limit_input_path}" [=[
:paste stmt
let index: i32 = 0;
while index < 200000 {
    print(123456789);
    index = index + 1;
}
:end
= 13
:quit
]=])
set(output_limit_stdout_path
    "${CMAKE_CURRENT_BINARY_DIR}/repl-output-limit-stdout.txt")
set(output_limit_stderr_path
    "${CMAKE_CURRENT_BINARY_DIR}/repl-output-limit-stderr.txt")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" repl --timeout 10 --output-limit 1 --no-prompt
    INPUT_FILE "${output_limit_input_path}"
    OUTPUT_FILE "${output_limit_stdout_path}"
    ERROR_FILE "${output_limit_stderr_path}"
    RESULT_VARIABLE output_limit_result
)
file(READ "${output_limit_stderr_path}" output_limit_errors)
file(SIZE "${output_limit_stdout_path}" output_limit_stdout_size)
if(output_limit_stdout_size GREATER 4096)
    math(EXPR output_limit_tail_offset "${output_limit_stdout_size} - 4096")
else()
    set(output_limit_tail_offset 0)
endif()
file(READ "${output_limit_stdout_path}" output_limit_tail
    OFFSET ${output_limit_tail_offset} LIMIT 4096)
if(NOT output_limit_result EQUAL 0 OR
   NOT output_limit_errors MATCHES "worker output was truncated at the 1 MiB limit" OR
   NOT output_limit_errors MATCHES "worker exceeded the 1 MiB output limit" OR
   NOT output_limit_tail MATCHES "= 13")
    message(FATAL_ERROR
        "REPL output-limit recovery failed (${output_limit_result}).\n"
        "stdout tail:\n${output_limit_tail}\nstderr:\n${output_limit_errors}")
endif()

set(process_tree_marker
    "${CMAKE_CURRENT_BINARY_DIR}/repl-descendant-survived.txt")
set(process_tree_fd_leak_marker
    "${CMAKE_CURRENT_BINARY_DIR}/repl-descendant-fd-leak.txt")
file(REMOVE "${process_tree_marker}" "${process_tree_fd_leak_marker}")
set(process_tree_input_path
    "${CMAKE_CURRENT_BINARY_DIR}/repl-process-tree-input.txt")
file(WRITE "${process_tree_input_path}" [=[
:paste decl
extern "C" fn repl_spawn_descendant() -> i32;
extern "C" fn repl_process_local_sequence() -> i32;
extern "C" fn repl_register_slow_process_exit() -> i32;
:end
= repl_process_local_sequence()
= repl_process_local_sequence()
= repl_register_slow_process_exit()
= repl_spawn_descendant()
= 17
:quit
]=])
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "LUNA_REPL_DESCENDANT_EXECUTABLE=${LUNA_REPL_DESCENDANT_EXECUTABLE}"
        "LUNA_REPL_DESCENDANT_MARKER=${process_tree_marker}"
        "LUNA_REPL_DESCENDANT_FD_LEAK_MARKER=${process_tree_fd_leak_marker}"
        "${LUNA_EXECUTABLE}" repl
        --link "${LUNA_REPL_PROCESS_TREE_LIBRARY}" --no-prompt
    INPUT_FILE "${process_tree_input_path}"
    RESULT_VARIABLE process_tree_result
    OUTPUT_VARIABLE process_tree_output
    ERROR_VARIABLE process_tree_errors
    TIMEOUT 8
)
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 3)
string(REGEX MATCHALL "= 1([^0-9]|$)" process_local_sequence_results
    "${process_tree_output}")
list(LENGTH process_local_sequence_results process_local_sequence_count)
if(NOT process_tree_result EQUAL 0 OR
   NOT process_tree_output MATCHES "declaration stored" OR
   NOT process_local_sequence_count EQUAL 2 OR
   NOT process_tree_output MATCHES "= 31" OR
   NOT process_tree_output MATCHES "= 4242" OR
   NOT process_tree_output MATCHES "= 17" OR
   process_tree_errors MATCHES "error\\[" OR
   EXISTS "${process_tree_marker}" OR
   EXISTS "${process_tree_fd_leak_marker}")
    message(FATAL_ERROR
        "REPL process-tree cleanup failed (${process_tree_result}).\n"
        "${process_tree_output}\n${process_tree_errors}")
endif()

if(LUNA_REPL_MEMORY_LIMIT_ENFORCED)
    set(memory_limit_input_path
        "${CMAKE_CURRENT_BINARY_DIR}/repl-memory-limit-input.txt")
    file(WRITE "${memory_limit_input_path}" [=[
:decl extern "C" fn repl_exceed_memory_limit() -> i32;
= repl_exceed_memory_limit()
= 19
:quit
]=])
    execute_process(
        COMMAND "${LUNA_EXECUTABLE}" repl --memory-limit 256
            --link "${LUNA_REPL_PROCESS_TREE_LIBRARY}" --no-prompt
        INPUT_FILE "${memory_limit_input_path}"
        RESULT_VARIABLE memory_limit_result
        OUTPUT_VARIABLE memory_limit_output
        ERROR_VARIABLE memory_limit_errors
    )
    if(NOT memory_limit_result EQUAL 0 OR
       NOT memory_limit_output MATCHES "declaration stored" OR
       NOT memory_limit_output MATCHES "= 19" OR
       NOT memory_limit_errors MATCHES "error\\[repl\\]: worker terminated while running JIT code")
        message(FATAL_ERROR
            "REPL memory-limit recovery failed (${memory_limit_result}).\n"
            "${memory_limit_output}\n${memory_limit_errors}")
    endif()
endif()
