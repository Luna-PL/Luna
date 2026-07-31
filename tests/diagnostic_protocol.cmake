cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

function(json_records transcript output)
    string(REPLACE "\r\n" "\n" normalized "${transcript}")
    string(REPLACE "\n" ";" records "${normalized}")
    list(FILTER records EXCLUDE REGEX "^$")
    foreach(record IN LISTS records)
        string(JSON protocol GET "${record}" protocol)
        string(JSON version GET "${record}" version)
        if(NOT protocol STREQUAL "luna.diagnostic" OR NOT version EQUAL 1)
            message(FATAL_ERROR "unexpected protocol identity: ${record}")
        endif()
    endforeach()
    set(${output} "${records}" PARENT_SCOPE)
endfunction()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" check
            "${LUNA_SOURCE_DIR}/tests/fixtures/parse_missing_binding_name.luna"
            --message-format=json
    RESULT_VARIABLE invalid_result
    OUTPUT_VARIABLE invalid_output
    ERROR_VARIABLE invalid_error
)
if(NOT invalid_result EQUAL 1 OR NOT invalid_error STREQUAL "")
    message(FATAL_ERROR
        "invalid input did not use the JSONL diagnostic stream exclusively\n"
        "result: ${invalid_result}\nstderr: ${invalid_error}\nstdout: ${invalid_output}")
endif()
json_records("${invalid_output}" invalid_records)
list(LENGTH invalid_records invalid_count)
if(NOT invalid_count EQUAL 3)
    message(FATAL_ERROR "expected hello, diagnostic, summary; got: ${invalid_output}")
endif()
list(GET invalid_records 0 hello)
list(GET invalid_records 1 diagnostic)
list(GET invalid_records 2 summary)
string(JSON hello_kind GET "${hello}" kind)
string(JSON hello_language GET "${hello}" language_version)
string(JSON diagnostic_kind GET "${diagnostic}" kind)
string(JSON diagnostic_code GET "${diagnostic}" code)
string(JSON diagnostic_byte GET "${diagnostic}" primary start byte)
string(JSON summary_kind GET "${summary}" kind)
string(JSON summary_success GET "${summary}" success)
if(NOT hello_kind STREQUAL "hello" OR
   NOT hello_language STREQUAL "0.2.0-alpha" OR
   NOT diagnostic_kind STREQUAL "diagnostic" OR
   NOT diagnostic_code STREQUAL "PAR0001" OR
   diagnostic_byte LESS 0 OR
   NOT summary_kind STREQUAL "summary" OR
   summary_success)
    message(FATAL_ERROR "invalid protocol record sequence: ${invalid_output}")
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" check
            "${LUNA_SOURCE_DIR}/examples/minimal.luna"
            --message-format=json
    RESULT_VARIABLE valid_result
    OUTPUT_VARIABLE valid_output
    ERROR_VARIABLE valid_error
)
if(NOT valid_result EQUAL 0 OR NOT valid_error STREQUAL "")
    message(FATAL_ERROR
        "valid input did not complete through JSONL\n"
        "result: ${valid_result}\nstderr: ${valid_error}\nstdout: ${valid_output}")
endif()
json_records("${valid_output}" valid_records)
list(LENGTH valid_records valid_count)
list(GET valid_records 0 valid_hello)
list(GET valid_records 1 valid_summary)
string(JSON valid_hello_kind GET "${valid_hello}" kind)
string(JSON valid_summary_kind GET "${valid_summary}" kind)
string(JSON valid_success GET "${valid_summary}" success)
if(NOT valid_count EQUAL 2 OR
   NOT valid_hello_kind STREQUAL "hello" OR
   NOT valid_summary_kind STREQUAL "summary" OR
   NOT valid_success)
    message(FATAL_ERROR "valid protocol sequence is malformed: ${valid_output}")
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" run
            "${LUNA_SOURCE_DIR}/examples/minimal.luna"
            --message-format=json
    RESULT_VARIABLE misuse_result
    OUTPUT_VARIABLE misuse_output
    ERROR_VARIABLE misuse_error
)
if(NOT misuse_result EQUAL 2 OR NOT misuse_error STREQUAL "")
    message(FATAL_ERROR "protocol misuse must be JSONL with exit status 2")
endif()
json_records("${misuse_output}" misuse_records)
list(LENGTH misuse_records misuse_count)
if(NOT misuse_count EQUAL 3)
    message(FATAL_ERROR "protocol misuse sequence is malformed: ${misuse_output}")
endif()
