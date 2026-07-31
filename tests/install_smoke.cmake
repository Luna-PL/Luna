if(NOT DEFINED LUNA_BINARY_DIR OR
   NOT DEFINED LUNA_AOT_COMPILER OR
   NOT DEFINED LUNA_RUNTIME_FILE)
    message(FATAL_ERROR
        "install smoke requires the build directory, AOT compiler, and "
        "runtime archive name")
endif()

foreach(required
        LUNA_INSTALL_BINDIR
        LUNA_INSTALL_LIBDIR
        LUNA_INSTALL_INCLUDEDIR
        LUNA_INSTALL_DATADIR
        LUNA_INSTALL_DOCDIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "install smoke is missing ${required}")
    endif()
endforeach()

set(stage "${LUNA_BINARY_DIR}/install-smoke-stage")
set(work "${stage}/standalone")
file(REMOVE_RECURSE "${stage}")

set(install_command
    "${CMAKE_COMMAND}" --install "${LUNA_BINARY_DIR}" --prefix "${stage}")
if(DEFINED LUNA_BUILD_CONFIG AND NOT LUNA_BUILD_CONFIG STREQUAL "")
    list(APPEND install_command --config "${LUNA_BUILD_CONFIG}")
endif()
execute_process(
    COMMAND ${install_command}
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error
)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR
        "staged installation failed.\n${install_output}\n${install_error}")
endif()

set(installed_luna
    "${stage}/${LUNA_INSTALL_BINDIR}/luna${LUNA_EXECUTABLE_SUFFIX}")
set(installed_runtime
    "${stage}/${LUNA_INSTALL_LIBDIR}/${LUNA_RUNTIME_FILE}")
set(installed_runtime_header
    "${stage}/${LUNA_INSTALL_INCLUDEDIR}/luna/runtime/RuntimeABI.h")
set(installed_stdlib
    "${stage}/${LUNA_INSTALL_DATADIR}/luna/stdlib/luna.workspace")
set(installed_type_reference
    "${stage}/${LUNA_INSTALL_DOCDIR}/docs/reference/type_system.md")

foreach(installed
        "${installed_luna}"
        "${installed_runtime}"
        "${installed_runtime_header}"
        "${installed_stdlib}"
        "${installed_type_reference}")
    if(NOT EXISTS "${installed}")
        message(FATAL_ERROR
            "staged installation is missing required artifact: ${installed}")
    endif()
endforeach()

execute_process(
    COMMAND "${installed_luna}" --version
    RESULT_VARIABLE version_result
    OUTPUT_VARIABLE version_output
    ERROR_VARIABLE version_error
)
if(NOT version_result EQUAL 0 OR
   NOT version_output MATCHES "^Luna 0\\.2\\.0-alpha")
    message(FATAL_ERROR
        "installed driver reported an unexpected version.\n"
        "Result: ${version_result}\n${version_output}\n${version_error}")
endif()

file(MAKE_DIRECTORY "${work}")
set(source "${work}/install_smoke.luna")
file(WRITE "${source}"
"fn main() -> i32 {\n"
"    print(\"installed-luna-smoke\");\n"
"    return 0;\n"
"}\n")

execute_process(
    COMMAND "${installed_luna}" check "${source}"
    RESULT_VARIABLE check_result
    OUTPUT_VARIABLE check_output
    ERROR_VARIABLE check_error
)
if(NOT check_result EQUAL 0)
    message(FATAL_ERROR
        "installed driver failed to check a standalone source.\n"
        "${check_output}\n${check_error}")
endif()

execute_process(
    COMMAND "${installed_luna}" run "${source}" -O2
    RESULT_VARIABLE jit_result
    OUTPUT_VARIABLE jit_output
    ERROR_VARIABLE jit_error
)
if(NOT jit_result EQUAL 0 OR
   NOT jit_output MATCHES "installed-luna-smoke")
    message(FATAL_ERROR
        "installed driver JIT smoke failed.\n"
        "Result: ${jit_result}\n${jit_output}\n${jit_error}")
endif()

execute_process(
    COMMAND "${installed_luna}" build "${source}" -O2
            --runtime-lib "${installed_runtime}"
            --cc "${LUNA_AOT_COMPILER}"
    RESULT_VARIABLE aot_build_result
    OUTPUT_VARIABLE aot_build_output
    ERROR_VARIABLE aot_build_error
)
set(aot_executable
    "${work}/install_smoke${LUNA_EXECUTABLE_SUFFIX}")
if(NOT aot_build_result EQUAL 0 OR
   NOT EXISTS "${aot_executable}")
    message(FATAL_ERROR
        "installed driver AOT build failed.\n"
        "Result: ${aot_build_result}\n"
        "${aot_build_output}\n${aot_build_error}")
endif()

execute_process(
    COMMAND "${aot_executable}"
    RESULT_VARIABLE aot_result
    OUTPUT_VARIABLE aot_output
    ERROR_VARIABLE aot_error
)
if(NOT aot_result EQUAL 0 OR
   NOT aot_output MATCHES "installed-luna-smoke" OR
   NOT aot_error STREQUAL "")
    message(FATAL_ERROR
        "installed AOT executable smoke failed.\n"
        "Result: ${aot_result}\n${aot_output}\n${aot_error}")
endif()

file(REMOVE_RECURSE "${stage}")
