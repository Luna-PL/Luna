cmake_minimum_required(VERSION 3.20)
project(Luna VERSION 0.3.0 LANGUAGES C CXX)

include(GNUInstallDirs)

if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(LLVM REQUIRED CONFIG)
if(NOT TARGET LLVM)
    message(FATAL_ERROR
        "This LLVM installation does not export the monolithic LLVM target. "
        "Set LLVM_DIR to the matching LLVM CMake package or install LLVM development files.")
endif()

# Luna emits textual LLVM IR for AOT builds, so the native link driver must
# understand LLVM IR even when Luna itself is compiled with GCC. Keep this
# tool separate from CMAKE_CXX_COMPILER and make it overridable for packaged
# or cross-toolchain test environments.
if(NOT DEFINED LUNA_AOT_COMPILER)
    find_program(LUNA_AOT_COMPILER
        NAMES clang++-${LLVM_VERSION_MAJOR} clang++
        HINTS ${LLVM_TOOLS_BINARY_DIR}
    )
endif()
set(LUNA_AOT_COMPILER "${LUNA_AOT_COMPILER}" CACHE FILEPATH
    "Clang-compatible driver used by Luna AOT integration tests")
if(NOT LUNA_AOT_COMPILER)
    message(FATAL_ERROR
        "Luna AOT tests require clang++. Set LUNA_AOT_COMPILER to a "
        "Clang-compatible driver for the configured LLVM installation.")
endif()
include(CTest)

find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --verify HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE LUNA_GIT_RESULT
        OUTPUT_VARIABLE LUNA_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    # A normal commit updates refs/heads/<branch>, not .git/HEAD. Register both
    # resolved Git paths so `cmake --build` refreshes the embedded identity
    # after either a branch commit or a detached-HEAD checkout.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" symbolic-ref --quiet HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE LUNA_GIT_REF_RESULT
        OUTPUT_VARIABLE LUNA_GIT_REF
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    set(LUNA_GIT_WATCH_PATHS HEAD)
    if(LUNA_GIT_REF_RESULT EQUAL 0 AND NOT LUNA_GIT_REF STREQUAL "")
        list(APPEND LUNA_GIT_WATCH_PATHS "${LUNA_GIT_REF}")
    endif()
    foreach(LUNA_GIT_WATCH_PATH IN LISTS LUNA_GIT_WATCH_PATHS)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --git-path
                    "${LUNA_GIT_WATCH_PATH}"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            RESULT_VARIABLE LUNA_GIT_PATH_RESULT
            OUTPUT_VARIABLE LUNA_GIT_PATH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(LUNA_GIT_PATH_RESULT EQUAL 0 AND NOT LUNA_GIT_PATH STREQUAL "")
            get_filename_component(LUNA_GIT_PATH "${LUNA_GIT_PATH}"
                ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
            if(EXISTS "${LUNA_GIT_PATH}")
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                    "${LUNA_GIT_PATH}")
            endif()
        endif()
    endforeach()
else()
    set(LUNA_GIT_RESULT 1)
endif()
if(NOT LUNA_GIT_RESULT EQUAL 0 OR LUNA_GIT_COMMIT STREQUAL "")
    set(LUNA_GIT_COMMIT "unknown")
endif()

option(LUNA_ENABLE_ROCM_SMOKE "Run ROCm JIT/AOT smoke tests on a configured AMD GPU" OFF)
option(LUNA_ENABLE_ROCM_BENCHMARK "Run the large Luna versus C++23/HIP ROCm comparison" OFF)
option(LUNA_ENABLE_CPU_BENCHMARK "Run common Luna versus C++23 CPU benchmarks" OFF)
option(LUNA_ENABLE_BASIC_BENCHMARK "Run the lightweight Luna CPU baseline" OFF)
option(LUNA_STRICT_WARNINGS "Treat compiler warnings in Luna-owned targets as errors" OFF)
option(LUNA_ENABLE_SANITIZERS
    "Instrument the compiler and embedded JIT runtime with ASan/UBSan" OFF)
