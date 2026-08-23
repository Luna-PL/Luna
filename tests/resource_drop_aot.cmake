if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()
if(NOT DEFINED LUNA_BINARY_DIR)
    message(FATAL_ERROR "LUNA_BINARY_DIR must point at the build tree")
endif()

set(work_dir "${LUNA_BINARY_DIR}/resource_drop_aot")
set(source "${work_dir}/resource_generic_drop.luna")

file(MAKE_DIRECTORY "${work_dir}")
file(COPY_FILE
    "${LUNA_SOURCE_DIR}/tests/fixtures/resource_generic_drop.luna"
    "${source}" ONLY_IF_DIFFERENT)
include("${LUNA_SOURCE_DIR}/tests/aot_package_fixture.cmake")
luna_stage_aot_application("${source}" "resource_drop_aot")
set(ir "${LUNA_AOT_IR_PATH}")
set(executable "${LUNA_AOT_EXECUTABLE_PATH}")
file(REMOVE "${ir}" "${executable}")

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${LUNA_AOT_PACKAGE_DIR}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0 OR NOT EXISTS "${executable}")
    message(FATAL_ERROR
        "generic recursive Drop AOT build failed.\n"
        "Output:\n${build_output}\n${build_error}")
endif()

execute_process(
    COMMAND "${executable}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error)
file(REMOVE "${ir}" "${executable}" "${source}")

if(NOT run_result EQUAL 0 OR
   NOT run_output MATCHES "41[\r\n]+42[\r\n]*$")
    message(FATAL_ERROR
        "generic recursive Drop AOT behavior diverged.\n"
        "Result: ${run_result}\n"
        "Output:\n${run_output}\n${run_error}")
endif()
