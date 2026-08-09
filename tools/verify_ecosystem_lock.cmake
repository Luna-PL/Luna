cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the Luna source tree")
endif()
set(lock_path "${LUNA_SOURCE_DIR}/ecosystem.lock.json")
if(NOT EXISTS "${lock_path}")
    message(FATAL_ERROR "ecosystem lock is missing: ${lock_path}")
endif()

find_package(Git REQUIRED)
file(READ "${lock_path}" lock_json)
string(JSON schema_version GET "${lock_json}" schema_version)
if(NOT schema_version EQUAL 1)
    message(FATAL_ERROR "unsupported ecosystem lock schema: ${schema_version}")
endif()

function(verify_repository component directory)
    if(NOT EXISTS "${directory}/.git")
        message(FATAL_ERROR "${component} repository is missing: ${directory}")
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${directory}"
        RESULT_VARIABLE head_result
        OUTPUT_VARIABLE actual_commit
        ERROR_VARIABLE head_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT head_result EQUAL 0)
        message(FATAL_ERROR "cannot read ${component} HEAD: ${head_error}")
    endif()
    string(JSON expected_commit GET "${lock_json}" components ${component} commit)
    if(NOT actual_commit STREQUAL expected_commit)
        message(FATAL_ERROR
            "${component} commit mismatch: actual=${actual_commit}, expected=${expected_commit}")
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain
        WORKING_DIRECTORY "${directory}"
        RESULT_VARIABLE status_result
        OUTPUT_VARIABLE status_output
        ERROR_VARIABLE status_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT status_result EQUAL 0)
        message(FATAL_ERROR "cannot inspect ${component} worktree: ${status_error}")
    endif()
    if(NOT status_output STREQUAL "")
        message(FATAL_ERROR "${component} worktree is not clean:\n${status_output}")
    endif()
endfunction()

verify_repository(toolchain "${LUNA_SOURCE_DIR}/toolchain")
verify_repository(lunax "${LUNA_SOURCE_DIR}/Lunax")

file(READ "${LUNA_SOURCE_DIR}/toolchain/compatibility/luna.json"
     compatibility_json)
string(JSON locked_toolchain_version GET "${lock_json}" components toolchain version)
string(JSON actual_toolchain_version GET "${compatibility_json}" toolchain_version)
string(JSON locked_language_version GET "${lock_json}" components luna language_version)
string(JSON compatible_language_version GET "${compatibility_json}" luna language_version)
if(NOT actual_toolchain_version STREQUAL locked_toolchain_version OR
   NOT compatible_language_version STREQUAL locked_language_version)
    message(FATAL_ERROR
        "toolchain compatibility manifest disagrees with ecosystem lock")
endif()

file(READ "${LUNA_SOURCE_DIR}/Lunax/VERSION" actual_lunax_version)
string(STRIP "${actual_lunax_version}" actual_lunax_version)
string(JSON locked_lunax_version GET "${lock_json}" components lunax version)
if(NOT actual_lunax_version STREQUAL locked_lunax_version)
    message(FATAL_ERROR "Lunax VERSION disagrees with ecosystem lock")
endif()

if(DEFINED LUNA_EXECUTABLE AND EXISTS "${LUNA_EXECUTABLE}")
    execute_process(
        COMMAND "${LUNA_EXECUTABLE}" analyze
                "${LUNA_SOURCE_DIR}/examples/minimal.luna"
                --message-format=json
        RESULT_VARIABLE analyze_result
        OUTPUT_VARIABLE analyze_output
        ERROR_VARIABLE analyze_error)
    if(NOT analyze_result EQUAL 0 OR NOT analyze_error STREQUAL "")
        message(FATAL_ERROR "cannot probe locked Luna compiler: ${analyze_error}")
    endif()
    string(REPLACE "\r\n" "\n" analyze_output "${analyze_output}")
    string(REPLACE "\n" ";" analysis_records "${analyze_output}")
    list(FILTER analysis_records EXCLUDE REGEX "^$")
    list(GET analysis_records 0 analysis_hello)
    string(JSON actual_language_version GET "${analysis_hello}" language_version)
    string(JSON actual_compiler_commit GET "${analysis_hello}" compiler_commit)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${LUNA_SOURCE_DIR}"
        OUTPUT_VARIABLE luna_head
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT actual_language_version STREQUAL locked_language_version OR
       NOT actual_compiler_commit STREQUAL luna_head)
        message(FATAL_ERROR
            "Luna binary is incompatible with this ecosystem lock: "
            "language binary=${actual_language_version}, lock=${locked_language_version}; "
            "commit binary=${actual_compiler_commit}, source=${luna_head}")
    endif()
endif()

message(STATUS "ecosystem lock verified")
