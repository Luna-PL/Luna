if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR OR NOT DEFINED LUNA_BINARY_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR and LUNA_BINARY_DIR are required")
endif()

set(workspace "${LUNA_SOURCE_DIR}/tests/fixtures/workspaces/local")
set(app "${workspace}/app")
set(moon "${LUNA_BINARY_DIR}/manifest-workspace.moonir")
file(REMOVE "${moon}")

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" run "${app}" --emit-moonir "${moon}"
    RESULT_VARIABLE app_result
    OUTPUT_VARIABLE app_output
    ERROR_VARIABLE app_error
)
if(NOT app_result EQUAL 42 OR NOT EXISTS "${moon}")
    file(REMOVE "${moon}")
    message(FATAL_ERROR
        "manifest workspace app failed.\nResult: ${app_result}\n${app_output}\n${app_error}")
endif()
file(READ "${moon}" moon_text)
file(REMOVE "${moon}")
string(FIND "${moon_text}" "moon.using \"org.luna.fixture.core\" as core" using_at)
if(using_at EQUAL -1)
    message(FATAL_ERROR "resolved dependency edge was not retained in MoonIR.\n${moon_text}")
endif()
string(FIND "${moon_text}" "__luna_" isolated_symbol_at)
if(isolated_symbol_at EQUAL -1)
    message(FATAL_ERROR
        "same-named declarations in separate modules did not receive isolated linkage.\n${moon_text}")
endif()

set(aot_workspace "${LUNA_BINARY_DIR}/manifest-aot-workspace")
file(REMOVE_RECURSE "${aot_workspace}")
file(COPY "${workspace}/" DESTINATION "${aot_workspace}")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build "${aot_workspace}/app"
    RESULT_VARIABLE aot_build_result
    OUTPUT_VARIABLE aot_build_output
    ERROR_VARIABLE aot_build_error
)
set(aot_executable "${aot_workspace}/app/org.luna.fixture.app")
if(WIN32)
    string(APPEND aot_executable ".exe")
endif()
if(NOT aot_build_result EQUAL 0 OR NOT EXISTS "${aot_executable}")
    file(REMOVE_RECURSE "${aot_workspace}")
    message(FATAL_ERROR
        "qualified dependency call failed to build through AOT.\n"
        "${aot_build_output}\n${aot_build_error}")
endif()
execute_process(
    COMMAND "${aot_executable}"
    RESULT_VARIABLE aot_run_result
    OUTPUT_VARIABLE aot_run_output
    ERROR_VARIABLE aot_run_error
)
file(REMOVE_RECURSE "${aot_workspace}")
if(NOT aot_run_result EQUAL 42)
    message(FATAL_ERROR
        "qualified dependency call diverged under AOT.\n"
        "Result: ${aot_run_result}\n${aot_run_output}\n${aot_run_error}")
endif()

foreach(library IN ITEMS
        "${LUNA_SOURCE_DIR}/stdlib/core"
        "${LUNA_SOURCE_DIR}/stdlib/std")
    execute_process(
        COMMAND "${LUNA_EXECUTABLE}" check "${library}"
        RESULT_VARIABLE check_result
        OUTPUT_VARIABLE check_output
        ERROR_VARIABLE check_error
    )
    if(NOT check_result EQUAL 0)
        message(FATAL_ERROR
            "standard-library package check failed for ${library}.\n"
            "${check_output}\n${check_error}")
    endif()
endforeach()

set(invalid_workspace "${LUNA_BINARY_DIR}/manifest-invalid-workspace")
file(REMOVE_RECURSE "${invalid_workspace}")
file(COPY "${workspace}/" DESTINATION "${invalid_workspace}")
file(REMOVE "${invalid_workspace}/luna.lock")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" check "${invalid_workspace}/app"
    RESULT_VARIABLE missing_lock_result
    OUTPUT_VARIABLE missing_lock_output
    ERROR_VARIABLE missing_lock_error
)
string(FIND "${missing_lock_output}\n${missing_lock_error}"
       "workspace dependencies require luna.lock" missing_lock_at)
if(NOT missing_lock_result EQUAL 1 OR missing_lock_at EQUAL -1)
    file(REMOVE_RECURSE "${invalid_workspace}")
    message(FATAL_ERROR "missing lock file was not rejected predictably")
endif()

file(REMOVE_RECURSE "${invalid_workspace}")
file(COPY "${workspace}/" DESTINATION "${invalid_workspace}")
file(WRITE "${invalid_workspace}/app/luna.package"
"[package]\nid = \"org.luna.fixture.app\"\nversion = \"1.0.0\"\nsources = [\"src\"]\n\n[dependencies]\n")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" check "${invalid_workspace}/app"
    RESULT_VARIABLE undeclared_result
    OUTPUT_VARIABLE undeclared_output
    ERROR_VARIABLE undeclared_error
)
string(FIND "${undeclared_output}\n${undeclared_error}"
       "is not declared in [dependencies]" undeclared_at)
