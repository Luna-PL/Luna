# Export is an ABI promise, not merely a parser flag. Build a two-file package
# and inspect its emitted host IR: exported functions must be externally
# visible, while package-private declarations must keep internal linkage.

if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

set(package_dir "${LUNA_SOURCE_DIR}/tests/fixtures/packages/exported_package")
set(ir_path "${package_dir}/exported_package.ll")
set(executable_path "${package_dir}/exported_package")
if(WIN32)
    set(executable_path "${executable_path}.exe")
endif()

function(cleanup_package_outputs)
    file(REMOVE "${ir_path}" "${executable_path}")
endfunction()

cleanup_package_outputs()
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${package_dir}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0 OR NOT EXISTS "${ir_path}")
    cleanup_package_outputs()
    message(FATAL_ERROR
        "package AOT build failed.\n"
        "Result: ${build_result}\n"
        "Output:\n${build_output}\n${build_error}")
endif()

file(READ "${ir_path}" ir)
string(FIND "${ir}" "define i32 @add(" exported_add)
string(FIND "${ir}" "define internal i32 @private_offset(" private_offset)
cleanup_package_outputs()

if(exported_add EQUAL -1 OR private_offset EQUAL -1)
    message(FATAL_ERROR
        "package export linkage did not match the public ABI contract.\n"
        "Expected external @add and internal @private_offset.\n"
        "IR:\n${ir}")
endif()
