if(NOT DEFINED LUNA_EXECUTABLE OR NOT DEFINED LUNA_SOURCE_DIR OR
   NOT DEFINED LUNA_BINARY_DIR)
    message(FATAL_ERROR "Moon Container CLI test is missing its paths")
endif()

set(package_dir
    "${LUNA_SOURCE_DIR}/tests/fixtures/packages/package_kind_application")
set(work_dir "${LUNA_BINARY_DIR}/moon-container-cli-test")
file(REMOVE_RECURSE "${work_dir}")
file(MAKE_DIRECTORY "${work_dir}")
set(first "${work_dir}/first.moon")
set(second "${work_dir}/second.moon")

foreach(output IN ITEMS "${first}" "${second}")
    execute_process(
        COMMAND "${LUNA_EXECUTABLE}" build "${package_dir}"
                -t moon -o "${output}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr)
    if(NOT result EQUAL 0 OR NOT EXISTS "${output}")
        message(FATAL_ERROR
            "-t moon failed (${result})\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
endforeach()

file(SHA256 "${first}" first_digest)
file(SHA256 "${second}" second_digest)
if(NOT first_digest STREQUAL second_digest)
    message(FATAL_ERROR "-t moon output is not deterministic")
endif()
file(READ "${first}" magic_hex OFFSET 0 LIMIT 8 HEX)
if(NOT magic_hex STREQUAL "894d4f4f4e0d0a1a")
    message(FATAL_ERROR "-t moon output has the wrong container magic")
endif()

set(invalid_output "${work_dir}/standalone.moon")
execute_process(
    COMMAND "${LUNA_EXECUTABLE}" build
            "${package_dir}/src/main.luna" -t moon -o "${invalid_output}"
    RESULT_VARIABLE invalid_result
    OUTPUT_VARIABLE invalid_stdout
    ERROR_VARIABLE invalid_stderr)
if(invalid_result EQUAL 0 OR EXISTS "${invalid_output}" OR
   NOT invalid_stderr MATCHES "requires a package directory")
    message(FATAL_ERROR
        "-t moon accepted standalone source\nstdout:\n${invalid_stdout}\nstderr:\n${invalid_stderr}")
endif()
