if(NOT DEFINED LUNA_EXECUTABLE OR NOT EXISTS "${LUNA_EXECUTABLE}")
    message(FATAL_ERROR "LUNA_EXECUTABLE must point at a built luna binary")
endif()
if(NOT DEFINED LUNA_SOURCE_DIR OR NOT DEFINED LUNA_BINARY_DIR)
    message(FATAL_ERROR "LUNA_SOURCE_DIR and LUNA_BINARY_DIR are required")
endif()

set(package_dir "${LUNA_SOURCE_DIR}/tests/fixtures/packages/module_headers")
set(moon_path "${LUNA_BINARY_DIR}/package-module-model.moonir")
file(REMOVE "${moon_path}")

execute_process(
    COMMAND "${LUNA_EXECUTABLE}" run "${package_dir}"
            --emit-moonir "${moon_path}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT result EQUAL 42 OR NOT EXISTS "${moon_path}")
    file(REMOVE "${moon_path}")
    message(FATAL_ERROR
        "package/module model fixture failed.\nResult: ${result}\n${output}\n${error}")
endif()

file(READ "${moon_path}" moon)
file(REMOVE "${moon_path}")
foreach(expected IN ITEMS
        "moon.module @org.luna.module_headers"
        "moon.source_module \"application\""
        "moon.source_module \"math::integer\""
        "moon.using \"org.luna.std\" as std")
    string(FIND "${moon}" "${expected}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR
            "MoonIR lost package/module identity '${expected}'.\n${moon}")
    endif()
endforeach()
