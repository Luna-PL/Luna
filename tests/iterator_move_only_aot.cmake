if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

set(source_path
    "${LUNA_SOURCE_DIR}/tests/fixtures/iterator_move_only_array.luna")
include("${LUNA_SOURCE_DIR}/tests/aot_package_fixture.cmake")
luna_stage_aot_application("${source_path}" "iterator_move_only_aot")
set(ir_path "${LUNA_AOT_IR_PATH}")
set(executable_path "${LUNA_AOT_EXECUTABLE_PATH}")
set(expected_output
    "61\n62\n71\n71\n72\n72\n81\n81\n82\n83\n91\n92\n91\n101\n102\n102\n103\n121\n122\n122\n123\n131\n231\n141\n142\n1\n143\n1\n151\n151\n161\n161\n171\n171\n181\n182\n184\n184\n")

file(REMOVE "${ir_path}" "${executable_path}")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" run "${source_path}"
    RESULT_VARIABLE jit_result
    OUTPUT_VARIABLE jit_output
    ERROR_VARIABLE jit_error
)
set(expected_jit
    "${expected_output}Program exited with code: 0\n")
if(NOT jit_result EQUAL 0 OR
   NOT "${jit_output}" STREQUAL "${expected_jit}" OR
   NOT "${jit_error}" STREQUAL "")
    file(REMOVE "${ir_path}" "${executable_path}")
    message(FATAL_ERROR
        "move-only iterator JIT behavior diverged.\n"
        "stdout:\n${jit_output}\nstderr:\n${jit_error}")
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${LUNA_AOT_PACKAGE_DIR}" -O0
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0 OR
   NOT EXISTS "${ir_path}" OR
   NOT EXISTS "${executable_path}")
    file(REMOVE "${ir_path}" "${executable_path}")
    message(FATAL_ERROR
        "move-only iterator AOT build failed.\n"
        "${build_output}\n${build_error}")
endif()

file(READ "${ir_path}" generated_ir)

execute_process(
    COMMAND "${executable_path}"
    RESULT_VARIABLE aot_result
    OUTPUT_VARIABLE aot_output
    ERROR_VARIABLE aot_error
)
file(REMOVE "${ir_path}" "${executable_path}")
if(NOT aot_result EQUAL 0 OR
   NOT "${aot_output}" STREQUAL "${expected_output}" OR
   NOT "${aot_error}" STREQUAL "")
    message(FATAL_ERROR
        "move-only iterator JIT/AOT behavior diverged.\n"
        "stdout:\n${aot_output}\nstderr:\n${aot_error}")
endif()
