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
