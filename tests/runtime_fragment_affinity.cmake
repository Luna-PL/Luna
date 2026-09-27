cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED LUNA_FRAGMENT_BENCHMARK_EXECUTABLE)
    message(FATAL_ERROR "LUNA_FRAGMENT_BENCHMARK_EXECUTABLE is required")
endif()

# Capability lookup is read-only. Selecting the first allowed CPU here is only
# a correctness smoke test, not a performance experiment or topology policy.
execute_process(COMMAND "${LUNA_FRAGMENT_BENCHMARK_EXECUTABLE}" --affinity-info
    RESULT_VARIABLE status OUTPUT_VARIABLE info ERROR_VARIABLE errors TIMEOUT 30)
if(NOT status EQUAL 0 OR NOT errors STREQUAL "")
    message(FATAL_ERROR "affinity query failed (${status}): ${errors}")
endif()
string(REPLACE "\r\n" "\n" info "${info}")
if(DEFINED LUNA_COMPILED_PROBE_EXECUTABLE)
    execute_process(COMMAND "${LUNA_COMPILED_PROBE_EXECUTABLE}" --compiled-fragment-affinity-info
        RESULT_VARIABLE status OUTPUT_VARIABLE compiled_info ERROR_VARIABLE errors TIMEOUT 30)
    string(REPLACE "\r\n" "\n" compiled_info "${compiled_info}")
    if(NOT status EQUAL 0 OR NOT errors STREQUAL "" OR NOT compiled_info STREQUAL info)
        message(FATAL_ERROR "compiled/native affinity capabilities differ or query failed: ${compiled_info}\n${errors}")
    endif()
endif()

function(require_cpu_rejected cpu)
    execute_process(COMMAND "${LUNA_FRAGMENT_BENCHMARK_EXECUTABLE}" --pinned-thread "${cpu}" 1 1
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
    if(NOT status EQUAL 1 OR NOT output STREQUAL "" OR errors STREQUAL "")
        message(FATAL_ERROR "unsupported/disallowed CPU was not rejected: ${cpu}")
    endif()
    if(DEFINED LUNA_COMPILED_PROBE_EXECUTABLE)
        execute_process(COMMAND "${LUNA_COMPILED_PROBE_EXECUTABLE}"
                --compiled-fragment-cost-pinned-thread "${cpu}" 1 1 O0
            RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
        if(NOT status EQUAL 1 OR NOT output STREQUAL "" OR errors STREQUAL "")
            message(FATAL_ERROR "compiled unsupported/disallowed CPU was not rejected: ${cpu}")
        endif()
    endif()
endfunction()

if(info MATCHES "^# protocol=luna[.]fragment-cost[.]affinity-info[.]v1\n# supported=no\n# reason=([a-z_]+)\n$")
    set(reason "${CMAKE_MATCH_1}")
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        if(NOT reason STREQUAL "windows_multiple_processor_groups")
            message(FATAL_ERROR "unexpected Windows affinity limitation: ${reason}")
        endif()
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" OR NOT reason STREQUAL "unsupported_platform")
        message(FATAL_ERROR "unexpected affinity limitation: ${reason}")
    endif()
    require_cpu_rejected(0)
    message(STATUS "Pinned-thread probe rejects unsupported platform/group; default probe is unchanged")
    return()
endif()
if(NOT info MATCHES "^# protocol=luna[.]fragment-cost[.]affinity-info[.]v1\n# supported=yes\n# cpu_limit=([1-9][0-9]*)\n# processor_group=(0|none)\n# allowed_cpus=([0-9,]+)\n$")
    message(FATAL_ERROR "malformed affinity information: ${info}")
endif()
set(cpu_limit "${CMAKE_MATCH_1}")
set(processor_group "${CMAKE_MATCH_2}")
set(allowed_csv "${CMAKE_MATCH_3}")
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    if(NOT processor_group STREQUAL "0" OR NOT cpu_limit MATCHES "^(32|64)$")
        message(FATAL_ERROR "invalid Windows group/mask range")
    endif()
elseif(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" OR
       NOT processor_group STREQUAL "none" OR NOT cpu_limit EQUAL 1024)
    message(FATAL_ERROR "unexpected platform/mask range")
endif()
string(REPLACE "," ";" allowed "${allowed_csv}")
set(previous -1)
foreach(cpu IN LISTS allowed)
    if(NOT cpu MATCHES "^(0|[1-9][0-9]*)$" OR
       cpu LESS_EQUAL previous OR cpu GREATER_EQUAL cpu_limit)
        message(FATAL_ERROR "invalid/duplicate/unordered CPU: ${cpu}")
    endif()
    set(previous "${cpu}")
endforeach()
list(GET allowed 0 first_cpu)
set(warning_option -Werror=dev)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.4)
    set(warning_option -Werror=author)
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" "${warning_option}"
        "-DLUNA_FRAGMENT_BENCHMARK_EXECUTABLE=${LUNA_FRAGMENT_BENCHMARK_EXECUTABLE}"
        -DLUNA_FRAGMENT_BENCHMARK_ITERATIONS=3
        "-DLUNA_FRAGMENT_BENCHMARK_LOGICAL_CPU=${first_cpu}"
        -P "${CMAKE_CURRENT_LIST_DIR}/runtime_fragment_benchmark.cmake"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 180)
