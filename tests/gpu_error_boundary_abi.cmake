# The host lowering must turn a failed event wait into an observable program
# failure rather than continuing with resources whose launch did not succeed.

if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

set(source "${LUNA_SOURCE_DIR}/examples/heterogeneous.luna")
set(ir "${source}.ll")
set(executable "${LUNA_SOURCE_DIR}/examples/heterogeneous")
file(REMOVE "${ir}" "${executable}")

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${source}" -O2
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0 OR NOT EXISTS "${ir}")
    file(REMOVE "${ir}" "${executable}")
    message(FATAL_ERROR
        "could not emit heterogeneous AOT IR for error-boundary validation.\n"
        "Result: ${build_result}\n${build_output}\n${build_error}")
endif()

file(READ "${ir}" generated_ir)
file(REMOVE "${ir}" "${executable}")
foreach(required
        "call i32 @rt_gpu_await_event"
        "call void @rt_gpu_report_operation_error_and_abort()"
        "gpu.operation.failed"
        "unreachable")
    string(FIND "${generated_ir}" "${required}" required_at)
    if(required_at EQUAL -1)
        message(FATAL_ERROR
            "heterogeneous await lowering is missing '${required}'; failed GPU work could continue silently")
    endif()
endforeach()
