if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR OR NOT DEFINED LUNA_BINARY_DIR OR
   NOT DEFINED LUNA_C_COMPILER)
    message(FATAL_ERROR
        "LUNA_SOURCE_DIR, LUNA_BINARY_DIR, and LUNA_C_COMPILER are required")
endif()

set(work_dir "${LUNA_BINARY_DIR}/cffi-artifact-test")
file(REMOVE_RECURSE "${work_dir}")
file(MAKE_DIRECTORY "${work_dir}")
foreach(package IN ITEMS cffi_library cffi_no_exports cffi_typed_export
        package_kind_application)
    file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/packages/${package}"
         DESTINATION "${work_dir}")
endforeach()

set(package_dir "${work_dir}/cffi_library")
if(WIN32)
    set(library_path "${package_dir}/build/cffi/cffi.dll")
elseif(APPLE)
    set(library_path "${package_dir}/build/cffi/libcffi.dylib")
else()
    set(library_path "${package_dir}/build/cffi/libcffi.so")
endif()
set(header_path "${package_dir}/build/cffi/cffi.h")

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${package_dir}" -t cffi
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0 OR NOT EXISTS "${library_path}" OR
   NOT EXISTS "${header_path}")
    message(FATAL_ERROR
        "CFFI build did not produce the platform library and header.\n"
        "result=${build_result}\n${build_output}\n${build_error}")
endif()

file(READ "${header_path}" header)
foreach(signature IN ITEMS
        "int32_t luna_add(int32_t arg0, int32_t arg1);"
        "double luna_scale(double arg0, double arg1);")
    string(FIND "${header}" "${signature}" signature_position)
    if(signature_position EQUAL -1)
        message(FATAL_ERROR
            "generated header omitted '${signature}':\n${header}")
    endif()
endforeach()

set(consumer "${work_dir}/cffi_consumer.c")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/cffi_consumer.c"
     DESTINATION "${work_dir}")
if(WIN32)
    execute_process(
        COMMAND "${LUNA_C_COMPILER}" -std=c11 -Wall -Wextra -Werror
            -I "${package_dir}/build/cffi" -c "${consumer}"
            -o "${work_dir}/cffi_consumer.obj"
        RESULT_VARIABLE consumer_compile_result
        OUTPUT_VARIABLE consumer_compile_output
        ERROR_VARIABLE consumer_compile_error)
else()
    execute_process(
        COMMAND "${LUNA_C_COMPILER}" -std=c11 -Wall -Wextra -Werror
            -I "${package_dir}/build/cffi" "${consumer}" "${library_path}"
            -o "${work_dir}/cffi_consumer"
        RESULT_VARIABLE consumer_compile_result
        OUTPUT_VARIABLE consumer_compile_output
        ERROR_VARIABLE consumer_compile_error)
endif()
if(NOT consumer_compile_result EQUAL 0)
    message(FATAL_ERROR
        "a strict C11 consumer could not compile against the generated header.\n"
        "${consumer_compile_output}\n${consumer_compile_error}")
endif()
if(NOT WIN32)
    execute_process(
        COMMAND "${work_dir}/cffi_consumer"
        RESULT_VARIABLE consumer_result
        OUTPUT_VARIABLE consumer_output
        ERROR_VARIABLE consumer_error)
    if(NOT consumer_result EQUAL 0)
        message(FATAL_ERROR
            "C consumer failed through the generated shared library: "
            "${consumer_result}\n${consumer_output}\n${consumer_error}")
    endif()
endif()

# The Native application side of T003 uses the same manifest kind but a
# different artifact boundary and default directory.
set(native_package "${work_dir}/package_kind_application")
set(native_output
    "${native_package}/build/native/application_kind")
if(WIN32)
    string(APPEND native_output ".exe")
endif()
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${native_package}" -t native
    RESULT_VARIABLE native_result
    OUTPUT_VARIABLE native_stdout
    ERROR_VARIABLE native_stderr)
if(NOT native_result EQUAL 0 OR NOT EXISTS "${native_output}" OR
   EXISTS "${native_package}/org.luna.fixture.application_kind")
    message(FATAL_ERROR
        "Native application did not use build/native package output.\n"
        "${native_stdout}\n${native_stderr}")
endif()
execute_process(
    COMMAND "${native_output}"
    RESULT_VARIABLE native_run_result
    OUTPUT_VARIABLE native_run_stdout
    ERROR_VARIABLE native_run_stderr)
if(NOT native_run_result EQUAL 0)
    message(FATAL_ERROR
        "default Native package artifact did not execute.\n"
        "${native_run_stdout}\n${native_run_stderr}")
endif()

set(unspecified_package "${work_dir}/package_kind_unspecified")
file(COPY "${native_package}/" DESTINATION "${unspecified_package}")
file(READ "${unspecified_package}/luna.package" unspecified_manifest)
string(REPLACE "kind = \"application\"\n" ""
       unspecified_manifest "${unspecified_manifest}")
file(WRITE "${unspecified_package}/luna.package" "${unspecified_manifest}")

function(expect_target_failure name package target diagnostic)
    set(output_path "${work_dir}/${name}.artifact")
    file(REMOVE "${output_path}" "${output_path}.ll")
    execute_process(
        COMMAND "${LUNA_EXECUTABLE}" build "${package}" -t "${target}"
            -o "${output_path}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(result EQUAL 0 OR EXISTS "${output_path}")
        message(FATAL_ERROR
            "${name} unexpectedly produced an artifact.\n${output}\n${error}")
    endif()
    string(FIND "${output}${error}" "${diagnostic}" diagnostic_position)
    if(diagnostic_position EQUAL -1)
        message(FATAL_ERROR
            "${name} omitted expected diagnostic '${diagnostic}'.\n"
            "${output}\n${error}")
    endif()
endfunction()

expect_target_failure(
    cffi_application "${work_dir}/package_kind_application" cffi
    "requires kind = \"library\"")
expect_target_failure(
    cffi_no_exports "${work_dir}/cffi_no_exports" cffi
    "must export at least one")
expect_target_failure(
    cffi_typed_export "${work_dir}/cffi_typed_export" cffi
    "contains non-C export")
expect_target_failure(
    cffi_standalone "${package_dir}/src/api.luna" cffi
    "formal artifact builds require a package directory")
expect_target_failure(
    native_standalone "${native_package}/src/main.luna" native
    "formal artifact builds require a package directory")
expect_target_failure(
    native_unspecified_kind "${unspecified_package}" native
    "requires id, version, kind")
expect_target_failure(
    native_library "${package_dir}" native
    "Native library proof emission is not implemented")

file(REMOVE_RECURSE "${work_dir}")
