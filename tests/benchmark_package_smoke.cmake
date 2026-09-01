cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built Luna compiler")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR OR NOT DEFINED LUNA_BINARY_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR and LUNA_BINARY_DIR are required")
endif()

set(work_dir "${LUNA_BINARY_DIR}/benchmark-package-smoke")
file(REMOVE_RECURSE "${work_dir}")
file(MAKE_DIRECTORY "${work_dir}/src")
file(COPY_FILE
     "${LUNA_SOURCE_DIR}/benchmarks/luna_cpu_arithmetic.luna"
     "${work_dir}/src/main.luna")
file(WRITE "${work_dir}/luna.package"
     "[package]\n"
     "id = \"org.luna.benchmark.smoke\"\n"
     "version = \"0.3.0\"\n"
     "kind = \"application\"\n"
     "sources = [\"src\"]\n")

set(executable_suffix "")
if(WIN32)
    set(executable_suffix ".exe")
endif()
set(ir_path "${work_dir}/build/native/smoke${executable_suffix}.ll")
set(executable_path "${work_dir}/build/native/smoke${executable_suffix}")

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${work_dir}" -O2
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0 OR
   NOT EXISTS "${ir_path}" OR NOT EXISTS "${executable_path}")
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR
        "benchmark package build failed:\n${build_output}${build_error}")
endif()

execute_process(
    COMMAND "${executable_path}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error)
file(REMOVE_RECURSE "${work_dir}")
if(NOT run_result EQUAL 0 OR NOT run_output STREQUAL "1\n" OR
   NOT run_error STREQUAL "")
    message(FATAL_ERROR
        "benchmark package run failed: result=${run_result}\n"
        "stdout=${run_output}\nstderr=${run_error}")
endif()

message(STATUS "Luna 0.3 benchmark package smoke test passed")
