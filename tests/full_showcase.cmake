# The executable showcase is both user-facing documentation and an integration
# boundary.  Keep it honest across package loading, MoonIR, JIT, AOT and the
# portable kernel simulator.

if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR OR NOT DEFINED LUNA_BINARY_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR and LUNA_BINARY_DIR are required")
endif()

set(work_dir "${LUNA_BINARY_DIR}/full-showcase")
set(showcase_dir "${work_dir}/full_showcase")
set(app_dir "${showcase_dir}/app")
set(moonir_path "${work_dir}/showcase.moonir")
set(package_id "org.luna.examples.showcase.app")
set(executable_path "${app_dir}/build/native/app")
if(WIN32)
    string(APPEND executable_path ".exe")
endif()
set(ir_path "${executable_path}.ll")

file(REMOVE_RECURSE "${work_dir}")
file(MAKE_DIRECTORY "${work_dir}")
file(COPY "${LUNA_SOURCE_DIR}/examples/full_showcase" DESTINATION "${work_dir}")

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" check "${app_dir}"
            --emit-moonir "${moonir_path}"
    RESULT_VARIABLE check_result
    OUTPUT_VARIABLE check_output
    ERROR_VARIABLE check_error
)
if(NOT check_result EQUAL 0 OR NOT EXISTS "${moonir_path}")
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR
        "full showcase failed semantic/MoonIR validation.\n"
        "Result: ${check_result}\n${check_output}\n${check_error}")
endif()

file(READ "${moonir_path}" moonir)
foreach(required_text IN ITEMS
        "moon.module @org.luna.examples.showcase.app"
        "moon.using \"org.luna.examples.showcase.foundation\" as showcase"
        "::data::model::trait::Tagged"
        "generic_recipe"
        " instantiation"
        "retention runtime"
        " kernel")
    string(FIND "${moonir}" "${required_text}" required_at)
    if(required_at EQUAL -1)
        file(REMOVE_RECURSE "${work_dir}")
        message(FATAL_ERROR
            "full showcase MoonIR is missing '${required_text}'.\n${moonir}")
    endif()
endforeach()
string(FIND "${moonir}" "dynamic_select" dynamic_select_at)
if(NOT dynamic_select_at EQUAL -1)
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR "full showcase retained the removed dynamic-select protocol.\n${moonir}")
endif()
string(FIND "${moonir}" "unresolved" unresolved_at)
if(NOT unresolved_at EQUAL -1)
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR "full showcase leaked an unresolved type into MoonIR.\n${moonir}")
endif()

function(run_showcase_jit optimization output_var)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env LUNA_GPU_BACKEND=sim
                "${LUNA_EXECUTABLE}" run "${app_dir}" "${optimization}"
        RESULT_VARIABLE jit_result
        OUTPUT_VARIABLE jit_output
        ERROR_VARIABLE jit_error
    )
    set(jit_transcript "${jit_output}\n${jit_error}")
    string(FIND "${jit_transcript}" "error[" jit_error_at)
    string(FIND "${jit_output}" "Program exited with code: 42" jit_marker)
    if(NOT jit_result EQUAL 42 OR NOT jit_error_at EQUAL -1 OR jit_marker EQUAL -1)
        file(REMOVE_RECURSE "${work_dir}")
        message(FATAL_ERROR
            "full showcase JIT ${optimization} failed.\n"
            "Result: ${jit_result}\n${jit_transcript}")
    endif()
    string(SUBSTRING "${jit_output}" 0 ${jit_marker} program_output)
    set("${output_var}" "${program_output}" PARENT_SCOPE)
endfunction()

run_showcase_jit("-O0" jit_o0_output)
run_showcase_jit("-O2" jit_o2_output)
if(NOT "${jit_o0_output}" STREQUAL "${jit_o2_output}")
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR
        "full showcase O0/O2 JIT output diverged.\n"
        "O0:\n${jit_o0_output}\nO2:\n${jit_o2_output}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env LUNA_GPU_BACKEND=sim
            "${LUNA_EXECUTABLE}" build "${app_dir}" -O2
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0 OR NOT EXISTS "${ir_path}" OR
   NOT EXISTS "${executable_path}")
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR
        "full showcase AOT build failed.\n"
        "Result: ${build_result}\n${build_output}\n${build_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env LUNA_GPU_BACKEND=sim "${executable_path}"
    RESULT_VARIABLE aot_result
    OUTPUT_VARIABLE aot_output
    ERROR_VARIABLE aot_error
)
if(NOT aot_result EQUAL 42 OR NOT "${aot_output}" STREQUAL "${jit_o2_output}")
    file(REMOVE_RECURSE "${work_dir}")
    message(FATAL_ERROR
        "full showcase JIT/AOT behavior diverged.\n"
        "JIT stdout:\n${jit_o2_output}\n"
        "AOT result: ${aot_result}\nAOT stdout:\n${aot_output}\n"
        "AOT stderr:\n${aot_error}")
endif()

file(REMOVE_RECURSE "${work_dir}")
