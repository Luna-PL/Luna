cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED LUNA_FRAGMENT_BENCHMARK_EXECUTABLE)
    message(FATAL_ERROR "LUNA_FRAGMENT_BENCHMARK_EXECUTABLE is required")
endif()
if(NOT DEFINED LUNA_FRAGMENT_BENCHMARK_ITERATIONS)
    set(LUNA_FRAGMENT_BENCHMARK_ITERATIONS 3)
endif()
if(NOT LUNA_FRAGMENT_BENCHMARK_ITERATIONS MATCHES "^[0-9]+$" OR
   LUNA_FRAGMENT_BENCHMARK_ITERATIONS LESS 1 OR
   LUNA_FRAGMENT_BENCHMARK_ITERATIONS GREATER 10000000)
    message(FATAL_ERROR "benchmark iterations must be 1..10000000")
endif()

set(probe_arguments --interleaved)
set(expected_protocol luna.fragment-cost.interleaved.v1)
set(expected_affinity "# affinity=uncontrolled,power_policy=uncontrolled")
if(DEFINED LUNA_FRAGMENT_BENCHMARK_LOGICAL_CPU)
    if(NOT LUNA_FRAGMENT_BENCHMARK_LOGICAL_CPU MATCHES "^(0|[1-9][0-9]*)$" OR
       LUNA_FRAGMENT_BENCHMARK_LOGICAL_CPU GREATER 1023)
        message(FATAL_ERROR "logical CPU must be a canonical integer in 0..1023")
    endif()
    set(probe_arguments --pinned-thread "${LUNA_FRAGMENT_BENCHMARK_LOGICAL_CPU}")
    set(expected_protocol luna.fragment-cost.pinned-thread.v1)
    set(processor_group none)
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        set(processor_group 0)
    endif()
    set(expected_affinity "# affinity=measurement_thread,logical_cpu=${LUNA_FRAGMENT_BENCHMARK_LOGICAL_CPU},processor_group=${processor_group},verified=sample_boundaries,power_policy=uncontrolled")
endif()

# These checks are protocol/correctness gates, never latency thresholds.
execute_process(
    COMMAND "${LUNA_FRAGMENT_BENCHMARK_EXECUTABLE}" ${probe_arguments}
        "${LUNA_FRAGMENT_BENCHMARK_ITERATIONS}" 30
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors
    TIMEOUT 180)
if(NOT status EQUAL 0 OR NOT errors STREQUAL "")
    message(FATAL_ERROR "interleaved probe failed (${status}): ${errors}")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../benchmarks/runtime_fragment_benchmark.cpp"
    expected_source_hash)
