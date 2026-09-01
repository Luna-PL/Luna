if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR OR NOT DEFINED LUNA_BINARY_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR and LUNA_BINARY_DIR are required")
endif()

set(workspace "${LUNA_BINARY_DIR}/core-result-workspace")
file(REMOVE_RECURSE "${workspace}")
file(COPY "${LUNA_SOURCE_DIR}/stdlib/" DESTINATION "${workspace}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/core_result_app/"
     DESTINATION "${workspace}/app")
file(WRITE "${workspace}/luna.workspace"
"[workspace]\nmembers = [\"core\", \"sys\", \"alloc\", \"std\", \"app\"]\n")
file(APPEND "${workspace}/luna.lock"
"\n[[package]]\nid = \"org.luna.fixture.core_result\"\nversion = \"1.0.0\"\nsource = \"workspace:app\"\nhash = \"fixture-core-result-v1\"\n")

function(verify_result label result output error)
    if(NOT "${result}" EQUAL 42 OR NOT "${error}" STREQUAL "")
        file(REMOVE_RECURSE "${workspace}")
        message(FATAL_ERROR
            "Core Result ${label} failed.\nResult: ${result}\n"
            "stdout:\n${output}\nstderr:\n${error}")
    endif()
    foreach(marker IN ITEMS 121 122 123 124 125 126)
        string(REGEX MATCHALL "(^|\n)${marker}(\n|$)" matches "${output}")
        list(LENGTH matches match_count)
        if(NOT match_count EQUAL 1)
            file(REMOVE_RECURSE "${workspace}")
            message(FATAL_ERROR
                "Core Result ${label} did not clean marker ${marker} exactly once.\n"
                "stdout:\n${output}")
        endif()
    endforeach()
endfunction()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" run "${workspace}/app"
    RESULT_VARIABLE jit_result
    OUTPUT_VARIABLE jit_output
    ERROR_VARIABLE jit_error
)
verify_result("JIT run" "${jit_result}" "${jit_output}" "${jit_error}")

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${workspace}/app" -O0
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
set(aot_executable "${workspace}/app/build/native/core_result")
if(WIN32)
    string(APPEND aot_executable ".exe")
endif()
if(NOT build_result EQUAL 0 OR NOT EXISTS "${aot_executable}")
    file(REMOVE_RECURSE "${workspace}")
    message(FATAL_ERROR
        "Core Result AOT build failed.\n${build_output}\n${build_error}")
endif()
execute_process(
    COMMAND "${aot_executable}"
    RESULT_VARIABLE aot_result
    OUTPUT_VARIABLE aot_output
    ERROR_VARIABLE aot_error
)
verify_result("AOT run" "${aot_result}" "${aot_output}" "${aot_error}")

function(verify_panic label source expected)
    file(WRITE "${workspace}/app/src/main.luna" "${source}")
    execute_process(
        COMMAND "${LUNA_EXECUTABLE}" run "${workspace}/app"
        RESULT_VARIABLE panic_result
        OUTPUT_VARIABLE panic_output
        ERROR_VARIABLE panic_error
    )
    set(transcript "${panic_output}\n${panic_error}")
    string(FIND "${transcript}" "error[" compiler_error)
    string(FIND "${transcript}" "${expected}" expected_at)
    if("${panic_result}" STREQUAL "0" OR
       NOT compiler_error EQUAL -1 OR expected_at EQUAL -1)
        file(REMOVE_RECURSE "${workspace}")
        message(FATAL_ERROR
            "Core Result ${label} did not reach the expected panic boundary.\n"
            "Result: ${panic_result}\nExpected: ${expected}\n"
            "Transcript:\n${transcript}")
    endif()
endfunction()

string(CONCAT source_prefix
"package org.luna.fixture.core_result;\n"
"module application;\n"
"using org.luna.core as core;\n\n")

verify_panic(
    "unwrap(Err)"
    "${source_prefix}fn main() -> i32 {\n    let value = Err::<i32, i32>(1);\n    return core::result::unwrap(value) + 0;\n}\n"
    "Luna panic: called core::result::unwrap on Err")
verify_panic(
    "expect(Err)"
    "${source_prefix}fn main() -> i32 {\n    let value = Err::<i32, i32>(1);\n    return core::result::expect(value, \"missing result payload\") + 0;\n}\n"
    "Luna panic: missing result payload")
verify_panic(
    "unwrap_err(Ok)"
    "${source_prefix}fn main() -> i32 {\n    let value = Ok::<i32, i32>(1);\n    return core::result::unwrap_err(value) + 0;\n}\n"
    "Luna panic: called core::result::unwrap_err on Ok")
verify_panic(
    "expect_err(Ok)"
    "${source_prefix}fn main() -> i32 {\n    let value = Ok::<i32, i32>(1);\n    return core::result::expect_err(value, \"missing error payload\") + 0;\n}\n"
    "Luna panic: missing error payload")

file(REMOVE_RECURSE "${workspace}")
