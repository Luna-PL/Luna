#include "driver/AotLinker.h"

#include "diagnostics/Diagnostic.h"

#include <llvm/Support/Program.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifndef LUNA_DL_LIBRARY
#define LUNA_DL_LIBRARY ""
#endif

namespace luna::driver {
namespace {

void printErrors(const std::vector<diagnostic::Diagnostic>& errors) {
    for (const auto& error : errors) std::cerr << error << "\n";
}

std::string quoteForDisplay(const std::string& value) {
    if (value.find_first_of(" \t\"'") == std::string::npos) return value;
    std::string quoted = "\"";
    for (char c : value) {
        if (c == '"' || c == '\\') quoted += '\\';
        quoted += c;
    }
    quoted += '"';
    return quoted;
}

bool isLibraryPath(const std::string& value) {
    auto endsWith = [&value](const char* suffix) {
        const std::string suffixString(suffix);
        return value.size() >= suffixString.size() &&
               value.compare(
                   value.size() - suffixString.size(),
                   suffixString.size(),
                   suffixString) == 0;
    };
    return value.find('/') != std::string::npos ||
           value.find('\\') != std::string::npos ||
           endsWith(".a") || endsWith(".so") || endsWith(".dylib") ||
           endsWith(".dll") || endsWith(".lib");
}

} // namespace

int AotLinker::build(CodeGenerator& codeGenerator, AotLinkOptions options) {
    namespace fs = std::filesystem;

    const fs::path inputPath(options.inputPath);
    fs::path irPath;
    fs::path artifactPath;
    if (fs::is_directory(inputPath)) {
        const std::string packageName = options.declaredPackageName.empty()
            ? inputPath.filename().string()
            : options.declaredPackageName;
        irPath = inputPath / (packageName + ".ll");
        artifactPath = inputPath / packageName;
    } else {
        irPath = inputPath.string() + ".ll";
        artifactPath = inputPath.parent_path() / inputPath.stem();
    }
    if (!options.outputPath.empty()) {
        artifactPath = fs::path(options.outputPath);
        irPath = artifactPath;
        irPath += ".ll";
    }
#ifdef _WIN32
    if (options.outputPath.empty() &&
        options.artifactKind == AotArtifactKind::Executable)
        artifactPath += ".exe";
#endif

    std::error_code filesystemError;
    if (!artifactPath.parent_path().empty())
        fs::create_directories(artifactPath.parent_path(), filesystemError);
    if (filesystemError) {
        std::cerr << diagnostic::format(
            "driver",
            "cannot create AOT output directory: " +
                filesystemError.message(),
            artifactPath.parent_path().string(), 0, 0,
            "check the output path permissions") << "\n";
        return 1;
    }

    std::cout << "Emitting LLVM IR: " << irPath.string() << "\n";
    if (!codeGenerator.emitObjectFile(irPath.string())) {
        printErrors(codeGenerator.errors());
        return 1;
    }

    // AOT remains self-contained when run from the build tree, but an
    // installed driver can supply its runtime and compiler explicitly or
    // through environment variables. This makes packaging reproducible
    // without silently linking against an unrelated build directory.
    if (options.runtimeLibrary.empty()) {
        if (const char* configured = std::getenv("LUNA_RUNTIME_LIB"))
            options.runtimeLibrary = configured;
    }
    if (options.runtimeLibrary.empty())
        options.runtimeLibrary = std::string(BUILD_DIR) + "/libruntime.a";
    if (!fs::exists(options.runtimeLibrary)) {
        std::cerr << diagnostic::format(
            "driver",
            "runtime library does not exist: '" + options.runtimeLibrary + "'",
            options.runtimeLibrary,
            0,
            0,
            "pass `--runtime-lib <path>` or set LUNA_RUNTIME_LIB to Luna's libruntime.a")
                  << "\n";
        return 1;
    }

    if (options.compiler.empty()) {
        if (const char* configured = std::getenv("LUNA_CXX"))
            options.compiler = configured;
    }
    if (options.compiler.empty()) options.compiler = "clang++";

    const char* optimizationFlag = "-O0";
    if (options.optimizationLevel == LunaOptimizationLevel::O2)
        optimizationFlag = "-O2";
    else if (options.optimizationLevel == LunaOptimizationLevel::O3)
        optimizationFlag = "-O3";

    auto compilerPath = llvm::sys::findProgramByName(options.compiler);
    if (!compilerPath) {
        std::cerr << diagnostic::format(
            "driver",
            "cannot find AOT compiler '" + options.compiler + "': " +
                compilerPath.getError().message(),
            options.compiler,
            0,
            0,
            "pass --cc with an executable path or add the compiler to PATH")
                  << "\n";
        return 1;
    }

    std::vector<std::string> linkerArgs = {
        *compilerPath,
        optimizationFlag,
    };
    if (options.artifactKind == AotArtifactKind::SharedLibrary) {
#ifdef __APPLE__
        linkerArgs.push_back("-dynamiclib");
#else
        linkerArgs.push_back("-shared");
#endif
    }
    linkerArgs.push_back(irPath.generic_string());
    linkerArgs.push_back(options.runtimeLibrary);
    if (std::string(LUNA_DL_LIBRARY).size())
        linkerArgs.push_back("-l" + std::string(LUNA_DL_LIBRARY));
    linkerArgs.push_back("-o");
    linkerArgs.push_back(artifactPath.generic_string());
    for (const auto& library : options.linkLibraries)
        linkerArgs.push_back(isLibraryPath(library) ? library : "-l" + library);

    std::cout << "Linking:";
    for (const auto& argument : linkerArgs)
        std::cout << ' ' << quoteForDisplay(argument);
    std::cout << "\n";

    std::vector<llvm::StringRef> linkerArgRefs;
    linkerArgRefs.reserve(linkerArgs.size());
    for (const auto& argument : linkerArgs)
        linkerArgRefs.emplace_back(argument);

    std::string executionError;
    const int linkResult = llvm::sys::ExecuteAndWait(
        *compilerPath,
        linkerArgRefs,
        std::nullopt,
        {},
        0,
        0,
        &executionError);
    if (linkResult != 0) {
        std::cerr << diagnostic::format(
            "driver",
            "AOT linker '" + options.compiler + "' failed with status " +
                std::to_string(linkResult) +
                (executionError.empty() ? "" : ": " + executionError),
            "",
            0,
            0,
            "inspect the linker command above; verify --cc, --runtime-lib, and every --link dependency")
                  << "\n";
        return 1;
    }

    std::cout << "Built "
              << (options.artifactKind == AotArtifactKind::SharedLibrary
                      ? "shared library: " : "executable: ")
              << artifactPath.string() << "\n";
    return 0;
}

} // namespace luna::driver
