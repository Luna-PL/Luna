# Return cleanup is control-flow sensitive.  Build a fixture with one nested
# and one fall-through return, then make sure the generated IR contains one
# rt_dealloc at each return site rather than a single unreachable cleanup.

if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

set(source_path "${LUNA_SOURCE_DIR}/tests/fixtures/ownership_return_cleanup.luna")
include("${LUNA_SOURCE_DIR}/tests/aot_package_fixture.cmake")
luna_stage_aot_application("${source_path}" "return_cleanup_abi")
set(ir_path "${LUNA_AOT_IR_PATH}")
set(executable_path "${LUNA_AOT_EXECUTABLE_PATH}")

function(cleanup_outputs)
    file(REMOVE "${ir_path}" "${executable_path}")
endfunction()

cleanup_outputs()
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${LUNA_AOT_PACKAGE_DIR}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0 OR NOT EXISTS "${ir_path}")
    cleanup_outputs()
    message(FATAL_ERROR
        "return-cleanup AOT build failed.\n"
        "Result: ${build_result}\n"
        "Output:\n${build_output}\n${build_error}")
endif()

file(READ "${ir_path}" ir)
string(REGEX MATCHALL "call void @rt_dealloc" cleanup_calls "${ir}")
list(LENGTH cleanup_calls cleanup_count)
cleanup_outputs()

if(NOT cleanup_count EQUAL 2)
    message(FATAL_ERROR
        "return cleanup did not remain path-sensitive. Expected two rt_dealloc calls, "
        "found ${cleanup_count}.\nIR:\n${ir}")
endif()

string(REGEX MATCHALL "call void @rt_dealloc\\(ptr [^,]+, i64 4, i64 4\\)" exact_layout_calls "${ir}")
list(LENGTH exact_layout_calls exact_layout_count)
if(NOT exact_layout_count EQUAL 2)
    message(FATAL_ERROR
        "return cleanup did not preserve the exact i32 allocation layout. "
        "Expected two size=4/alignment=4 calls, found ${exact_layout_count}.\nIR:\n${ir}")
endif()