option(LUNA_ENABLE_THREAD_SANITIZER
    "Instrument the concurrent MoonRuntime state-machine test with TSan" OFF)
option(LUNA_ENABLE_FUZZING
    "Build the Clang libFuzzer Moon Container trust-boundary harness" OFF)
set(LUNA_ROCM_SMOKE_ARCH "gfx1101" CACHE STRING
    "AMDGPU architecture used by the opt-in ROCm smoke test")

add_library(runtime STATIC
    src/runtime/ApplicationHostServices.cpp
    src/runtime/MoonRuntime.cpp
    src/runtime/Runtime.cpp)

add_library(runtime-abi-c-compile OBJECT tests/runtime_abi_c_compile.c)
target_include_directories(runtime-abi-c-compile PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(runtime-abi-test tests/runtime_abi_test.cpp)
target_include_directories(runtime-abi-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(runtime-abi-test PRIVATE runtime)

add_executable(runtime-allocation-abi-test
    tests/runtime_allocation_abi_test.cpp)
target_include_directories(runtime-allocation-abi-test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(runtime-allocation-abi-test PRIVATE runtime)

add_executable(runtime-default-allocation-test
    tests/runtime_default_allocation_test.cpp)
target_include_directories(runtime-default-allocation-test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(runtime-default-allocation-test PRIVATE runtime)

add_executable(runtime-filesystem-abi-test
    tests/runtime_filesystem_abi_test.cpp)
target_include_directories(runtime-filesystem-abi-test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(runtime-filesystem-abi-test PRIVATE runtime)

add_executable(runtime-application-host-test
    tests/runtime_application_host_test.cpp)
target_include_directories(runtime-application-host-test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(runtime-application-host-test PRIVATE runtime)

find_package(Threads REQUIRED)
add_executable(moon-runtime-test
    tests/moon_runtime_test.cpp
    src/runtime/MoonRuntime.cpp)
target_include_directories(moon-runtime-test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(moon-runtime-test PRIVATE Threads::Threads)

add_executable(runtime-gpu-error-test tests/runtime_gpu_error_test.cpp)
target_include_directories(runtime-gpu-error-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(runtime-gpu-error-test PRIVATE runtime)

add_library(luna-fragment-plugin-fixture SHARED tests/fragment_plugin_fixture.cpp)
target_include_directories(luna-fragment-plugin-fixture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(fragment-plugin-test tests/fragment_plugin_test.cpp)
target_include_directories(fragment-plugin-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(fragment-plugin-test PRIVATE runtime)

add_executable(source-manager-test
    tests/source_manager_test.cpp
    src/tooling/SourceManager.cpp
)
target_include_directories(source-manager-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(core-contracts-test tests/core_contracts_test.cpp)
target_include_directories(core-contracts-test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(symbol-catalog-test
    tests/symbol_catalog_test.cpp
    src/core/TypeRelations.cpp
    src/selector/Selector.cpp)
target_include_directories(symbol-catalog-test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(native-artifact-test
    tests/native_artifact_test.cpp
    src/driver/NativeArtifact.cpp
    src/driver/NativeGeneration.cpp
    src/runtime/MoonRuntime.cpp)
target_include_directories(native-artifact-test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(native-artifact-test SYSTEM PRIVATE
    ${LLVM_INCLUDE_DIRS})
target_link_libraries(native-artifact-test PRIVATE LLVM Threads::Threads)
if(CMAKE_DL_LIBS AND NOT WIN32)
    target_link_libraries(native-artifact-test PRIVATE ${CMAKE_DL_LIBS})
endif()

add_executable(moon-container-test
    tests/moon_container_test.cpp
    src/moonir/Container.cpp
)
target_include_directories(moon-container-test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(moon-container-test SYSTEM PRIVATE
    ${LLVM_INCLUDE_DIRS})
target_link_libraries(moon-container-test PRIVATE LLVM)

add_library(luna-frontend OBJECT
    src/core/TypeRelations.cpp
    src/core/TypeLayout.cpp
    src/lexer/Lexer.cpp
    src/parser/Parser.cpp
    src/sema/TypeSystem.cpp
    src/sema/SymbolTable.cpp
    src/sema/SemanticAnalyzer.cpp
    src/sema/BodyAnalyzer.cpp
    src/sema/CompileTimeEvaluator.cpp
    src/sema/ControlAnalyzer.cpp
    src/sema/DeclarationCollector.cpp
    src/sema/SemanticContext.cpp
    src/sema/SemanticContextAccess.cpp
    src/sema/TypeResolver.cpp
    src/sema/TraitChecker.cpp
    src/sema/OwnershipChecker.cpp
    src/selector/Selector.cpp
    src/instantiation/Instantiator.cpp
    src/macro/MacroProcessor.cpp
    src/package/Package.cpp
    src/package/PackageManager.cpp
    src/tooling/AnalysisSnapshot.cpp
    src/tooling/ReferenceIndex.cpp
    src/tooling/SymbolIndex.cpp
)
target_include_directories(luna-frontend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(analysis-snapshot-test
    tests/analysis_snapshot_test.cpp
    $<TARGET_OBJECTS:luna-frontend>
)
target_include_directories(analysis-snapshot-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(moon-container-model-test
    tests/moon_container_model_test.cpp
    src/moonir/Container.cpp
    src/moonir/ContainerModel.cpp
    src/moonir/MoonIR.cpp
    src/moonir/Verifier.cpp
    $<TARGET_OBJECTS:luna-frontend>
)
target_include_directories(moon-container-model-test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(moon-container-model-test SYSTEM PRIVATE
    ${LLVM_INCLUDE_DIRS})
target_link_libraries(moon-container-model-test PRIVATE LLVM)

if(LUNA_ENABLE_FUZZING)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "LUNA_ENABLE_FUZZING requires Clang/libFuzzer")
    endif()
    add_executable(moon-container-fuzz
        tests/moon_container_fuzz.cpp
        src/moonir/Container.cpp
        src/moonir/ContainerModel.cpp
        src/moonir/MoonIR.cpp
        src/moonir/Verifier.cpp
        $<TARGET_OBJECTS:luna-frontend>
    )
    target_include_directories(moon-container-fuzz
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_include_directories(moon-container-fuzz SYSTEM PRIVATE
        ${LLVM_INCLUDE_DIRS})
    target_compile_options(moon-container-fuzz PRIVATE
        -fsanitize=fuzzer,address,undefined
        -fno-omit-frame-pointer)
    target_link_options(moon-container-fuzz PRIVATE
        -fsanitize=fuzzer,address,undefined
        -fno-omit-frame-pointer)
    target_link_libraries(moon-container-fuzz PRIVATE LLVM)
endif()

add_executable(moonir-canonical-test
    tests/moonir_canonical_test.cpp
    src/driver/MoonGeneration.cpp
    src/moonir/Container.cpp
    src/moonir/ContainerModel.cpp
    src/moonir/ControlFlowBuilder.cpp
    src/moonir/Lowering.cpp
    src/moonir/MoonIR.cpp
    src/moonir/Sealer.cpp
    src/moonir/Verifier.cpp
    src/codegen/CGHelpers.cpp
    src/codegen/CodeGenerator.cpp
    src/codegen/CodeGeneratorCleanup.cpp
    src/codegen/CodeGeneratorControlFlow.cpp
    src/codegen/CodeGeneratorExecution.cpp
    src/codegen/CodeGeneratorExpressions.cpp
    src/codegen/CodeGeneratorFunctions.cpp
    src/codegen/CodeGeneratorGpu.cpp
    src/codegen/CodeGeneratorIterator.cpp
    src/codegen/CodeGeneratorModule.cpp
    src/codegen/CodeGeneratorRangeAnalysis.cpp
    src/codegen/CodeGeneratorRuntimeDescriptors.cpp
    src/runtime/ApplicationHostServices.cpp
    src/runtime/MoonRuntime.cpp
    src/runtime/Runtime.cpp
    src/driver/CompilerPipeline.cpp
    src/moonir/Optimizer.cpp
    $<TARGET_OBJECTS:luna-frontend>
)
target_include_directories(moonir-canonical-test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(moonir-canonical-test SYSTEM PRIVATE
    ${LLVM_INCLUDE_DIRS})
target_link_libraries(moonir-canonical-test PRIVATE LLVM)
if(CMAKE_DL_LIBS AND NOT WIN32)
    target_link_libraries(moonir-canonical-test PRIVATE ${CMAKE_DL_LIBS})
endif()

add_executable(luna
    src/main.cpp
    src/driver/AotLinker.cpp
    src/driver/CommandLine.cpp
    src/driver/CompilerPipeline.cpp
    src/driver/Driver.cpp
    src/driver/MoonGeneration.cpp
    src/driver/NativeArtifact.cpp
    src/driver/NativeGeneration.cpp
    src/driver/Repl.cpp
    $<TARGET_OBJECTS:luna-frontend>
    src/moonir/ControlFlowBuilder.cpp
    src/moonir/Container.cpp
    src/moonir/ContainerModel.cpp
    src/moonir/MoonIR.cpp
    src/moonir/Lowering.cpp
    src/moonir/Verifier.cpp
    src/moonir/Optimizer.cpp
    src/moonir/Printer.cpp
    src/moonir/Sealer.cpp
    src/codegen/CGHelpers.cpp
    src/codegen/CodeGenerator.cpp
    src/codegen/CodeGeneratorCleanup.cpp
    src/codegen/CodeGeneratorControlFlow.cpp
    src/codegen/CodeGeneratorExecution.cpp
    src/codegen/CodeGeneratorExpressions.cpp
    src/codegen/CodeGeneratorFunctions.cpp
    src/codegen/CodeGeneratorGpu.cpp
    src/codegen/CodeGeneratorIterator.cpp
    src/codegen/CodeGeneratorModule.cpp
    src/codegen/CodeGeneratorRangeAnalysis.cpp
    src/codegen/CodeGeneratorRuntimeDescriptors.cpp
    src/runtime/ApplicationHostServices.cpp
    src/runtime/MoonRuntime.cpp
    src/runtime/Runtime.cpp
)

if(LUNA_STRICT_WARNINGS)
    set(luna_warning_targets
        runtime
        runtime-abi-c-compile
        runtime-abi-test
        runtime-allocation-abi-test
        runtime-default-allocation-test
        runtime-filesystem-abi-test
        runtime-application-host-test
        moon-runtime-test
        runtime-gpu-error-test
        luna-fragment-plugin-fixture
        fragment-plugin-test
        source-manager-test
        core-contracts-test
        symbol-catalog-test
        native-artifact-test
        moon-container-test
        moon-container-model-test
        luna-frontend
        analysis-snapshot-test
        moonir-canonical-test
        luna
    )
    if(LUNA_ENABLE_FUZZING)
        list(APPEND luna_warning_targets moon-container-fuzz)
    endif()
    foreach(target IN LISTS luna_warning_targets)
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4 /WX)
        else()
            target_compile_options(${target} PRIVATE
                -Wall -Wextra -Wpedantic -Werror)
        endif()
    endforeach()
endif()

if(LUNA_ENABLE_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR
            "LUNA_ENABLE_SANITIZERS requires Clang or GCC")
    endif()
    # `luna` embeds Runtime.cpp for JIT symbol resolution. Instrument the
    # shared frontend objects and both consumers without contaminating the
    # installed AOT runtime archive with sanitizer symbols.
    foreach(target IN ITEMS
            luna luna-frontend analysis-snapshot-test moonir-canonical-test
            moon-container-test moon-container-model-test moon-runtime-test
            native-artifact-test)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer)
    endforeach()
    target_link_options(luna PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer)
    target_link_options(analysis-snapshot-test PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer)
    target_link_options(moonir-canonical-test PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer)
    target_link_options(moon-container-test PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer)
    target_link_options(moon-container-model-test PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer)
    target_link_options(moon-runtime-test PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer)
    target_link_options(native-artifact-test PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer)
endif()

if(LUNA_ENABLE_THREAD_SANITIZER)
    if(LUNA_ENABLE_SANITIZERS)
        message(FATAL_ERROR
            "LUNA_ENABLE_THREAD_SANITIZER cannot be combined with ASan/UBSan")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR
            "LUNA_ENABLE_THREAD_SANITIZER requires Clang or GCC")
    endif()
    target_compile_options(moon-runtime-test PRIVATE
        -fsanitize=thread
        -fno-omit-frame-pointer)
    target_link_options(moon-runtime-test PRIVATE
        -fsanitize=thread
        -fno-omit-frame-pointer)
endif()

target_include_directories(luna SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_include_directories(luna PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(luna PRIVATE
    __STDC_CONSTANT_MACROS
    __STDC_FORMAT_MACROS
    __STDC_LIMIT_MACROS
    LUNA_DL_LIBRARY="${CMAKE_DL_LIBS}"
    LUNA_COMPILER_COMMIT="${LUNA_GIT_COMMIT}"
    BUILD_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)

if(UNIX AND NOT APPLE AND NOT WIN32)
    target_compile_definitions(luna PRIVATE _GNU_SOURCE)
endif()

# LLVM exports a version-independent imported target.  Avoid -lLLVM-22 and
# platform-specific linker flags so the same project works with other LLVM
# versions and on macOS.
target_link_libraries(luna PRIVATE LLVM)
if(CMAKE_DL_LIBS AND NOT WIN32)
    target_link_libraries(runtime PUBLIC ${CMAKE_DL_LIBS})
    target_link_libraries(luna PRIVATE ${CMAKE_DL_LIBS})
endif()
# JIT-visible Luna runtime symbols are registered explicitly with ORC. Do not
# export every compiler symbol merely to make in-process JIT work: that policy
# is unreliable on Windows and unnecessarily enlarges the host symbol surface.

install(TARGETS luna runtime
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES src/runtime/FragmentPluginABI.h src/runtime/NativeArtifactABI.h
              src/runtime/RuntimeABI.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/luna/runtime)
install(FILES README.md README.zh-CN.md CHANGELOG.md VERSION LICENSE-MIT LICENSE-APACHE
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs)
install(DIRECTORY examples/full_showcase/
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/examples/full_showcase)
install(DIRECTORY stdlib/ DESTINATION ${CMAKE_INSTALL_DATADIR}/luna/stdlib)

if(BUILD_TESTING)
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    add_test(
        NAME luna.analysis-snapshot
        COMMAND $<TARGET_FILE:analysis-snapshot-test>
            ${CMAKE_CURRENT_SOURCE_DIR}
    )
    add_test(
        NAME luna.moonir-canonical
        COMMAND $<TARGET_FILE:moonir-canonical-test>
    )
    add_test(
        NAME luna.moon-container
        COMMAND $<TARGET_FILE:moon-container-test>
    )
    add_test(
        NAME luna.moon-runtime
        COMMAND $<TARGET_FILE:moon-runtime-test>
    )
    add_test(
        NAME luna.moon-container-model
        COMMAND $<TARGET_FILE:moon-container-model-test>
    )
    add_test(
        NAME luna.moon-container-cli
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DLUNA_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/moon_container_cli.cmake
    )
    add_test(
        NAME luna.moon-container-oracle
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/moon_container_oracle.py
            --compiler $<TARGET_FILE:luna>
            --package
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/packages/package_kind_application
            --work-dir
                ${CMAKE_CURRENT_BINARY_DIR}/moon-container-oracle-test
    )
    add_test(
        NAME luna.cffi-artifact
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DLUNA_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DLUNA_C_COMPILER=${CMAKE_C_COMPILER}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cffi_artifact.cmake
    )
    add_test(
        NAME luna.native-artifact
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_NATIVE_VERIFIER=$<TARGET_FILE:native-artifact-test>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DLUNA_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DPython3_EXECUTABLE=${Python3_EXECUTABLE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_artifact.cmake
    )
    if(LUNA_ENABLE_FUZZING)
        set(LUNA_MOON_FUZZ_CORPUS
            "${CMAKE_CURRENT_BINARY_DIR}/moon-container-fuzz-corpus")
        add_custom_target(moon-container-fuzz-corpus
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/moon_container_fuzz_corpus.py
                --compiler $<TARGET_FILE:luna>
                --package
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/packages/package_kind_application
                --output-dir ${LUNA_MOON_FUZZ_CORPUS}
            DEPENDS luna
            VERBATIM)
        add_test(
            NAME luna.moon-container-fuzz-corpus
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/moon_container_fuzz_corpus.py
                --compiler $<TARGET_FILE:luna>
                --package
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/packages/package_kind_application
                --output-dir ${LUNA_MOON_FUZZ_CORPUS})
        set_tests_properties(luna.moon-container-fuzz-corpus PROPERTIES
            FIXTURES_SETUP moon-container-fuzz-seeds
            LABELS fuzz)
        add_test(
            NAME luna.moon-container-fuzz-smoke
            COMMAND $<TARGET_FILE:moon-container-fuzz>
                ${LUNA_MOON_FUZZ_CORPUS}
                -runs=2000
                -max_len=1048576
                -timeout=5
                -dict=${CMAKE_CURRENT_SOURCE_DIR}/tests/moon_container_fuzz.dict
                -artifact_prefix=${LUNA_MOON_FUZZ_CORPUS}/artifacts/)
        set_tests_properties(luna.moon-container-fuzz-smoke PROPERTIES
            FIXTURES_REQUIRED moon-container-fuzz-seeds
            LABELS fuzz
            RUN_SERIAL TRUE
            TIMEOUT 120
            ENVIRONMENT
                "ASAN_OPTIONS=detect_leaks=0:alloc_dealloc_mismatch=0;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
    endif()
    add_test(
        NAME luna.source-manager
        COMMAND $<TARGET_FILE:source-manager-test>
    )
    add_test(
        NAME luna.core-contracts
        COMMAND $<TARGET_FILE:core-contracts-test>
    )
    add_test(
        NAME luna.symbol-catalog
        COMMAND $<TARGET_FILE:symbol-catalog-test>
    )
    add_test(
        NAME luna.runtime-abi-v1
        COMMAND $<TARGET_FILE:runtime-abi-test>
    )
    add_test(
        NAME luna.runtime-allocation-abi-v1
        COMMAND $<TARGET_FILE:runtime-allocation-abi-test>
    )
    add_test(
        NAME luna.runtime-default-allocation
        COMMAND $<TARGET_FILE:runtime-default-allocation-test>
    )
    add_test(
        NAME luna.runtime-filesystem-abi-v1
        COMMAND $<TARGET_FILE:runtime-filesystem-abi-test>
    )
    add_test(
        NAME luna.runtime-application-host-v1
        COMMAND $<TARGET_FILE:runtime-application-host-test>
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/result_basic.luna
    )
    add_test(
        NAME luna.std-io-smoke
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/std_io_smoke.cmake
    )
    add_test(
        NAME luna.semantic-regressions
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/semantic_regressions.cmake
    )
    add_test(
        NAME luna.diagnostic-protocol-v1
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/diagnostic_protocol.cmake
    )
    add_test(
        NAME luna.compiler-identity
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_identity.cmake
    )
    set_tests_properties(luna.compiler-identity PROPERTIES LABELS "release")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Lunax/.git" AND
       EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/toolchain/.git")
        add_test(
            NAME luna.ecosystem-0.2-lock
            COMMAND ${CMAKE_COMMAND}
                -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_ecosystem_lock.cmake
        )
        set_tests_properties(luna.ecosystem-0.2-lock PROPERTIES
            LABELS "integration")
    endif()
    add_test(
        NAME luna.analysis-protocol-v1
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DLUNA_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/analysis_protocol.cmake
    )
    add_test(
        NAME luna.repl-smoke
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/repl_smoke.cmake
    )
    add_test(
        NAME luna.file-guide-inventory
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/file_guide_inventory.cmake
    )
    set_tests_properties(luna.file-guide-inventory PROPERTIES RUN_SERIAL TRUE)
    add_test(
        NAME luna.0.3-design-contract
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/luna_0_3_design_contract.cmake
    )
    add_test(
        NAME luna.migration-0.2-manifest
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/migration_0_2_baseline.cmake
    )
    add_test(
        NAME luna.shadow-identity
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DLUNA_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/shadow_identity.cmake
    )
    add_test(
        NAME luna.package-export-abi
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DLUNA_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/package_export_abi.cmake
    )
    add_test(
        NAME luna.package-module-model
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DLUNA_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/package_module_model.cmake
    )
    add_test(
        NAME luna.package-manifest-workspace
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DLUNA_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/package_manifest_workspace.cmake
    )
    add_test(
        NAME luna.core-surface
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DLUNA_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/core_surface.cmake
    )
    add_test(
        NAME luna.rc-arc-core
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DLUNA_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/rc_arc_core.cmake
    )
    add_test(
        NAME luna.full-showcase
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DLUNA_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/full_showcase.cmake
    )
    add_test(
        NAME luna.return-cleanup-abi
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/return_cleanup_abi.cmake
    )
    add_test(
        NAME luna.resource-drop-aot
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DLUNA_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/resource_drop_aot.cmake
    )
    add_test(
        NAME luna.result-error-aot
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/result_error_aot.cmake
    )
    add_test(
        NAME luna.result-extended-aot
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/result_extended_aot.cmake
    )
    add_test(
        NAME luna.control-flow-aot
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/control_flow_aot.cmake
    )
    add_test(
        NAME luna.ffi-aot
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi_aot.cmake
    )
    add_test(
        NAME luna.jit-aot-parity
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/jit_aot_parity.cmake
    )
    add_test(
        NAME luna.iterator-pipeline-aot
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/iterator_pipeline_aot.cmake
    )
    add_test(
        NAME luna.iterator-move-only-aot
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/iterator_move_only_aot.cmake
    )
    add_test(
        NAME luna.iterator-materialized-aot
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/iterator_materialized_aot.cmake
    )
    add_test(
        NAME luna.iterator-materialized-move-only-aot
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/iterator_materialized_move_only_aot.cmake
    )
    add_test(
        NAME luna.optimization-pipeline
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/optimization_pipeline.cmake
    )
    add_test(
        NAME luna.fragment-lowering-abi
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/fragment_lowering_abi.cmake
    )
    add_test(
        NAME luna.structured-cps-abi
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/structured_cps_abi.cmake
    )
    add_test(
        NAME luna.aot-runtime-boundary
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DLUNA_RUNTIME_LIBRARY=$<TARGET_FILE:runtime>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/aot_runtime_boundary.cmake
    )
    add_test(
        NAME luna.install-smoke
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DLUNA_BUILD_CONFIG=$<CONFIG>
            -DLUNA_VERSION=${PROJECT_VERSION}
            -DLUNA_AOT_COMPILER=${LUNA_AOT_COMPILER}
            -DLUNA_EXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}
            -DLUNA_RUNTIME_FILE=$<TARGET_FILE_NAME:runtime>
            -DLUNA_INSTALL_BINDIR=${CMAKE_INSTALL_BINDIR}
            -DLUNA_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}
            -DLUNA_INSTALL_INCLUDEDIR=${CMAKE_INSTALL_INCLUDEDIR}
            -DLUNA_INSTALL_DATADIR=${CMAKE_INSTALL_DATADIR}
            -DLUNA_INSTALL_DOCDIR=${CMAKE_INSTALL_DOCDIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/install_smoke.cmake
    )
    set_tests_properties(luna.install-smoke PROPERTIES
        LABELS "release;install"
    )
    add_test(
        NAME luna.runtime-gpu-error-state
        COMMAND $<TARGET_FILE:runtime-gpu-error-test>
    )
    add_test(
        NAME luna.external-fragment-plugin-abi
        COMMAND $<TARGET_FILE:fragment-plugin-test>
                $<TARGET_FILE:luna-fragment-plugin-fixture>
    )
    add_test(
        NAME luna.external-fragment-dispatch
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DLUNA_PLUGIN=$<TARGET_FILE:luna-fragment-plugin-fixture>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/external_fragment_dispatch.cmake
    )
    add_test(
        NAME luna.gpu-error-boundary-abi
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DLUNA_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu_error_boundary_abi.cmake
    )
    add_test(
        NAME luna.rocm-isa-abi
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/rocm_isa_abi.cmake
    )
    add_test(
        NAME luna.jit-aot-extended-parity
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DLUNA_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/jit_aot_extended_parity.cmake
    )
    add_test(
        NAME luna.jit-runtime-symbols
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/jit_runtime_symbols.cmake
    )
    add_test(
        NAME luna.gpu-target-split
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DLUNA_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu_target_split.cmake
    )
    add_test(
        NAME luna.stable-core-parity
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DLUNA_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/stable_core_parity.cmake
    )
    add_test(
        NAME luna.moon-cost-boundaries
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DLUNA_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/moon_cost_boundaries.cmake
    )
    if(LUNA_ENABLE_ROCM_SMOKE)
        add_test(
            NAME luna.rocm-smoke
            COMMAND ${CMAKE_COMMAND}
                -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
                -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -DLUNA_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
                -DLUNA_ROCM_ARCH=${LUNA_ROCM_SMOKE_ARCH}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/rocm_smoke.cmake
        )
        set_tests_properties(luna.rocm-smoke PROPERTIES LABELS "rocm;hardware")
    endif()
    if(LUNA_ENABLE_ROCM_BENCHMARK)
        add_test(
            NAME luna.rocm-cpp23-comparison
            COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/run_rocm_cpp23_comparison.sh
                $<TARGET_FILE:luna> ${CMAKE_CURRENT_SOURCE_DIR}
        )
        set_tests_properties(luna.rocm-cpp23-comparison PROPERTIES
            LABELS "rocm;hardware;benchmark"
            TIMEOUT 180
        )
    endif()
    if(LUNA_ENABLE_CPU_BENCHMARK)
        add_test(
            NAME luna.cpu-comparison
            COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/run_cpu_comparison.sh
                $<TARGET_FILE:luna> ${CMAKE_CURRENT_SOURCE_DIR}
        )
        set_tests_properties(luna.cpu-comparison PROPERTIES
            LABELS "benchmark;cpu"
            TIMEOUT 180
        )
    endif()
    if(LUNA_ENABLE_BASIC_BENCHMARK)
        add_test(
            NAME luna.basic-benchmark
            COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/run_basic_benchmark.sh
                $<TARGET_FILE:luna> ${CMAKE_CURRENT_SOURCE_DIR}
        )
        set_tests_properties(luna.basic-benchmark PROPERTIES
            LABELS "benchmark;cpu"
            TIMEOUT 120
        )
    endif()
endif()

if(LLVM_ENABLE_RTTI)
    message(STATUS "LLVM compiled with RTTI")
else()
    message(WARNING "LLVM compiled without RTTI — avoid dynamic_cast across LLVM types")
endif()