file(REMOVE_RECURSE "${invalid_workspace}")
if(NOT undeclared_result EQUAL 1 OR undeclared_at EQUAL -1)
    message(FATAL_ERROR "source using without a manifest dependency was accepted")
endif()

file(REMOVE_RECURSE "${invalid_workspace}")
file(COPY "${workspace}/" DESTINATION "${invalid_workspace}")
file(WRITE "${invalid_workspace}/app/src/main.luna"
"package org.luna.fixture.app;\nmodule application;\nusing org.luna.fixture.core as core;\n\nfn main() -> i32 {\n    return core::values::private_value();\n}\n")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" check "${invalid_workspace}/app"
    RESULT_VARIABLE private_result
    OUTPUT_VARIABLE private_output
    ERROR_VARIABLE private_error
)
string(FIND "${private_output}\n${private_error}"
       "is private to package 'org.luna.fixture.core'" private_at)
file(REMOVE_RECURSE "${invalid_workspace}")
if(NOT private_result EQUAL 1 OR private_at EQUAL -1)
    message(FATAL_ERROR "a private dependency declaration crossed the package boundary")
endif()

file(REMOVE_RECURSE "${invalid_workspace}")
file(COPY "${workspace}/" DESTINATION "${invalid_workspace}")
file(WRITE "${invalid_workspace}/app/src/main.luna"
"package org.luna.fixture.app;\nmodule application;\nusing org.luna.fixture.core as core;\n\nnominal struct AppValue { value: i32; }\n\nimpl core::values::Describe for AppValue {\n}\n\nfn main() -> i32 { return 0; }\n")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" check "${invalid_workspace}/app"
    RESULT_VARIABLE local_target_result
    OUTPUT_VARIABLE local_target_output
    ERROR_VARIABLE local_target_error
)
if(NOT local_target_result EQUAL 0)
    file(REMOVE_RECURSE "${invalid_workspace}")
    message(FATAL_ERROR
        "foreign trait implementation for a local nominal target was rejected.\n"
        "${local_target_output}\n${local_target_error}")
endif()

file(WRITE "${invalid_workspace}/app/src/main.luna"
"package org.luna.fixture.app;\nmodule application;\nusing org.luna.fixture.core as core;\n\nimpl core::values::Describe for core::values::PublicBox {\n}\n\nfn main() -> i32 { return 0; }\n")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" check "${invalid_workspace}/app"
    RESULT_VARIABLE orphan_result
    OUTPUT_VARIABLE orphan_output
    ERROR_VARIABLE orphan_error
)
string(FIND "${orphan_output}\n${orphan_error}"
       "orphan impl of trait 'core::values::Describe'" orphan_at)
if(NOT orphan_result EQUAL 1 OR orphan_at EQUAL -1)
    file(REMOVE_RECURSE "${invalid_workspace}")
    message(FATAL_ERROR "foreign-trait/foreign-type orphan impl was accepted")
endif()

file(WRITE "${invalid_workspace}/app/src/main.luna"
"package org.luna.fixture.app;\nmodule application;\nusing org.luna.fixture.core as core;\n\nimpl Drop for core::values::PublicBox {\n    fn drop(value: &mut core::values::PublicBox) -> unit {\n    }\n}\n\nfn main() -> i32 { return 0; }\n")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" check "${invalid_workspace}/app"
    RESULT_VARIABLE drop_orphan_result
    OUTPUT_VARIABLE drop_orphan_output
    ERROR_VARIABLE drop_orphan_error
)
string(FIND "${drop_orphan_output}\n${drop_orphan_error}"
       "orphan impl of `Drop`" drop_orphan_at)
if(NOT drop_orphan_result EQUAL 1 OR drop_orphan_at EQUAL -1)
    file(REMOVE_RECURSE "${invalid_workspace}")
    message(FATAL_ERROR "foreign Drop impl was accepted")
endif()

file(WRITE "${invalid_workspace}/app/src/main.luna"
"package org.luna.fixture.app;\nmodule application;\nusing org.luna.fixture.core as core;\n\nimpl From<core::values::PublicOption> for core::values::PublicBox {\n    fn from(value: core::values::PublicOption) -> core::values::PublicBox {\n        return new core::values::PublicBox(0);\n    }\n}\n\nfn main() -> i32 { return 0; }\n")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" check "${invalid_workspace}/app"
    RESULT_VARIABLE from_orphan_result
    OUTPUT_VARIABLE from_orphan_output
    ERROR_VARIABLE from_orphan_error
)
string(FIND "${from_orphan_output}\n${from_orphan_error}"
       "orphan impl of compiler trait `From`" from_orphan_at)
file(REMOVE_RECURSE "${invalid_workspace}")
if(NOT from_orphan_result EQUAL 1 OR from_orphan_at EQUAL -1)
    message(FATAL_ERROR "foreign From impl was accepted")
endif()
