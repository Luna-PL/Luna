if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR OR NOT DEFINED LUNA_BINARY_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR and LUNA_BINARY_DIR are required")
endif()

set(work_dir "${LUNA_BINARY_DIR}/gpu-target-split")
set(dump_dir "${work_dir}/unexpected-hsaco")
file(REMOVE_RECURSE "${work_dir}")
file(MAKE_DIRECTORY "${work_dir}" "${dump_dir}")
file(COPY "${LUNA_SOURCE_DIR}/examples/heterogeneous.luna" DESTINATION "${work_dir}")
set(source "${work_dir}/heterogeneous.luna")
set(executable "${work_dir}/heterogeneous")
if(WIN32)
    set(executable "${executable}.exe")
endif()

# LUNA_GPU_BACKEND belongs to the program being executed. It must not cause
# the compiler to emit a vendor code object when --gpu-target is absent.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            LUNA_GPU_BACKEND=rocm
            LUNA_GPU_DUMP_HSACO=${dump_dir}
            "${LUNA_EXECUTABLE}" build "${source}" -O2
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
file(GLOB unexpected_hsaco "${dump_dir}/*.hsaco")
if(NOT build_result EQUAL 0 OR NOT EXISTS "${executable}" OR unexpected_hsaco)
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR
        "runtime backend leaked into compile target selection\n"
        "Result: ${build_result}\n${build_output}\n${build_error}\n"
        "Unexpected HSACO: ${unexpected_hsaco}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env LUNA_GPU_BACKEND=sim "${executable}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error
)
string(FIND "${run_output}" "42" result_at)
if(NOT run_result EQUAL 0 OR result_at EQUAL -1)
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR
        "default simulator artifact did not execute after split target build\n"
        "Result: ${run_result}\n${run_output}\n${run_error}")
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${source}"
            --gpu-target=rocm:gfx1101,rocm:gfx1030
    RESULT_VARIABLE conflict_result
    OUTPUT_VARIABLE conflict_output
    ERROR_VARIABLE conflict_error
)
string(FIND "${conflict_output}\n${conflict_error}"
       "one artifact cannot contain multiple ROCm architectures yet"
       conflict_at)
file(REMOVE_RECURSE "${work_dir}")
if(NOT conflict_result EQUAL 1 OR conflict_at EQUAL -1)
    message(FATAL_ERROR
        "conflicting GPU targets were not rejected predictably\n"
        "Result: ${conflict_result}\n${conflict_output}\n${conflict_error}")
endif()
