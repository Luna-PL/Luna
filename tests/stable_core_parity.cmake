# Full Week-2 behavior matrix.  Each stable-core input is executed through
# JIT and AOT at O0, O2, and O3; both its source-level exit status and observable
# stdout must agree.  This is intentionally broader than a smoke test: it
# catches optimization/linking drift in generics, traits, selectors, FFI,
# packages, and the portable GPU simulator.

if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR OR NOT DEFINED LUNA_BINARY_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR and LUNA_BINARY_DIR are required")
endif()

set(work_dir "${LUNA_BINARY_DIR}/stable-core-parity")
file(REMOVE_RECURSE "${work_dir}")
file(MAKE_DIRECTORY "${work_dir}")

set(LUNA_EXE_SUFFIX "")
if(WIN32)
    set(LUNA_EXE_SUFFIX ".exe")
endif()

function(check_parity name input_path ir_path executable_path level)
    file(REMOVE "${ir_path}" "${executable_path}")
    execute_process(
        COMMAND "${LUNA_EXECUTABLE}" run "${input_path}" "${level}"
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
            "${name} (${level}): JIT execution failed.\nResult: ${jit_result}\n"
            "Output:\n${jit_transcript}")
    endif()
    string(SUBSTRING "${jit_output}" 0 ${jit_marker} jit_program_output)

    execute_process(
        COMMAND "${LUNA_EXECUTABLE}" build "${input_path}" "${level}"
        RESULT_VARIABLE build_result
        OUTPUT_VARIABLE build_output
        ERROR_VARIABLE build_error
    )
    if(NOT build_result EQUAL 0 OR NOT EXISTS "${ir_path}" OR NOT EXISTS "${executable_path}")
        file(REMOVE "${ir_path}" "${executable_path}")
        message(FATAL_ERROR
            "${name} (${level}): AOT build failed.\nResult: ${build_result}\n"
            "Output:\n${build_output}\n${build_error}")
    endif()

    execute_process(
        COMMAND "${executable_path}"
        RESULT_VARIABLE aot_result
        OUTPUT_VARIABLE aot_output
        ERROR_VARIABLE aot_error
    )
    file(REMOVE "${ir_path}" "${executable_path}")
    if(NOT jit_result EQUAL aot_result OR
       NOT "${jit_program_output}" STREQUAL "${aot_output}" OR
       NOT "${aot_error}" STREQUAL "")
        message(FATAL_ERROR
            "${name} (${level}): JIT and AOT behavior diverged.\n"
            "JIT result: ${jit_result}\nJIT stdout:\n${jit_program_output}\n"
            "AOT result: ${aot_result}\nAOT stdout:\n${aot_output}\nAOT stderr:\n${aot_error}")
    endif()
endfunction()

function(check_file name relative_path)
    set(source_path "${LUNA_SOURCE_DIR}/${relative_path}")
    string(MAKE_C_IDENTIFIER "${relative_path}" case_id)
    set(case_dir "${work_dir}/${case_id}")
    file(MAKE_DIRECTORY "${case_dir}")
    file(COPY "${source_path}" DESTINATION "${case_dir}")
    get_filename_component(source_name "${source_path}" NAME)
    set(input_path "${case_dir}/${source_name}")
    get_filename_component(source_dir "${input_path}" DIRECTORY)
    get_filename_component(source_stem "${input_path}" NAME_WE)
    foreach(level IN ITEMS -O0 -O2 -O3)
        check_parity("${name}" "${input_path}" "${input_path}.ll"
                     "${source_dir}/${source_stem}${LUNA_EXE_SUFFIX}" "${level}")
    endforeach()
endfunction()

check_file("minimal function" "examples/minimal.luna")
check_file("operators" "examples/operators.luna")
check_file("generic monomorphization" "examples/generic.luna")
check_file("compile-time reflection" "examples/compile_time.luna")
check_file("closure" "examples/closure.luna")
check_file("ADT lowering" "examples/adt.luna")
check_file("metadata selection" "examples/versioning.luna")
check_file("dynamic metadata selection" "examples/dynamic_select.luna")
check_file("trait metadata" "examples/trait_versioning.luna")
check_file("nominal trait metadata" "examples/trait_versioned_nominal.luna")
check_file("C FFI" "examples/ffi.luna")
check_file("heterogeneous simulator" "examples/heterogeneous.luna")
check_file("heterogeneous bulk transfer" "tests/fixtures/heterogeneous_bulk_transfer.luna")

file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/packages/exported_package"
     DESTINATION "${work_dir}")
set(package_dir "${work_dir}/exported_package")
foreach(level IN ITEMS -O0 -O2 -O3)
    check_parity("multi-file exported package" "${package_dir}"
                 "${package_dir}/exported_package.ll"
                 "${package_dir}/exported_package${LUNA_EXE_SUFFIX}" "${level}")
endforeach()
file(REMOVE_RECURSE "${work_dir}")
