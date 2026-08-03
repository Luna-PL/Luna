cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

find_package(Git QUIET)
set(expected_commit "unknown")
if(Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --verify HEAD
        WORKING_DIRECTORY "${LUNA_SOURCE_DIR}"
        RESULT_VARIABLE git_result
        OUTPUT_VARIABLE git_commit
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(git_result EQUAL 0 AND NOT git_commit STREQUAL "")
        set(expected_commit "${git_commit}")
    endif()
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" analyze
            "${LUNA_SOURCE_DIR}/examples/minimal.luna"
            --message-format=json
    RESULT_VARIABLE analyze_result
    OUTPUT_VARIABLE analyze_output
    ERROR_VARIABLE analyze_error
)
if(NOT analyze_result EQUAL 0 OR NOT analyze_error STREQUAL "")
    message(FATAL_ERROR
        "cannot read compiler identity\n"
        "result: ${analyze_result}\nstderr: ${analyze_error}\nstdout: ${analyze_output}")
endif()

string(REPLACE "\r\n" "\n" analyze_output "${analyze_output}")
string(REPLACE "\n" ";" records "${analyze_output}")
list(FILTER records EXCLUDE REGEX "^$")
list(GET records 0 hello)
string(JSON protocol GET "${hello}" protocol)
string(JSON kind GET "${hello}" kind)
string(JSON actual_commit GET "${hello}" compiler_commit)
if(NOT protocol STREQUAL "luna.analysis" OR NOT kind STREQUAL "hello")
    message(FATAL_ERROR "compiler identity did not start with analysis hello: ${hello}")
endif()
if(NOT actual_commit STREQUAL expected_commit)
    message(FATAL_ERROR
        "compiler commit identity is stale: binary=${actual_commit}, source=${expected_commit}")
endif()
