if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR must point at the source tree")
endif()

set(source_path
    "${LUNA_SOURCE_DIR}/tests/fixtures/iterator_materialized_move_only.luna")
set(ir_path "${source_path}.ll")
set(executable_path
    "${LUNA_SOURCE_DIR}/tests/fixtures/iterator_materialized_move_only")
if(WIN32)
    string(APPEND executable_path ".exe")
endif()
set(expected_output
    "1201\n201\n202\n203\n211\n212\n221\n222\n7\n231\n232\n233\n231\n241\n242\n9\n1241\n241\n1242\n242\n10\n")

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
        "owning materialized iterator JIT behavior diverged.\n"
        "stdout:\n${jit_output}\nstderr:\n${jit_error}")
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${source_path}" -O0
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0 OR
   NOT EXISTS "${ir_path}" OR
   NOT EXISTS "${executable_path}")
    file(REMOVE "${ir_path}" "${executable_path}")
    message(FATAL_ERROR
        "owning materialized iterator AOT build failed.\n"
        "${build_output}\n${build_error}")
endif()

file(READ "${ir_path}" generated_ir)
foreach(expected_ir IN ITEMS
        "pending.iterator.source.flags"
        "pending.iterator.index"
        "iter.item.initialized"
        "store i1 false"
        "pending.iterator.source.drop.2")
    string(FIND "${generated_ir}" "${expected_ir}" expected_ir_at)
    if(expected_ir_at EQUAL -1)
        file(REMOVE "${ir_path}" "${executable_path}")
        message(FATAL_ERROR
            "owning materialized iterator IR lacks '${expected_ir}'")
    endif()
endforeach()
string(FIND "${generated_ir}" "@rt_iterator" runtime_iterator_at)
if(NOT runtime_iterator_at EQUAL -1)
    file(REMOVE "${ir_path}" "${executable_path}")
    message(FATAL_ERROR
        "owning materialized iterator unexpectedly uses a runtime iterator")
endif()

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
        "owning materialized iterator JIT/AOT behavior diverged.\n"
        "stdout:\n${aot_output}\nstderr:\n${aot_error}")
endif()
