if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR OR NOT DEFINED LUNA_BINARY_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR and LUNA_BINARY_DIR are required")
endif()

set(workspace "${LUNA_BINARY_DIR}/rc-arc-core-workspace")
file(REMOVE_RECURSE "${workspace}")
file(COPY "${LUNA_SOURCE_DIR}/stdlib/" DESTINATION "${workspace}")
file(COPY "${LUNA_SOURCE_DIR}/tests/fixtures/rc_arc_core_app/"
     DESTINATION "${workspace}/app")
file(WRITE "${workspace}/luna.workspace"
"[workspace]\nmembers = [\"core\", \"sys\", \"alloc\", \"std\", \"app\"]\n")
file(APPEND "${workspace}/luna.lock"
"\n[[package]]\nid = \"org.luna.fixture.rc_arc_core\"\nversion = \"1.0.0\"\nsource = \"workspace:app\"\nhash = \"fixture-rc-arc-core-v1\"\n")

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" run "${workspace}/app"
    RESULT_VARIABLE jit_result
    OUTPUT_VARIABLE jit_output
    ERROR_VARIABLE jit_error
)
set(expected_output "7\n7\n99\n8\n8\n199\n9\n299\n")
string(FIND "${jit_output}" "${expected_output}" jit_values)
string(FIND "${jit_output}" "Program exited with code: 42" jit_marker)
if(NOT jit_result EQUAL 42 OR NOT jit_values EQUAL 0 OR jit_marker EQUAL -1)
    file(REMOVE_RECURSE "${workspace}")
    message(FATAL_ERROR
        "ordinary Core Rc/Arc JIT behavior failed.\n"
        "Result: ${jit_result}\nstdout:\n${jit_output}\nstderr:\n${jit_error}")
endif()

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${workspace}/app" -O0
            --emit-moonir "${workspace}/rc-arc.moonir"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
set(aot_executable "${workspace}/app/build/native/rc_arc_core")
if(WIN32)
    string(APPEND aot_executable ".exe")
endif()
if(NOT build_result EQUAL 0 OR NOT EXISTS "${aot_executable}")
    file(REMOVE_RECURSE "${workspace}")
    message(FATAL_ERROR
        "ordinary Core Rc/Arc AOT build failed.\n"
        "${build_output}\n${build_error}")
endif()
execute_process(
    COMMAND "${aot_executable}"
    RESULT_VARIABLE aot_result
    OUTPUT_VARIABLE aot_output
    ERROR_VARIABLE aot_error
)
if(NOT aot_result EQUAL 42 OR NOT "${aot_output}" STREQUAL "${expected_output}" OR
   NOT "${aot_error}" STREQUAL "")
    file(REMOVE_RECURSE "${workspace}")
    message(FATAL_ERROR
        "ordinary Core Rc/Arc JIT/AOT behavior diverged.\n"
        "Result: ${aot_result}\nstdout:\n${aot_output}\nstderr:\n${aot_error}")
endif()

file(READ "${workspace}/rc-arc.moonir" moon_text)
string(FIND "${moon_text}" "kind rc" old_rc_kind)
string(FIND "${moon_text}" "kind arc" old_arc_kind)
string(FIND "${moon_text}" "nominal spelling \"Rc<SharedResource>\"" rc_nominal)
string(FIND "${moon_text}" "nominal spelling \"Arc<SharedResource>\"" arc_nominal)
if(NOT old_rc_kind EQUAL -1 OR NOT old_arc_kind EQUAL -1 OR
   rc_nominal EQUAL -1 OR arc_nominal EQUAL -1)
    file(REMOVE_RECURSE "${workspace}")
    message(FATAL_ERROR
        "Rc/Arc MoonIR retained old kinds or lost ordinary nominal types.\n"
        "${moon_text}")
endif()

set(generated_executable "${workspace}/app/build/native/rc_arc_core")
if(WIN32)
    string(APPEND generated_executable ".exe")
endif()
set(generated_ir "${generated_executable}.ll")
file(READ "${generated_ir}" llvm_text)
string(FIND "${llvm_text}" "@rt_rc_allocate_v1" rc_runtime)
string(FIND "${llvm_text}" "@rt_arc_allocate_v1" arc_runtime)
if(rc_runtime EQUAL -1 OR arc_runtime EQUAL -1)
    file(REMOVE_RECURSE "${workspace}")
    message(FATAL_ERROR "ordinary Core Rc/Arc lost their Runtime ABI v1 calls")
endif()

file(WRITE "${workspace}/app/src/main.luna"
"package org.luna.fixture.rc_arc_core;\n"
"module application;\n"
"using org.luna.core as core;\n"
"fn main() -> i32 {\n"
"    let owner = core::Rc::new(7);\n"
"    let copied = owner;\n"
"    return *owner.pointer;\n"
"}\n")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" check "${workspace}/app"
    RESULT_VARIABLE copy_result
    OUTPUT_VARIABLE copy_output
    ERROR_VARIABLE copy_error
)
string(FIND "${copy_error}" "must be moved explicitly" copy_diagnostic)
if(copy_result EQUAL 0 OR copy_diagnostic EQUAL -1)
    file(REMOVE_RECURSE "${workspace}")
    message(FATAL_ERROR
        "ordinary Rc handle was implicitly copied or emitted the wrong diagnostic.\n"
        "stdout:\n${copy_output}\nstderr:\n${copy_error}")
endif()

file(REMOVE_RECURSE "${workspace}")
