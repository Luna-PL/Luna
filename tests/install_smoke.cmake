if(NOT DEFINED LUNA_BINARY_DIR OR
   NOT DEFINED LUNA_VERSION OR
   NOT DEFINED LUNA_AOT_COMPILER OR
   NOT DEFINED LUNA_CXX_COMPILER OR
   NOT DEFINED LUNA_CMAKE_GENERATOR OR
   NOT DEFINED LUNA_RUNTIME_FILE)
    message(FATAL_ERROR
        "install smoke requires the build directory, Luna version, "
        "AOT compiler, and runtime archive name")
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
set(installed_descriptor_header
    "${stage}/${LUNA_INSTALL_INCLUDEDIR}/luna/runtime/RuntimeDescriptorABI.h")
set(installed_evolution_header
    "${stage}/${LUNA_INSTALL_INCLUDEDIR}/luna/runtime/Evolution.h")
set(installed_moon_runtime_header
    "${stage}/${LUNA_INSTALL_INCLUDEDIR}/luna/runtime/MoonRuntime.h")
set(installed_stdlib
    "${stage}/${LUNA_INSTALL_DATADIR}/luna/stdlib/luna.workspace")
set(installed_type_reference
    "${stage}/${LUNA_INSTALL_DOCDIR}/docs/reference/type_system.md")

foreach(installed
        "${installed_luna}"
        "${installed_runtime}"
        "${installed_runtime_header}"
        "${installed_descriptor_header}"
        "${installed_evolution_header}"
        "${installed_moon_runtime_header}"
        "${installed_stdlib}"
        "${installed_type_reference}")
    if(NOT EXISTS "${installed}")
        message(FATAL_ERROR
            "staged installation is missing required artifact: ${installed}")
    endif()
endforeach()

set(retired_fragment_plugin_header
    "${stage}/${LUNA_INSTALL_INCLUDEDIR}/luna/runtime/FragmentPluginABI.h")
if(EXISTS "${retired_fragment_plugin_header}")
    message(FATAL_ERROR
        "staged installation retained removed fragment plugin ABI header")
endif()

# Compile and link one independent C++17 consumer against the installed
# evolution header and runtime archive. This catches a header that merely
# exists in the package but cannot be consumed from its public include path.
set(host_api_source_dir "${work}/evolution-host-source")
set(host_api_build_dir "${work}/evolution-host-build")
file(MAKE_DIRECTORY "${host_api_source_dir}")
file(WRITE "${host_api_source_dir}/main.cpp"
"#include <luna/runtime/Evolution.h>\n"
"static_assert(luna::runtime::EvolutionApiVersion == 1);\n"
"int main() {\n"
"    luna::runtime::MoonRuntime runtime;\n"
"    return runtime.activeGenerationId(\"missing\") == 0 ? 0 : 1;\n"
"}\n")
file(WRITE "${host_api_source_dir}/CMakeLists.txt"
"cmake_minimum_required(VERSION 3.20)\n"
"project(LunaEvolutionInstallConsumer LANGUAGES CXX)\n"
"find_package(Threads REQUIRED)\n"
"add_executable(evolution-install-consumer main.cpp)\n"
"set_property(TARGET evolution-install-consumer PROPERTY CXX_STANDARD 17)\n"
"set_property(TARGET evolution-install-consumer PROPERTY CXX_STANDARD_REQUIRED ON)\n"
"target_include_directories(evolution-install-consumer PRIVATE [==[${stage}/${LUNA_INSTALL_INCLUDEDIR}]==])\n"
"target_link_libraries(evolution-install-consumer PRIVATE [==[${installed_runtime}]==] Threads::Threads ${CMAKE_DL_LIBS})\n")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${host_api_source_dir}"
        -B "${host_api_build_dir}"
        -G "${LUNA_CMAKE_GENERATOR}"
        -DCMAKE_CXX_COMPILER=${LUNA_CXX_COMPILER}
    RESULT_VARIABLE host_api_configure_result
    OUTPUT_VARIABLE host_api_configure_output
    ERROR_VARIABLE host_api_configure_error
)
if(NOT host_api_configure_result EQUAL 0)
    message(FATAL_ERROR
        "installed evolution API consumer failed to configure.\n"
        "${host_api_configure_output}\n${host_api_configure_error}")
endif()

set(host_api_build_command
    "${CMAKE_COMMAND}" --build "${host_api_build_dir}")
if(DEFINED LUNA_BUILD_CONFIG AND NOT LUNA_BUILD_CONFIG STREQUAL "")
    list(APPEND host_api_build_command --config "${LUNA_BUILD_CONFIG}")
endif()
execute_process(
    COMMAND ${host_api_build_command}
    RESULT_VARIABLE host_api_build_result
    OUTPUT_VARIABLE host_api_build_output
    ERROR_VARIABLE host_api_build_error
)
if(NOT host_api_build_result EQUAL 0)
    message(FATAL_ERROR
        "installed evolution API consumer failed to build.\n"
        "${host_api_build_output}\n${host_api_build_error}")
endif()

execute_process(
    COMMAND "${installed_luna}" --version
    RESULT_VARIABLE version_result
    OUTPUT_VARIABLE version_output
    ERROR_VARIABLE version_error
)
string(STRIP "${version_output}" reported_version)
if(NOT version_result EQUAL 0 OR
   NOT "${reported_version}" STREQUAL "Luna ${LUNA_VERSION}")
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
set(aot_package "${work}/aot-package")
file(MAKE_DIRECTORY "${aot_package}/src")
file(COPY "${source}" DESTINATION "${aot_package}/src")
file(WRITE "${aot_package}/luna.package"
"[package]\n"
"id = \"org.luna.fixture.install_smoke\"\n"
"version = \"1.0.0\"\n"
"kind = \"application\"\n"
"sources = [\"src\"]\n")

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
    COMMAND "${installed_luna}" build "${aot_package}" -O2
            --runtime-lib "${installed_runtime}"
            --cc "${LUNA_AOT_COMPILER}"
    RESULT_VARIABLE aot_build_result
    OUTPUT_VARIABLE aot_build_output
    ERROR_VARIABLE aot_build_error
)
set(aot_executable
    "${aot_package}/build/native/install_smoke${LUNA_EXECUTABLE_SUFFIX}")
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
