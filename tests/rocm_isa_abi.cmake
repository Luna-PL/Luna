if(NOT DEFINED LUNA_EXECUTABLE OR NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_EXECUTABLE and LUNA_SOURCE_DIR are required")
endif()

# llvm-objdump lives under /usr/lib/llvm-*/bin on Ubuntu/Debian and is often
# version-suffixed (llvm-objdump-22).  Search a generous set of names and hints
# so the test runs on GitHub Actions runners without manual PATH changes.
set(LUNA_LLVM_TOOL_HINTS
    /usr/lib/llvm-22/bin /usr/lib/llvm-21/bin /usr/lib/llvm-20/bin
    /usr/lib/llvm-19/bin /usr/lib/llvm-18/bin
    /usr/local/opt/llvm/bin
)
find_program(LUNA_LLVM_OBJDUMP
    NAMES llvm-objdump-22 llvm-objdump-21 llvm-objdump-20
          llvm-objdump-19 llvm-objdump-18 llvm-objdump
    HINTS ${LUNA_LLVM_TOOL_HINTS}
    DOC "LLVM objdump for AMDGPU ISA verification")
if(NOT LUNA_LLVM_OBJDUMP)
    message(STATUS "llvm-objdump not found — ROCm ISA ABI test will be skipped")
    return()
endif()
find_program(LUNA_LLVM_READOBJ
    NAMES llvm-readobj-22 llvm-readobj-21 llvm-readobj-20
          llvm-readobj-19 llvm-readobj-18 llvm-readobj
    HINTS ${LUNA_LLVM_TOOL_HINTS}
    DOC "LLVM readobj for AMDGPU metadata verification")
if(NOT LUNA_LLVM_READOBJ)
    message(STATUS "llvm-readobj not found — ROCm ISA ABI test will be skipped")
    return()
endif()
set(source "${LUNA_SOURCE_DIR}/benchmarks/luna_gpu_vector.luna")
include("${LUNA_SOURCE_DIR}/tests/aot_package_fixture.cmake")
luna_stage_aot_application("${source}" "rocm_isa_abi")
set(dump_dir "${CMAKE_CURRENT_BINARY_DIR}/luna-rocm-isa")
file(REMOVE_RECURSE "${dump_dir}")
file(MAKE_DIRECTORY "${dump_dir}")

execute_process(
    COMMAND ${CMAKE_COMMAND} -E env
        # Runtime backend selection must not influence compilation. An invalid
        # execution backend is therefore harmless for this AOT-only step.
        LUNA_GPU_BACKEND=invalid-backend
        LUNA_GPU_DUMP_HSACO=${dump_dir}
        "${LUNA_EXECUTABLE}" build "${LUNA_AOT_PACKAGE_DIR}" -O3
        --gpu-target=rocm:gfx1101
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0)
    message(STATUS "offline ROCm code generation failed (likely no AMDGPU target on this platform) — skipping ISA ABI check")
    file(REMOVE_RECURSE "${dump_dir}")
    return()
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

file(REMOVE_RECURSE "${LUNA_AOT_PACKAGE_DIR}")
file(REMOVE_RECURSE "${dump_dir}")
