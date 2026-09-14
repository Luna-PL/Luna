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
set(object_path "${executable_path}.o")
set(input_state_path "${work_dir}/build/native/.luna-build-input-state")

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
file(READ "${ir_path}" initial_ir)
string(FIND "${initial_ir}" "rt_install_application_host_services_v1"
    host_install_at)
if(NOT host_install_at EQUAL -1)
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR
        "output-only application used the heavyweight host/runtime link profile.\n"
        "Output:\n${build_output}\n${build_error}\nIR:\n${initial_ir}")
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${work_dir}" -O2
    RESULT_VARIABLE cached_build_result
    OUTPUT_VARIABLE cached_build_output
    ERROR_VARIABLE cached_build_error)
string(FIND "${cached_build_output}\n${cached_build_error}"
    "Up to date:" up_to_date_at)
string(FIND "${cached_build_output}\n${cached_build_error}"
    "Linking:" cached_linking_at)
string(FIND "${cached_build_output}\n${cached_build_error}"
    "Emitting LLVM IR:" cached_emitting_at)
string(FIND "${cached_build_output}\n${cached_build_error}"
    "Emitting native object:" cached_object_at)
if(NOT cached_build_result EQUAL 0 OR up_to_date_at EQUAL -1 OR
   NOT cached_linking_at EQUAL -1 OR NOT cached_emitting_at EQUAL -1 OR
   NOT cached_object_at EQUAL -1 OR NOT EXISTS "${input_state_path}")
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR
        "unchanged benchmark package was relinked:\n"
        "${cached_build_output}${cached_build_error}")
endif()

# A modified output must not be accepted even if its cache-state timestamp is
# made newer. The SHA-256 guard must force the fallback linker to repair it.
file(APPEND "${executable_path}" "tamper")
file(TOUCH_NOCREATE "${input_state_path}")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${work_dir}" -O2
    RESULT_VARIABLE repaired_build_result
    OUTPUT_VARIABLE repaired_build_output
    ERROR_VARIABLE repaired_build_error)
string(FIND "${repaired_build_output}\n${repaired_build_error}"
    "Linking:" repaired_linking_at)
if(NOT repaired_build_result EQUAL 0 OR repaired_linking_at EQUAL -1)
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR
        "modified cached artifact was not repaired:\n"
        "${repaired_build_output}${repaired_build_error}")
endif()

# The native object is part of the reproducible AOT cache, not a disposable
# side effect. Removing it must regenerate the object and relink the artifact.
file(REMOVE "${object_path}")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${work_dir}" -O2
    RESULT_VARIABLE object_rebuild_result
    OUTPUT_VARIABLE object_rebuild_output
    ERROR_VARIABLE object_rebuild_error)
string(FIND "${object_rebuild_output}\n${object_rebuild_error}"
    "Emitting native object:" object_rebuild_emitting_at)
string(FIND "${object_rebuild_output}\n${object_rebuild_error}"
    "Linking:" object_rebuild_linking_at)
if(NOT object_rebuild_result EQUAL 0 OR object_rebuild_emitting_at EQUAL -1 OR
   object_rebuild_linking_at EQUAL -1)
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR
        "missing cached native object was not rebuilt:\n"
        "${object_rebuild_output}${object_rebuild_error}")
endif()

execute_process(
    COMMAND "${executable_path}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error)
if(NOT run_result EQUAL 0 OR NOT run_output STREQUAL "1\n" OR
   NOT run_error STREQUAL "")
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR
        "benchmark package run failed: result=${run_result}\n"
        "stdout=${run_output}\nstderr=${run_error}")
endif()

# A content change must invalidate the input fingerprint, IR comparison, and
# link-state cache even when path and optimization level are unchanged.
file(WRITE "${work_dir}/src/main.luna"
    "fn main() -> i32 {\n"
    "    print(2);\n"
    "    return 0;\n"
    "}\n")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${work_dir}" -O2
    RESULT_VARIABLE changed_build_result
    OUTPUT_VARIABLE changed_build_output
    ERROR_VARIABLE changed_build_error)
string(FIND "${changed_build_output}\n${changed_build_error}"
    "Linking:" changed_linking_at)
string(FIND "${changed_build_output}\n${changed_build_error}"
    "Up to date:" changed_up_to_date_at)
if(NOT changed_build_result EQUAL 0 OR changed_linking_at EQUAL -1 OR
   NOT changed_up_to_date_at EQUAL -1)
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR
        "changed benchmark package did not relink:\n"
        "${changed_build_output}${changed_build_error}")
endif()
execute_process(
    COMMAND "${executable_path}"
    RESULT_VARIABLE changed_run_result
    OUTPUT_VARIABLE changed_run_output
    ERROR_VARIABLE changed_run_error)
file(REMOVE_RECURSE "${work_dir}")
if(NOT changed_run_result EQUAL 0 OR NOT changed_run_output STREQUAL "2\n" OR
   NOT changed_run_error STREQUAL "")
    message(FATAL_ERROR
        "rebuilt benchmark package was stale: result=${changed_run_result}\n"
        "stdout=${changed_run_output}\nstderr=${changed_run_error}")
endif()

message(STATUS "Luna 0.3 benchmark package smoke test passed")
