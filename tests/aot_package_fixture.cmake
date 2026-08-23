# Stage one legacy single-file fixture as a formal Luna application package.
# Production `build` is package-only; `check`, `run`, and `analyze` intentionally
# retain standalone-file support. Tests use this helper rather than weakening
# the production driver contract.

function(luna_stage_aot_application source_path fixture_name)
    if(DEFINED LUNA_BINARY_DIR)
        set(fixture_root "${LUNA_BINARY_DIR}/aot-package-fixtures")
    else()
        set(fixture_root "${CMAKE_CURRENT_BINARY_DIR}/aot-package-fixtures")
    endif()

    string(REGEX REPLACE "[^A-Za-z0-9_]" "_" package_component
        "${fixture_name}")
    set(package_id "org.luna.fixture.${package_component}")
    set(package_dir "${fixture_root}/${package_component}")
    set(source_dir "${package_dir}/src")
    file(REMOVE_RECURSE "${package_dir}")
    file(MAKE_DIRECTORY "${source_dir}")
    file(COPY "${source_path}" DESTINATION "${source_dir}")
    file(WRITE "${package_dir}/luna.package"
        "[package]\n"
        "id = \"${package_id}\"\n"
        "version = \"1.0.0\"\n"
        "kind = \"application\"\n"
        "sources = [\"src\"]\n")

    set(executable_path
        "${package_dir}/build/native/${package_component}")
    if(WIN32)
        string(APPEND executable_path ".exe")
    endif()
    set(LUNA_AOT_PACKAGE_DIR "${package_dir}" PARENT_SCOPE)
    set(LUNA_AOT_IR_PATH "${executable_path}.ll" PARENT_SCOPE)
    set(LUNA_AOT_EXECUTABLE_PATH "${executable_path}" PARENT_SCOPE)
endfunction()
