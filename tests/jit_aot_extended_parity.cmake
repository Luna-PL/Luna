# Extend the basic parity fixture to the two boundaries most likely to diverge
# after lowering: a multi-file package and optimized host-side GPU simulation.

if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR OR NOT DEFINED LUNA_BINARY_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR and LUNA_BINARY_DIR are required")
endif()

set(work_dir "${LUNA_BINARY_DIR}/jit-aot-extended-parity")
file(REMOVE_RECURSE "${work_dir}")
file(MAKE_DIRECTORY "${work_dir}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/packages/exported_package"
     DESTINATION "${work_dir}")
file(COPY "${LUNA_SOURCE_DIR}/examples/heterogeneous.luna"
     DESTINATION "${work_dir}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/string_literal_local_cleanup.luna"
     DESTINATION "${work_dir}")
include("${LUNA_SOURCE_DIR}/tests/aot_package_fixture.cmake")

set(LUNA_EXE_SUFFIX "")
if(WIN32)
    set(LUNA_EXE_SUFFIX ".exe")
endif()

function(check_parity name input_path ir_path executable_path)
    file(REMOVE "${ir_path}" "${executable_path}")
    execute_process(
        COMMAND "${LUNA_EXECUTABLE}" run "${input_path}" -O2
        RESULT_VARIABLE jit_result
        OUTPUT_VARIABLE jit_output
        ERROR_VARIABLE jit_error
    )
    set(jit_transcript "${jit_output}\n${jit_error}")
    string(FIND "${jit_transcript}" "error[" jit_error_at)
    string(FIND "${jit_output}" "Program exited with code: ${jit_result}" jit_marker)
    if(NOT jit_error_at EQUAL -1 OR jit_marker EQUAL -1)
        file(REMOVE "${ir_path}" "${executable_path}")
        message(FATAL_ERROR
            "${name}: optimized JIT execution failed.\nResult: ${jit_result}\n"
            "Output:\n${jit_transcript}")
    endif()
    string(SUBSTRING "${jit_output}" 0 ${jit_marker} jit_program_output)

    execute_process(
        COMMAND "${LUNA_EXECUTABLE}" build "${input_path}" -O2
        RESULT_VARIABLE build_result
        OUTPUT_VARIABLE build_output
        ERROR_VARIABLE build_error
    )
    if(NOT build_result EQUAL 0 OR NOT EXISTS "${ir_path}" OR NOT EXISTS "${executable_path}")
        file(REMOVE "${ir_path}" "${executable_path}")
        message(FATAL_ERROR
            "${name}: optimized AOT build failed.\nResult: ${build_result}\n"
            "Output:\n${build_output}\n${build_error}")
    endif()

    execute_process(
        COMMAND "${executable_path}"
        RESULT_VARIABLE aot_result
        OUTPUT_VARIABLE aot_output
        ERROR_VARIABLE aot_error
    )
    file(REMOVE "${ir_path}" "${executable_path}")
    if(NOT jit_result EQUAL aot_result OR NOT "${jit_program_output}" STREQUAL "${aot_output}")
        message(FATAL_ERROR
            "${name}: optimized JIT and AOT behavior diverged.\n"
            "JIT result: ${jit_result}\nJIT stdout:\n${jit_program_output}\n"
            "AOT result: ${aot_result}\nAOT stdout:\n${aot_output}\nAOT stderr:\n${aot_error}")
    endif()
endfunction()

set(package_dir "${work_dir}/exported_package")
check_parity(
    "multi-file package"
    "${package_dir}"
    "${package_dir}/build/native/exported_package${LUNA_EXE_SUFFIX}.ll"
    "${package_dir}/build/native/exported_package${LUNA_EXE_SUFFIX}")

set(heterogeneous_source "${work_dir}/heterogeneous.luna")
luna_stage_aot_application(
    "${heterogeneous_source}" "jit_extended_heterogeneous")
check_parity(
    "heterogeneous simulator"
    "${LUNA_AOT_PACKAGE_DIR}"
    "${LUNA_AOT_IR_PATH}"
    "${LUNA_AOT_EXECUTABLE_PATH}")

set(string_cleanup_source "${work_dir}/string_literal_local_cleanup.luna")
luna_stage_aot_application(
    "${string_cleanup_source}" "jit_extended_string_cleanup")
check_parity(
    "string literal local cleanup"
    "${LUNA_AOT_PACKAGE_DIR}"
    "${LUNA_AOT_IR_PATH}"
    "${LUNA_AOT_EXECUTABLE_PATH}")
file(REMOVE_RECURSE "${work_dir}")
