cmake_minimum_required(VERSION 3.20)
project(Luna VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(LLVM REQUIRED CONFIG)
if(NOT TARGET LLVM)
    message(FATAL_ERROR
        "This LLVM installation does not export the monolithic LLVM target. "
        "Set LLVM_DIR to the matching LLVM CMake package or install LLVM development files.")
endif()
include(CTest)

option(LUNA_ENABLE_ROCM_SMOKE "Run ROCm JIT/AOT smoke tests on a configured AMD GPU" OFF)
option(LUNA_ENABLE_ROCM_BENCHMARK "Run the large Luna versus C++23/HIP ROCm comparison" OFF)
option(LUNA_ENABLE_CPU_BENCHMARK "Run common Luna versus C++23 CPU benchmarks" OFF)
option(LUNA_ENABLE_BASIC_BENCHMARK "Run the lightweight Luna CPU baseline" OFF)

add_library(runtime STATIC src/runtime/Runtime.cpp)

add_executable(runtime-gpu-error-test tests/runtime_gpu_error_test.cpp)
target_include_directories(runtime-gpu-error-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(runtime-gpu-error-test PRIVATE runtime)

add_library(luna-fragment-plugin-fixture SHARED tests/fragment_plugin_fixture.cpp)
target_include_directories(luna-fragment-plugin-fixture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(fragment-plugin-test tests/fragment_plugin_test.cpp)
target_include_directories(fragment-plugin-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(fragment-plugin-test PRIVATE runtime)

add_executable(luna
    src/main.cpp
    src/lexer/Lexer.cpp
    src/parser/Parser.cpp
    src/sema/TypeSystem.cpp
    src/sema/SymbolTable.cpp
    src/sema/SemanticAnalyzer.cpp
    src/sema/TraitChecker.cpp
    src/sema/OwnershipChecker.cpp
    src/codegen/CGHelpers.cpp
    src/codegen/CodeGenerator.cpp
    src/runtime/Runtime.cpp
    src/package/Package.cpp
)

target_include_directories(luna PRIVATE ${LLVM_INCLUDE_DIRS})
target_include_directories(luna PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(luna PRIVATE
    __STDC_CONSTANT_MACROS
    __STDC_FORMAT_MACROS
    __STDC_LIMIT_MACROS
    LUNA_DL_LIBRARY="${CMAKE_DL_LIBS}"
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
set_target_properties(luna PROPERTIES ENABLE_EXPORTS TRUE)

# Keep a build-tree compatibility name for existing Alpha scripts.  The
# installed and documented driver is `luna`; the old `llvm-demo` name is not
# part of the new project identity.
if(WIN32)
    add_custom_command(TARGET luna POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:luna>" "$<TARGET_FILE_DIR:luna>/llvm-demo$<TARGET_FILE_SUFFIX:luna>"
        VERBATIM)
else()
    add_custom_command(TARGET luna POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -f "$<TARGET_FILE_DIR:luna>/llvm-demo"
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            "$<TARGET_FILE:luna>" "$<TARGET_FILE_DIR:luna>/llvm-demo"
        VERBATIM)
endif()

install(TARGETS luna runtime
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES src/runtime/FragmentPluginABI.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/luna/runtime)
install(FILES README.md CHANGELOG.md VERSION LICENSE-MIT LICENSE-APACHE
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DOCDIR})

if(BUILD_TESTING)
    add_test(
        NAME luna.semantic-regressions
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/semantic_regressions.cmake
    )
    add_test(
        NAME luna.package-export-abi
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/package_export_abi.cmake
    )
    add_test(
        NAME luna.return-cleanup-abi
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/return_cleanup_abi.cmake
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
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/jit_aot_extended_parity.cmake
    )
    add_test(
        NAME luna.stable-core-parity
        COMMAND ${CMAKE_COMMAND}
            -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
            -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/stable_core_parity.cmake
    )
    if(LUNA_ENABLE_ROCM_SMOKE)
        add_test(
            NAME luna.rocm-smoke
            COMMAND ${CMAKE_COMMAND}
                -DLUNA_EXECUTABLE=$<TARGET_FILE:luna>
                -DLUNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
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
