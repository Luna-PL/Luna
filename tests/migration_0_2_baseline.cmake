cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

set(LUNA_0_2_BASELINE_VERSION "0.2.1")
set(LUNA_0_2_BASELINE_COMMIT
    "a188d87a6f10d7fa67582389a0a0b915f3741401")

set(case_names)
set(case_kinds)
set(case_sources)
set(case_environments)
set(case_expectations)

macro(register_0_2_case name kind source environment expected)
    list(APPEND case_names "${name}")
    list(APPEND case_kinds "${kind}")
    list(APPEND case_sources "${source}")
    list(APPEND case_environments "${environment}")
    list(APPEND case_expectations "${expected}")
endmacro()

# This is the final representative 0.2 migration corpus, not the complete 0.2
# regression suite. Each case witnesses a surface that changes or disappears in
# 0.3. Keep assertions here and source programs in their original owner.
register_0_2_case(
    structural-record success
    tests/fixtures/structural_type_equivalence.luna -
    "Program exited with code: 41")
register_0_2_case(
    structural-enum success
    tests/fixtures/structural_enum_equivalence.luna -
    "Program exited with code: 42")
register_0_2_case(
    structural-generic-reuse success
    tests/fixtures/structural_generic_instance_reuse.luna -
    "Program exited with code: 1")
register_0_2_case(
    dynamic-symbol-selection success
    examples/dynamic_select.luna -
    "Program exited with code: 20")
register_0_2_case(
    dynamic-fragment-default success
    tests/fixtures/dynamic_fragments.luna -
    "41")
register_0_2_case(
    dynamic-fragment-environment-selection success
    tests/fixtures/dynamic_fragments.luna LUNA_FRAGMENT_PIPELINE=audit
    "43")
register_0_2_case(
    local-slot-and-statement-apply success
    examples/fragments.luna -
    "Program exited with code: 0")
register_0_2_case(
    explicit-fragment-contracts success
    tests/fixtures/fragment_contracts.luna -
    "41")
register_0_2_case(
    compiler-special-rc-arc success
    tests/fixtures/rc_arc.luna -
    "7\n8\n99\n99")
register_0_2_case(
    rc-implicit-copy-rejection error
    tests/fixtures/rc_implicit_copy_invalid.luna -
    "must be moved explicitly")

list(LENGTH case_names case_count)
foreach(list_name IN ITEMS
        case_kinds case_sources case_environments case_expectations)
    list(LENGTH ${list_name} value_count)
    if(NOT value_count EQUAL case_count)
        message(FATAL_ERROR "Malformed 0.2 migration corpus: ${list_name}")
    endif()
endforeach()

set(unique_case_names "${case_names}")
list(REMOVE_DUPLICATES unique_case_names)
list(LENGTH unique_case_names unique_case_count)
if(NOT unique_case_count EQUAL case_count)
    message(FATAL_ERROR "The 0.2 migration corpus contains duplicate case names")
endif()

math(EXPR last_case "${case_count} - 1")
foreach(index RANGE ${last_case})
    list(GET case_sources ${index} source)
    if(NOT EXISTS "${LUNA_SOURCE_DIR}/${source}")
        list(GET case_names ${index} name)
        message(FATAL_ERROR
            "0.2 migration case '${name}' lost its source: ${source}")
    endif()
endforeach()

if(NOT DEFINED LUNA_0_2_EXECUTABLE OR LUNA_0_2_EXECUTABLE STREQUAL "")
    message(STATUS
        "Validated ${case_count} 0.2 migration cases; execution is intentionally "
        "disabled until LUNA_0_2_EXECUTABLE is supplied")
    return()
endif()
if(NOT EXISTS "${LUNA_0_2_EXECUTABLE}")
    message(FATAL_ERROR
        "LUNA_0_2_EXECUTABLE does not exist: ${LUNA_0_2_EXECUTABLE}")
endif()

execute_process(
    COMMAND "${LUNA_0_2_EXECUTABLE}" --version
    RESULT_VARIABLE version_result
    OUTPUT_VARIABLE version_output
    ERROR_VARIABLE version_error
)
set(version_transcript "${version_output}\n${version_error}")
string(FIND "${version_transcript}"
    "Luna ${LUNA_0_2_BASELINE_VERSION}" version_at)
if(NOT version_result EQUAL 0 OR version_at EQUAL -1)
    message(FATAL_ERROR
        "The migration compiler is not Luna ${LUNA_0_2_BASELINE_VERSION}:\n"
        "${version_transcript}")
endif()

execute_process(
    COMMAND "${LUNA_0_2_EXECUTABLE}" analyze
            "${LUNA_SOURCE_DIR}/examples/minimal.luna"
            --message-format=json
    RESULT_VARIABLE identity_result
    OUTPUT_VARIABLE identity_output
    ERROR_VARIABLE identity_error
)
if(NOT identity_result EQUAL 0 OR NOT identity_error STREQUAL "")
    message(FATAL_ERROR
        "Cannot read the 0.2 migration compiler identity:\n${identity_error}")
endif()
string(REPLACE "\r\n" "\n" identity_output "${identity_output}")
string(REPLACE "\n" ";" identity_records "${identity_output}")
list(FILTER identity_records EXCLUDE REGEX "^$")
list(GET identity_records 0 identity_hello)
string(JSON actual_commit GET "${identity_hello}" compiler_commit)
if(NOT actual_commit STREQUAL LUNA_0_2_BASELINE_COMMIT)
    message(FATAL_ERROR
        "Wrong 0.2 migration compiler commit: expected "
        "${LUNA_0_2_BASELINE_COMMIT}, got ${actual_commit}")
endif()

foreach(index RANGE ${last_case})
    list(GET case_names ${index} name)
    list(GET case_kinds ${index} kind)
    list(GET case_sources ${index} source)
    list(GET case_environments ${index} environment)
    list(GET case_expectations ${index} expected)

    if(environment STREQUAL "-")
        execute_process(
            COMMAND "${LUNA_0_2_EXECUTABLE}" run
                    "${LUNA_SOURCE_DIR}/${source}"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE error
        )
    else()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env "${environment}"
                    "${LUNA_0_2_EXECUTABLE}" run
                    "${LUNA_SOURCE_DIR}/${source}"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE error
        )
    endif()
    set(transcript "${output}\n${error}")
    string(FIND "${transcript}" "${expected}" expected_at)
    string(FIND "${transcript}" "error[" compiler_error_at)

    if(kind STREQUAL "success")
        if(NOT compiler_error_at EQUAL -1 OR expected_at EQUAL -1)
            message(FATAL_ERROR
                "0.2 migration case '${name}' no longer succeeds as recorded.\n"
                "Expected: ${expected}\nTranscript:\n${transcript}")
        endif()
    elseif(kind STREQUAL "error")
        if(NOT result EQUAL 1 OR compiler_error_at EQUAL -1 OR
           expected_at EQUAL -1)
            message(FATAL_ERROR
                "0.2 migration case '${name}' lost its recorded diagnostic.\n"
                "Expected: ${expected}\nResult: ${result}\nTranscript:\n${transcript}")
        endif()
    else()
        message(FATAL_ERROR "Unknown migration case kind '${kind}' for '${name}'")
    endif()
endforeach()

message(STATUS
    "Reproduced ${case_count} Luna ${LUNA_0_2_BASELINE_VERSION} migration "
    "cases with compiler ${LUNA_0_2_BASELINE_COMMIT}")
