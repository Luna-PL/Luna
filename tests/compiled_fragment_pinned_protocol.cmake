cmake_minimum_required(VERSION 3.20)
include("${CMAKE_CURRENT_LIST_DIR}/compiled_fragment_probe_protocol.cmake")

# Separate raw-record protocol, deliberately not a v1 series/bundle extension.
# Validate controlled metadata first, then reuse the unchanged sample/body gate
# on a derived string. No observed bytes are written or relabeled on disk.
function(luna_validate_compiled_pinned_probe output iterations profile cpu group)
    if(NOT cpu MATCHES "^(0|[1-9][0-9]*)$" OR cpu GREATER 1023 OR
       NOT group MATCHES "^(0|none)$" OR (group STREQUAL "0" AND cpu GREATER 63))
        message(FATAL_ERROR "invalid pinned probe validation configuration")
    endif()
    string(REPLACE "\r\n" "\n" normalized "${output}")
    file(SHA256 "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../benchmarks/fragment_thread_affinity.h" control_hash)
    set(expected_affinity "measurement_thread,logical_cpu=${cpu},processor_group=${group},verified=sample_boundaries,power_policy=uncontrolled")
    foreach(required IN ITEMS "protocol=luna.compiled-fragment-cost.pinned-thread.v1"
            "affinity_control_sha256=${control_hash}" "affinity=${expected_affinity}"
            "setup_affinity=uncontrolled,measurement_scope=dispatch_samples")
        string(REGEX REPLACE "=.*$" "" key "${required}")
        luna_compiled_probe_metadata("${normalized}" "${key}" value)
        if(NOT "${key}=${value}" STREQUAL required)
            message(FATAL_ERROR "pinned probe metadata mismatch: ${key}")
        endif()
    endforeach()
    string(REPLACE "# protocol=luna.compiled-fragment-cost.pinned-thread.v1\n"
        "# protocol=luna.compiled-fragment-cost.v2\n" body "${normalized}")
    string(REPLACE "# affinity=${expected_affinity}\n"
        "# affinity=uncontrolled,power_policy=uncontrolled\n" body "${body}")
    luna_validate_compiled_probe("${body}" "${iterations}" "${profile}")
endfunction()
