# The AOT driver must accept an explicit runtime archive, diagnose a missing
# archive predictably, and only create a runtime GPU initialization boundary
# when kernel support is used or explicitly reserved.

if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()
if(NOT DEFINED LUNA_RUNTIME_LIBRARY OR NOT EXISTS "${LUNA_RUNTIME_LIBRARY}")
    message(FATAL_ERROR "LUNA_RUNTIME_LIBRARY must point at the built Luna runtime archive")
endif()

set(source_path "${LUNA_SOURCE_DIR}/tests/fixtures/aot_runtime_boundary.luna")
set(ir_path "${source_path}.ll")
set(executable_path "${LUNA_SOURCE_DIR}/tests/fixtures/aot_runtime_boundary")
if(WIN32)
    set(executable_path "${executable_path}.exe")
endif()
set(missing_runtime "${LUNA_SOURCE_DIR}/tests/fixtures/not-a-runtime.a")

function(cleanup_outputs)
    file(REMOVE "${ir_path}" "${executable_path}")
endfunction()

cleanup_outputs()
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${source_path}"
            --runtime-lib "${LUNA_RUNTIME_LIBRARY}" --cc clang++
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0 OR NOT EXISTS "${ir_path}" OR NOT EXISTS "${executable_path}")
    cleanup_outputs()
    message(FATAL_ERROR
        "AOT build with explicit runtime settings failed.\nResult: ${build_result}\n"
        "Output:\n${build_output}\n${build_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env LUNA_GPU_BACKEND=invalid-backend "${executable_path}"
    RESULT_VARIABLE runtime_result
    OUTPUT_VARIABLE runtime_output
    ERROR_VARIABLE runtime_error
)
if(NOT runtime_result EQUAL 0)
    cleanup_outputs()
    message(FATAL_ERROR
        "An unused kernel unexpectedly created a GPU runtime dependency.\n"
        "Result: ${runtime_result}\n"
        "Output:\n${runtime_output}\n${runtime_error}")
endif()

cleanup_outputs()
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${source_path}"
            --runtime-lib "${LUNA_RUNTIME_LIBRARY}" --cc clang++
            --reserve-kernel-runtime
    RESULT_VARIABLE reserved_build_result
    OUTPUT_VARIABLE reserved_build_output
    ERROR_VARIABLE reserved_build_error
)
if(NOT reserved_build_result EQUAL 0 OR NOT EXISTS "${ir_path}" OR NOT EXISTS "${executable_path}")
    cleanup_outputs()
    message(FATAL_ERROR
        "AOT build with reserved kernel support failed.\nResult: ${reserved_build_result}\n"
        "Output:\n${reserved_build_output}\n${reserved_build_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env LUNA_GPU_BACKEND=invalid-backend "${executable_path}"
    RESULT_VARIABLE reserved_runtime_result
    OUTPUT_VARIABLE reserved_runtime_output
    ERROR_VARIABLE reserved_runtime_error
)
string(FIND "${reserved_runtime_output}\n${reserved_runtime_error}"
    "GPU backend initialization failed for 'invalid-backend': unknown GPU backend 'invalid-backend'"
    initialization_error_at)
if(NOT reserved_runtime_result EQUAL 1 OR initialization_error_at EQUAL -1)
    cleanup_outputs()
    message(FATAL_ERROR
        "Reserved kernel support did not report an invalid GPU backend clearly.\n"
        "Result: ${reserved_runtime_result}\n"
        "Output:\n${reserved_runtime_output}\n${reserved_runtime_error}")
endif()

cleanup_outputs()
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${source_path}" --runtime-lib "${missing_runtime}"
    RESULT_VARIABLE missing_result
    OUTPUT_VARIABLE missing_output
    ERROR_VARIABLE missing_error
)
cleanup_outputs()
string(FIND "${missing_output}\n${missing_error}"
    "error[driver/DRV0001]: runtime library does not exist" missing_diagnostic_at)
if(NOT missing_result EQUAL 1 OR missing_diagnostic_at EQUAL -1)
    message(FATAL_ERROR
        "missing runtime library did not produce the expected driver diagnostic.\n"
        "Result: ${missing_result}\nOutput:\n${missing_output}\n${missing_error}")
endif()