foreach(required IN ITEMS
        "# protocol=${expected_protocol}"
        "# probe_sha256=${expected_source_hash}"
        "${expected_affinity}"
        "round,position,fragment_rows,case,ns_per_op,checksum,continuation_calls,fragment_calls"
        "# verified_samples=900,position_balanced=yes")
    string(FIND "${output}" "${required}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "probe lost metadata/summary: ${required}")
    endif()
endforeach()
foreach(key IN ITEMS git_commit build_type compiler cxx)
    if(NOT output MATCHES "# ${key}=[^\r\n]+")
        message(FATAL_ERROR "probe lost build metadata: ${key}")
    endif()
endforeach()
set(warmup ${LUNA_FRAGMENT_BENCHMARK_ITERATIONS})
if(warmup GREATER 1000)
    set(warmup 1000)
endif()
string(FIND "${output}" "# iterations=${LUNA_FRAGMENT_BENCHMARK_ITERATIONS},warmup=${warmup},rounds=30" configuration_at)
if(configuration_at EQUAL -1)
    message(FATAL_ERROR "probe reported a different sampling configuration")
endif()
math(EXPR calls "${LUNA_FRAGMENT_BENCHMARK_ITERATIONS} + ${warmup}")
math(EXPR activation_checksum "(${LUNA_FRAGMENT_BENCHMARK_ITERATIONS} + 1) / 2 + (${warmup} + 1) / 2")
math(EXPR four_calls "4 * ${calls}")
math(EXPR two_calls "2 * ${calls}")
set(case_names candidate_snapshot ref_plus_binding_set refs_plus_chain_4
    local_override_none safe_point_activate_and_pin dispatch_none dispatch_one
    dispatch_chain_2 dispatch_chain_4 dispatch_override_none)
set(row_counts 4 64 256)
set(checksums ${four_calls} ${calls} ${four_calls} ${calls} ${activation_checksum} 0 0 0 0 0)
set(continuations 0 0 0 0 0 ${calls} ${calls} ${calls} ${calls} ${calls})
set(fragments 0 0 0 0 0 0 ${calls} ${two_calls} ${four_calls} 0)
string(REPLACE "\r\n" "\n" normalized "${output}")
string(REPLACE "\n" ";" lines "${normalized}")
set(samples 0)
foreach(line IN LISTS lines)
    if(line MATCHES "^[0-9]")
        if(NOT line MATCHES "^[0-9]+,[0-9]+,[0-9]+,[a-z0-9_]+,[0-9]+[.][0-9],[0-9]+,[0-9]+,[0-9]+$")
            message(FATAL_ERROR "malformed sample: ${line}")
        endif()
        math(EXPR round "${samples} / 30 + 1")
        math(EXPR position "${samples} % 30 + 1")
        math(EXPR pair "(${position} - 1 + (${round} - 1) * 7) % 30")
        math(EXPR row_index "${pair} / 10")
        math(EXPR case_index "${pair} % 10")
        list(GET row_counts ${row_index} rows)
        list(GET case_names ${case_index} case_name)
        list(GET checksums ${case_index} checksum)
        list(GET continuations ${case_index} continuation_count)
        list(GET fragments ${case_index} fragment_count)
        if(NOT line MATCHES "^${round},${position},${rows},${case_name},[0-9]+[.][0-9],${checksum},${continuation_count},${fragment_count}$")
            message(FATAL_ERROR "sample schedule/counters mismatch: ${line}")
        endif()
        # Independent enumeration proves that each pair visits each position.
        set(visit "${pair}_${position}")
        if(DEFINED seen_${visit})
            message(FATAL_ERROR "duplicate pair/position: ${visit}")
        endif()
        set(seen_${visit} TRUE)
        math(EXPR samples "${samples} + 1")
    elseif(NOT line STREQUAL "" AND NOT line MATCHES "^#" AND
           NOT line MATCHES "^round,position,")
        message(FATAL_ERROR "unexpected probe output: ${line}")
    endif()
endforeach()
if(NOT samples EQUAL 900)
    message(FATAL_ERROR "probe emitted ${samples} samples, expected 900")
endif()

execute_process(COMMAND "${LUNA_FRAGMENT_BENCHMARK_EXECUTABLE}" ${probe_arguments} 1 1
    RESULT_VARIABLE status OUTPUT_VARIABLE partial ERROR_VARIABLE errors TIMEOUT 30)
if(NOT status EQUAL 0 OR NOT errors STREQUAL "" OR
   NOT partial MATCHES "# verified_samples=30,position_balanced=no")
    message(FATAL_ERROR "partial schedule incorrectly reported balance: ${partial}\n${errors}")
endif()
foreach(arguments IN ITEMS "-1" "3junk" "18446744073709551616" "1|3" "1|4097" "1|4|extra"
        "--interleaved|0|30" "--interleaved|10000001|30" "--interleaved|3|0"
        "--interleaved|3|-1" "--interleaved|3|301"
        "--interleaved|3|30|extra" "--affinity-info|extra"
        "--pinned-thread" "--pinned-thread|-1" "--pinned-thread|01"
        "--pinned-thread|3junk" "--pinned-thread|18446744073709551616"
        "--pinned-thread|1024" "--pinned-thread|0|0|30"
        "--pinned-thread|0|10000001|30" "--pinned-thread|0|3|0"
        "--pinned-thread|0|3|-1" "--pinned-thread|0|3|301"
        "--pinned-thread|0|3|30|extra")
    string(REPLACE "|" ";" arguments "${arguments}")
    execute_process(COMMAND "${LUNA_FRAGMENT_BENCHMARK_EXECUTABLE}" ${arguments}
        RESULT_VARIABLE status OUTPUT_VARIABLE invalid_output ERROR_VARIABLE errors TIMEOUT 10)
    if(NOT status EQUAL 1 OR NOT invalid_output STREQUAL "" OR errors STREQUAL "")
        message(FATAL_ERROR "invalid CLI accepted: ${arguments}")
    endif()
endforeach()
if(DEFINED LUNA_FRAGMENT_BENCHMARK_RECORD)
    # Explicit caller-selected generated build artifact, written after validation.
    file(WRITE "${LUNA_FRAGMENT_BENCHMARK_RECORD}" "${output}")
endif()
message(STATUS "${expected_protocol}: 900 samples, exact counters, balanced positions; no timing threshold")
