# Export is an ABI promise, not merely a parser flag. Build a two-file package
# and inspect its emitted host IR: exported functions must be externally
# visible, while package-private declarations must keep internal linkage.

if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR OR NOT DEFINED LUNA_BINARY_DIR)
    message(FATAL_ERROR
        "LUNA_SOURCE_DIR and LUNA_BINARY_DIR are required")
endif()

set(work_dir "${LUNA_BINARY_DIR}/package-export-abi")
set(package_dir "${work_dir}/exported_package")
set(executable_path "${package_dir}/build/native/exported_package")
if(WIN32)
    set(executable_path "${executable_path}.exe")
endif()
set(ir_path "${executable_path}.ll")

function(cleanup_package_outputs)
    file(REMOVE_RECURSE "${work_dir}")
endfunction()

cleanup_package_outputs()
file(MAKE_DIRECTORY "${work_dir}")
file(COPY
    "${LUNA_SOURCE_DIR}/tests/fixtures/packages/exported_package"
    DESTINATION "${work_dir}")
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
