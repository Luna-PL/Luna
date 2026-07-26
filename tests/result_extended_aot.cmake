# Generic Result payloads, structured matching and static From conversions
# must use the same representation and dispatch in JIT and AOT builds.

if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

set(LUNA_EXE_SUFFIX "")
if(WIN32)
    set(LUNA_EXE_SUFFIX ".exe")
endif()

function(check_result_fixture stem expected_output expected_ir)
    set(source_path "${LUNA_SOURCE_DIR}/tests/fixtures/${stem}.luna")
    set(ir_path "${source_path}.ll")
    set(executable_path
        "${LUNA_SOURCE_DIR}/tests/fixtures/${stem}${LUNA_EXE_SUFFIX}")
    file(REMOVE "${ir_path}" "${executable_path}")

    execute_process(
        COMMAND "${LUNA_EXECUTABLE}" run "${source_path}"
        RESULT_VARIABLE jit_result
        OUTPUT_VARIABLE jit_output
        ERROR_VARIABLE jit_error
    )
    set(expected_jit_output
        "${expected_output}Program exited with code: 0\n")
    if(NOT jit_result EQUAL 0 OR
       NOT "${jit_output}" STREQUAL "${expected_jit_output}" OR
       NOT "${jit_error}" STREQUAL "")
        message(FATAL_ERROR
            "${stem} JIT failed.\nResult: ${jit_result}\n"
            "stdout:\n${jit_output}\nstderr:\n${jit_error}")
    endif()

    execute_process(
        COMMAND "${LUNA_EXECUTABLE}" build "${source_path}"
        RESULT_VARIABLE build_result
        OUTPUT_VARIABLE build_output
        ERROR_VARIABLE build_error
    )
    if(NOT build_result EQUAL 0 OR
       NOT EXISTS "${ir_path}" OR NOT EXISTS "${executable_path}")
        file(REMOVE "${ir_path}" "${executable_path}")
        message(FATAL_ERROR
            "${stem} AOT build failed.\n${build_output}\n${build_error}")
    endif()
    file(READ "${ir_path}" generated_ir)
    string(FIND "${generated_ir}" "${expected_ir}" expected_ir_at)
    if(expected_ir_at EQUAL -1)
        file(REMOVE "${ir_path}" "${executable_path}")
        message(FATAL_ERROR
            "${stem} AOT IR does not contain '${expected_ir}'")
    endif()
    if(ARGC GREATER 3)
        string(FIND "${generated_ir}" "${ARGV3}" forbidden_ir_at)
        if(NOT forbidden_ir_at EQUAL -1)
            file(REMOVE "${ir_path}" "${executable_path}")
            message(FATAL_ERROR
                "${stem} AOT IR unexpectedly contains '${ARGV3}'")
        endif()
    endif()

    execute_process(
        COMMAND "${executable_path}"
        RESULT_VARIABLE aot_result
        OUTPUT_VARIABLE aot_output
        ERROR_VARIABLE aot_error
    )
    file(REMOVE "${ir_path}" "${executable_path}")
    if(NOT aot_result EQUAL 0 OR
       NOT "${aot_output}" STREQUAL "${expected_output}" OR
       NOT "${aot_error}" STREQUAL "")
        message(FATAL_ERROR
            "${stem} JIT/AOT behavior diverged.\n"
            "Result: ${aot_result}\nstdout:\n${aot_output}\n"
            "stderr:\n${aot_error}")
    endif()
endfunction()

check_result_fixture(
    "result_from_conversion"
    "42\n7\nconverted\n"
    "try.converted_error")
check_result_fixture(
    "result_from_resource"
    "71\n71\n"
    "try.converted_error")
check_result_fixture(
    "result_match"
    "40\n41\n50\n52\n"
    "match.tag")
check_result_fixture(
    "enum_match"
    "16\n1\n42\n42\n"
    "match.tag"
    "call ptr @rt_alloc")

# This fixture intentionally returns 30. Validate its ABI and behavior
# separately because a nonzero source-language main result is expected.
set(generic_source
    "${LUNA_SOURCE_DIR}/tests/fixtures/result_payload_abi_invalid.luna")
set(generic_ir "${generic_source}.ll")
set(generic_executable
    "${LUNA_SOURCE_DIR}/tests/fixtures/result_payload_abi_invalid${LUNA_EXE_SUFFIX}")
file(REMOVE "${generic_ir}" "${generic_executable}")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${generic_source}"
    RESULT_VARIABLE generic_build_result
    OUTPUT_VARIABLE generic_build_output
    ERROR_VARIABLE generic_build_error
)
if(NOT generic_build_result EQUAL 0 OR NOT EXISTS "${generic_ir}" OR
   NOT EXISTS "${generic_executable}")
    file(REMOVE "${generic_ir}" "${generic_executable}")
    message(FATAL_ERROR
        "generic Result AOT build failed.\n"
        "${generic_build_output}\n${generic_build_error}")
endif()
file(READ "${generic_ir}" generic_generated_ir)
string(FIND "${generic_generated_ir}" "[2 x i64]" generic_payload_at)
execute_process(
    COMMAND "${generic_executable}"
    RESULT_VARIABLE generic_result
    OUTPUT_VARIABLE generic_output
    ERROR_VARIABLE generic_error
)
file(REMOVE "${generic_ir}" "${generic_executable}")
if(generic_payload_at EQUAL -1 OR NOT generic_result EQUAL 30 OR
   NOT "${generic_output}" STREQUAL "41\n24\n" OR
   NOT "${generic_error}" STREQUAL "")
    message(FATAL_ERROR
        "generic Result AOT ABI/behavior diverged.\n"
        "Result: ${generic_result}\nstdout:\n${generic_output}\n"
        "stderr:\n${generic_error}")
endif()
