cmake_minimum_required(VERSION 3.20)
include("${CMAKE_CURRENT_LIST_DIR}/compiled_fragment_probe_protocol.cmake")

if(NOT DEFINED LUNA_COMPILED_PROBE_EXECUTABLE)
    message(FATAL_ERROR "LUNA_COMPILED_PROBE_EXECUTABLE is required")
endif()
if(NOT DEFINED LUNA_COMPILED_PROBE_ITERATIONS)
    set(LUNA_COMPILED_PROBE_ITERATIONS 3)
endif()
if(NOT LUNA_COMPILED_PROBE_ITERATIONS MATCHES "^[1-9][0-9]*$" OR
   LUNA_COMPILED_PROBE_ITERATIONS LESS 1 OR LUNA_COMPILED_PROBE_ITERATIONS GREATER 10000000)
    message(FATAL_ERROR "compiled probe iterations must be 1..10000000")
endif()
if(NOT DEFINED LUNA_COMPILED_PROBE_PROFILE)
    set(LUNA_COMPILED_PROBE_PROFILE O0)
endif()
if(NOT LUNA_COMPILED_PROBE_PROFILE MATCHES "^O[023]$")
    message(FATAL_ERROR "compiled probe profile must be O0, O2, or O3")
endif()
execute_process(COMMAND "${LUNA_COMPILED_PROBE_EXECUTABLE}" --compiled-fragment-cost
        "${LUNA_COMPILED_PROBE_ITERATIONS}" 9 "${LUNA_COMPILED_PROBE_PROFILE}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 180)
if(NOT status EQUAL 0 OR NOT errors STREQUAL "")
    message(FATAL_ERROR "compiled probe failed (${status}): ${errors}")
endif()
string(REPLACE "\r\n" "\n" output "${output}")
luna_validate_compiled_probe("${output}" "${LUNA_COMPILED_PROBE_ITERATIONS}" "${LUNA_COMPILED_PROBE_PROFILE}")
execute_process(COMMAND "${LUNA_COMPILED_PROBE_EXECUTABLE}" --compiled-fragment-cost 1 1 "${LUNA_COMPILED_PROBE_PROFILE}"
    RESULT_VARIABLE status OUTPUT_VARIABLE partial ERROR_VARIABLE errors TIMEOUT 30)
if(NOT status EQUAL 0 OR NOT errors STREQUAL "" OR
   NOT partial MATCHES "# verified_samples=9,position_balanced=no")
    message(FATAL_ERROR "partial compiled schedule incorrectly reported balance")
endif()
execute_process(COMMAND "${LUNA_COMPILED_PROBE_EXECUTABLE}" --compiled-fragment-cost 1 1
    RESULT_VARIABLE status OUTPUT_VARIABLE default_output ERROR_VARIABLE errors TIMEOUT 30)
if(NOT status EQUAL 0 OR NOT errors STREQUAL "" OR
   NOT default_output MATCHES "# moonir_optimization=O2,llvm_optimization=O0")
    message(FATAL_ERROR "compiled probe default profile changed")
endif()
foreach(arguments IN ITEMS "--unknown" "--compiled-fragment-cost|-1"
        "--compiled-fragment-cost|3junk" "--compiled-fragment-cost|18446744073709551616"
        "--compiled-fragment-cost|0" "--compiled-fragment-cost|10000001"
        "--compiled-fragment-cost|3|0" "--compiled-fragment-cost|3|-1"
        "--compiled-fragment-cost|3|91" "--compiled-fragment-cost|3|9|extra"
        "--compiled-fragment-cost|3|9|O1" "--compiled-fragment-cost|3|9|o2"
        "--compiled-fragment-cost|3|9|O2|extra")
    string(REPLACE "|" ";" arguments "${arguments}")
    execute_process(COMMAND "${LUNA_COMPILED_PROBE_EXECUTABLE}" ${arguments}
        RESULT_VARIABLE status OUTPUT_VARIABLE invalid_output ERROR_VARIABLE errors TIMEOUT 10)
    if(NOT status EQUAL 1 OR NOT invalid_output STREQUAL "" OR errors STREQUAL "")
        message(FATAL_ERROR "invalid compiled CLI accepted: ${arguments}")
    endif()
endforeach()
if(DEFINED LUNA_COMPILED_PROBE_RECORD)
    file(WRITE "${LUNA_COMPILED_PROBE_RECORD}" "${output}")
endif()
message(STATUS "Compiled Fragment ${LUNA_COMPILED_PROBE_PROFILE} probe: 81 samples, exact calls/checksums, balanced positions; no timing threshold")
