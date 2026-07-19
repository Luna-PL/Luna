if(NOT DEFINED LUNA_EXECUTABLE OR NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_EXECUTABLE and LUNA_SOURCE_DIR are required")
endif()

find_program(LUNA_LLVM_OBJDUMP NAMES llvm-objdump REQUIRED)
find_program(LUNA_LLVM_READOBJ NAMES llvm-readobj REQUIRED)
set(source "${LUNA_SOURCE_DIR}/benchmarks/luna_gpu_vector.luna")
set(dump_dir "${CMAKE_CURRENT_BINARY_DIR}/luna-rocm-isa")
file(REMOVE_RECURSE "${dump_dir}")
file(MAKE_DIRECTORY "${dump_dir}")

execute_process(
    COMMAND ${CMAKE_COMMAND} -E env
        LUNA_GPU_BACKEND=sim
        LUNA_GPU_EMIT_AMDGPU=1
        LUNA_AMDGPU_ARCH=gfx1101
        LUNA_GPU_DUMP_HSACO=${dump_dir}
        "${LUNA_EXECUTABLE}" build "${source}" -O3
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "offline ROCm code generation failed:\n${build_output}\n${build_error}")
endif()

file(GLOB hsaco_files "${dump_dir}/*.hsaco")
list(LENGTH hsaco_files hsaco_count)
if(hsaco_count LESS 2)
    message(FATAL_ERROR "expected both benchmark kernels in offline HSACO dump, found ${hsaco_count}")
endif()

foreach(hsaco IN LISTS hsaco_files)
    execute_process(
        COMMAND "${LUNA_LLVM_OBJDUMP}" -d "${hsaco}"
        RESULT_VARIABLE objdump_result
        OUTPUT_VARIABLE disassembly
        ERROR_VARIABLE objdump_error
    )
    if(NOT objdump_result EQUAL 0)
        message(FATAL_ERROR "could not disassemble ${hsaco}: ${objdump_error}")
    endif()
    if(NOT disassembly MATCHES "global_(load|store)_b32")
        message(FATAL_ERROR "${hsaco} has no global 32-bit memory instruction:\n${disassembly}")
    endif()
    if(disassembly MATCHES "flat_(load|store)_b32")
        message(FATAL_ERROR "${hsaco} regressed to flat memory addressing:\n${disassembly}")
    endif()

    execute_process(
        COMMAND "${LUNA_LLVM_READOBJ}" --notes "${hsaco}"
        RESULT_VARIABLE readobj_result
        OUTPUT_VARIABLE metadata
        ERROR_VARIABLE readobj_error
    )
    if(NOT readobj_result EQUAL 0)
        message(FATAL_ERROR "could not read ${hsaco} metadata: ${readobj_error}")
    endif()
    if(NOT metadata MATCHES "kernarg_segment_size: 16")
        message(FATAL_ERROR "${hsaco} has an unexpected kernarg ABI; expected 16 bytes:\n${metadata}")
    endif()
    if(metadata MATCHES "hidden_(hostcall|multigrid|heap|default_queue|completion_action|queue_ptr)")
        message(FATAL_ERROR "${hsaco} contains forbidden hidden runtime arguments:\n${metadata}")
    endif()
endforeach()

file(REMOVE
    "${LUNA_SOURCE_DIR}/benchmarks/luna_gpu_vector.luna.ll"
    "${LUNA_SOURCE_DIR}/benchmarks/luna_gpu_vector"
)
file(REMOVE_RECURSE "${dump_dir}")
