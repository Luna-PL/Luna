if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR OR NOT DEFINED LUNA_BINARY_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR and LUNA_BINARY_DIR are required")
endif()

set(workspace "${LUNA_BINARY_DIR}/core-option-workspace")
file(REMOVE_RECURSE "${workspace}")
file(COPY "${LUNA_SOURCE_DIR}/stdlib/" DESTINATION "${workspace}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/core_option_app/"
     DESTINATION "${workspace}/app")
file(WRITE "${workspace}/luna.workspace"
"[workspace]\nmembers = [\"core\", \"sys\", \"alloc\", \"std\", \"app\"]\n")
file(APPEND "${workspace}/luna.lock"
"\n[[package]]\nid = \"org.luna.fixture.core_option\"\nversion = \"1.0.0\"\nsource = \"workspace:app\"\nhash = \"fixture-core-option-v1\"\n")

function(verify_result label result output error)
    if(NOT "${result}" EQUAL 42 OR NOT "${error}" STREQUAL "")
        file(REMOVE_RECURSE "${workspace}")
        message(FATAL_ERROR
            "Core Option ${label} failed.\nResult: ${result}\n"
            "stdout:\n${output}\nstderr:\n${error}")
    endif()
    foreach(marker IN ITEMS 111 112 113 114 115)
        string(REGEX MATCHALL "(^|\n)${marker}(\n|$)" matches "${output}")
        list(LENGTH matches match_count)
        if(NOT match_count EQUAL 1)
            file(REMOVE_RECURSE "${workspace}")
            message(FATAL_ERROR
                "Core Option ${label} did not clean marker ${marker} exactly once.\n"
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
set(aot_executable "${workspace}/app/build/native/core_option")
if(WIN32)
    string(APPEND aot_executable ".exe")
endif()
if(NOT build_result EQUAL 0 OR NOT EXISTS "${aot_executable}")
    file(REMOVE_RECURSE "${workspace}")
    message(FATAL_ERROR
        "Core Option AOT build failed.\n${build_output}\n${build_error}")
endif()
execute_process(
    COMMAND "${aot_executable}"
    RESULT_VARIABLE aot_result
    OUTPUT_VARIABLE aot_output
    ERROR_VARIABLE aot_error
)
verify_result("AOT run" "${aot_result}" "${aot_output}" "${aot_error}")

function(verify_panic label expected)
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
            "Core Option ${label} did not reach the expected panic boundary.\n"
            "Result: ${panic_result}\nExpected: ${expected}\n"
            "Transcript:\n${transcript}")
    endif()
endfunction()

file(WRITE "${workspace}/app/src/main.luna"
"package org.luna.fixture.core_option;\n"
"module application;\n"
"using org.luna.core as core;\n\n"
"fn main() -> i32 {\n"
"    let value = core::option::Option::<i32>::None();\n"
"    return core::option::unwrap(value) + 0;\n"
"}\n")
verify_panic(
    "unwrap(None)"
    "Luna panic: called core::option::unwrap on None")

file(WRITE "${workspace}/app/src/main.luna"
"package org.luna.fixture.core_option;\n"
"module application;\n"
"using org.luna.core as core;\n\n"
"fn main() -> i32 {\n"
"    let value = core::option::Option::<i32>::None();\n"
"    return core::option::expect(value, \"missing option payload\") + 0;\n"
"}\n")
verify_panic(
    "expect(None)"
    "Luna panic: missing option payload")

file(REMOVE_RECURSE "${workspace}")