if(NOT status EQUAL 0 OR NOT errors STREQUAL "")
    message(FATAL_ERROR "pinned-thread correctness smoke failed (${status}): ${output}\n${errors}")
endif()
if(DEFINED LUNA_COMPILED_PROBE_EXECUTABLE)
    foreach(profile IN ITEMS O0 O2 O3)
        execute_process(COMMAND "${CMAKE_COMMAND}" "${warning_option}"
                "-DLUNA_COMPILED_PROBE_EXECUTABLE=${LUNA_COMPILED_PROBE_EXECUTABLE}"
                -DLUNA_COMPILED_PROBE_ITERATIONS=3 "-DLUNA_COMPILED_PROBE_PROFILE=${profile}"
                "-DLUNA_COMPILED_PROBE_LOGICAL_CPU=${first_cpu}"
                -P "${CMAKE_CURRENT_LIST_DIR}/compiled_fragment_benchmark.cmake"
            RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 180)
        if(NOT status EQUAL 0 OR NOT errors STREQUAL "")
            message(FATAL_ERROR "compiled pinned ${profile} smoke failed (${status}): ${output}\n${errors}")
        endif()
    endforeach()
endif()
# Exercise a second explicit index when possible, so CPU 0 is not an implicit
# success assumption. The probe independently checks counters for every sample.
list(GET allowed -1 last_allowed_cpu)
if(NOT last_allowed_cpu STREQUAL first_cpu)
    execute_process(COMMAND "${LUNA_FRAGMENT_BENCHMARK_EXECUTABLE}"
            --pinned-thread "${last_allowed_cpu}" 1 1
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
    string(FIND "${output}"
        "# affinity=measurement_thread,logical_cpu=${last_allowed_cpu},processor_group=${processor_group},verified=sample_boundaries,power_policy=uncontrolled"
        affinity_at)
    if(NOT status EQUAL 0 OR NOT errors STREQUAL "" OR affinity_at EQUAL -1 OR
       NOT output MATCHES "# verified_samples=30,position_balanced=no")
        message(FATAL_ERROR "second allowed CPU failed: ${last_allowed_cpu}: ${output}\n${errors}")
    endif()
    if(DEFINED LUNA_COMPILED_PROBE_EXECUTABLE)
        execute_process(COMMAND "${LUNA_COMPILED_PROBE_EXECUTABLE}"
                --compiled-fragment-cost-pinned-thread "${last_allowed_cpu}" 1 1 O0
            RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
        string(FIND "${output}"
            "# affinity=measurement_thread,logical_cpu=${last_allowed_cpu},processor_group=${processor_group},verified=sample_boundaries,power_policy=uncontrolled"
            affinity_at)
        if(NOT status EQUAL 0 OR NOT errors STREQUAL "" OR affinity_at EQUAL -1 OR
           NOT output MATCHES "# verified_samples=9,position_balanced=no")
            message(FATAL_ERROR "compiled second allowed CPU failed: ${last_allowed_cpu}: ${output}\n${errors}")
        endif()
    endif()
endif()
# A disallowed in-range CPU is preferable; a full allowed mask still has a
# representational boundary that must reject without falling back to any CPU.
set(disallowed "${cpu_limit}")
math(EXPR last_cpu "${cpu_limit} - 1")
foreach(cpu RANGE 0 ${last_cpu})
    list(FIND allowed "${cpu}" at)
    if(at EQUAL -1)
        set(disallowed "${cpu}")
        break()
    endif()
endforeach()
require_cpu_rejected("${disallowed}")
message(STATUS "Pinned-thread smoke: CPU ${first_cpu}, 900 verified samples; disallowed CPU rejected; no timing threshold")
if(DEFINED LUNA_COMPILED_PROBE_EXECUTABLE)
    message(STATUS "Compiled pinned-thread smoke: O0/O2/O3, 243 samples, distinct mode/control provenance; no timing threshold")
endif()
