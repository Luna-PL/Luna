#include "driver/Driver.h"
#include "Version.h"
#include "diagnostics/Diagnostic.h"
#include "driver/AotLinker.h"
#include "driver/CommandLine.h"
#include "driver/CompilerPipeline.h"
#include "driver/NativeArtifact.h"
#include "driver/Repl.h"
#include "moonir/ContainerModel.h"
#include "moonir/Printer.h"
#include "package/Package.h"
#include "runtime/Runtime.h"
#include "tooling/AnalysisSnapshot.h"
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "driver/DriverInternal.h"

namespace luna::driver {

#ifndef LUNA_COMPILER_COMMIT
#define LUNA_COMPILER_COMMIT "unknown"
#endif

using namespace driver_detail;

int run(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage();
        return 0;
    }

    std::string cmd = argv[1];

    if (cmd == "--version" || cmd == "-V" || cmd == "version") {
        std::cout << "Luna " << LUNA_VERSION_STRING << "\n";
        return 0;
    }

    auto parseResult = parseCommandLine(argc, argv);
    if (!parseResult.options) {
        if (requestsJsonOutput(argc, argv)) {
            if (argc > 1 && std::string(argv[1]) == "analyze") {
                printAnalysisHello();
                printAnalysisSummary(0, 0, false);
            } else {
                printJsonHello();
                const auto invocationError =
                    diagnostic::format("driver", parseResult.error, "", 0, 0,
                                       "run `luna` without arguments to view supported commands");
                printJsonDiagnostics({invocationError});
                printJsonSummary(1);
            }
            return 2;
        }
        std::cerr << parseResult.error << "\n";
        if (parseResult.showUsage) printUsage();
        return 1;
    }
    auto options = std::move(*parseResult.options);
    if (cmd == "__repl-worker") {
        return runReplWorker(std::cerr, options.linkLibraries, options.replWorkerRequestChannel,
                             options.replWorkerResultChannel, options.replWorkerReadySignal,
                             options.replWorkerGateSignal, options.replWorkerCompletionSignal,
                             options.replWorkerStdoutChannel, options.replWorkerStderrChannel,
                             options.replWorkerParentProcessId, options.replWorkerMemoryLimitMiB,
                             options.optimizationLevel);
    }
    if (cmd == "repl") {
        if (options.replShowHelp) {
            printReplUsage(std::cout);
            return 0;
        }
        static int mainExecutableAnchor = 0;
        ReplOptions replOptions;
        replOptions.optimizationLevel = options.optimizationLevel;
        replOptions.showPrompts = !options.replNoPrompt;
        replOptions.showTimings = options.replShowTimings;
        replOptions.workerExecutable =
            llvm::sys::fs::getMainExecutable(argv[0], &mainExecutableAnchor);
        replOptions.linkLibraries = std::move(options.linkLibraries);
        replOptions.executionTimeoutSeconds = options.replTimeoutSeconds;
        replOptions.executionMemoryLimitMiB = options.replMemoryLimitMiB;
        replOptions.executionOutputLimitMiB = options.replOutputLimitMiB;
        return runRepl(std::cin, std::cout, std::cerr, std::move(replOptions));
    }
    auto& filePath = options.inputPath;
    auto& linkLibraries = options.linkLibraries;
    auto& runtimeLibrary = options.runtimeLibrary;
    auto& aotCompiler = options.aotCompiler;
    auto& outputPath = options.outputPath;
    auto& moonIrOutput = options.moonIrOutput;
    auto& overlayPath = options.overlayPath;
    const bool overlaysFromStdin = options.overlaysFromStdin;
    auto& gpuTargets = options.gpuTargets;
    auto& printMoonCostReport = options.printMoonCostReport;
    auto& reserveKernelRuntime = options.reserveKernelRuntime;
    auto& optimizationLevel = options.optimizationLevel;
    const auto artifactTarget = options.artifactTarget;
    const bool jsonDiagnostics = options.messageFormat == MessageFormat::Json && cmd == "check";

    if (jsonDiagnostics) printJsonHello();

    if (cmd == "analyze") {
        printAnalysisHello();
        SourceOverlays overlays;
        if (!overlayPath.empty() || overlaysFromStdin) {
            std::ostringstream input;
            input << std::cin.rdbuf();
            if (std::cin.bad()) {
                std::cerr << "failed to read analysis overlay stdin\n";
                printAnalysisSummary(0, 0, false);
                return 2;
            }
            if (overlaysFromStdin) {
                std::string overlayError;
                auto parsed = parseAnalysisOverlays(input.str(), overlayError);
                if (!parsed) {
                    std::cerr << overlayError << '\n';
                    printAnalysisSummary(0, 0, false);
                    return 2;
                }
                overlays = std::move(*parsed);
            } else {
                overlays.push_back({overlayPath, input.str()});
            }
        }
        auto snapshot =
            overlays.empty()
                ? tooling::AnalysisSnapshot::analyzePath(filePath)
                : tooling::AnalysisSnapshot::analyzePathWithOverlays(filePath, overlays);
        size_t emitted = 0;
        size_t emittedReferences = 0;
        bool locationsComplete = true;
        for (const auto& symbol : snapshot.symbolIndex().declarations()) {
            if (printAnalysisSymbol(symbol, overlays))
                ++emitted;
            else
                locationsComplete = false;
        }
        for (const auto& reference : snapshot.referenceIndex().references()) {
            if (printAnalysisReference(reference, overlays))
                ++emittedReferences;
            else
                locationsComplete = false;
        }
        printAnalysisSummary(emitted, emittedReferences, snapshot.success() && locationsComplete);
        return snapshot.success() && locationsComplete ? 0 : 1;
    }

    if (cmd != "build" && (!runtimeLibrary.empty() || !aotCompiler.empty())) {
        const auto invocationError =
            diagnostic::format("driver", "AOT linker options are only valid with `build`", "", 0, 0,
                               "use `luna build ... --runtime-lib <path> --cc <compiler>");
        if (jsonDiagnostics) {
            printJsonDiagnostics({invocationError});
            printJsonSummary(1);
            return 2;
        }
        std::cerr << invocationError << "\n";
        return 1;
    }

    if (cmd == "build") {
        namespace fs = std::filesystem;
        const fs::path packagePath(filePath);
        std::error_code filesystemError;
        if (!fs::is_directory(packagePath, filesystemError) ||
            !fs::is_regular_file(packagePath / "luna.package", filesystemError)) {
            std::cerr << diagnostic::format(
                             "driver",
                             "formal artifact builds require a package directory with luna.package",
                             filePath, 0, 0,
                             "use standalone files with `check`, `run`, or `analyze`; pass a "
                             "package directory to `build`")
                      << "\n";
            return 1;
        }
    }

    std::optional<AotBuildCacheOptions> nativeBuildCache;
    std::string nativeBuildInputState;
    if (cmd == "build" && artifactTarget == ArtifactTarget::Native &&
        moonIrOutput.empty() && !printMoonCostReport) {
        static int mainExecutableAnchor = 0;
        AotBuildCacheOptions cacheOptions;
        cacheOptions.inputPath = filePath;
        cacheOptions.linkLibraries = linkLibraries;
        cacheOptions.runtimeLibrary = runtimeLibrary;
        cacheOptions.compiler = aotCompiler;
        cacheOptions.outputPath = outputPath;
        cacheOptions.compilerExecutable =
            llvm::sys::fs::getMainExecutable(argv[0], &mainExecutableAnchor);
        cacheOptions.optimizationLevel = optimizationLevel;
        cacheOptions.gpuTargets = gpuTargets;
        cacheOptions.reserveKernelRuntime = reserveKernelRuntime;
        auto cacheProbe = AotLinker::probeBuildCache(cacheOptions);
        cacheOptions.forceRelink = cacheProbe.forceRelink;
        nativeBuildInputState = std::move(cacheProbe.inputState);
        nativeBuildCache = std::move(cacheOptions);
        if (cacheProbe.current) {
            std::cout << "Up to date: " << cacheProbe.artifactPath << "\n";
            return 0;
        }
    }

    CompilerPipeline pipeline;
    if (!pipeline.compileToMoonIR(
            {filePath, optimizationLevel, reserveKernelRuntime, cmd == "build"})) {
        if (jsonDiagnostics) {
            printJsonDiagnostics(pipeline.errors());
            printJsonSummary(pipeline.errors().size());
        } else {
            printErrors(pipeline.errors(),
                        pipeline.errorStage().empty() ? nullptr : pipeline.errorStage().c_str());
        }
        return 1;
    }

    moon::Printer moonPrinter;
    if (!moonIrOutput.empty()) {
        std::ofstream output(moonIrOutput);
        if (!output) {
            const auto outputError = diagnostic::format(
                "driver", "cannot write MoonIR file '" + moonIrOutput + "'", moonIrOutput, 0, 0,
                "check the output directory and permissions");
            if (jsonDiagnostics) {
                printJsonDiagnostics({outputError});
                printJsonSummary(1);
            } else {
                std::cerr << outputError << "\n";
            }
            return 1;
        }
        moonPrinter.print(pipeline.moonModule(), output);
    }
    if (printMoonCostReport) moonPrinter.printCostReport(pipeline.moonModule(), std::cout);

    // Library packages deliberately have no main function. `check` validates
    // the complete frontend -> MoonIR boundary without manufacturing an
    // executable entry point or paying LLVM code-generation costs.
    if (cmd == "check") {
        if (jsonDiagnostics) printJsonSummary(0);
        return 0;
    }

    if (cmd == "build" && artifactTarget == ArtifactTarget::Moon)
        return buildMoonContainer(pipeline, filePath, outputPath);

    if (cmd == "build") {
        const auto& package = pipeline.analysisSnapshot().packageManifest();
        if (package.kind == PackageKind::Unspecified) {
            std::cerr << diagnostic::format(
                             "driver", "formal artifact builds require an explicit package kind",
                             filePath, 0, 0,
                             "set kind = \"application\" or kind = \"library\" in luna.package")
                      << "\n";
            return 1;
        }
        if (artifactTarget == ArtifactTarget::Cffi) {
            std::vector<const moon::FunctionDecl*> exports;
            std::string cffiError;
            if (!std::filesystem::is_directory(std::filesystem::path(filePath)) ||
                !collectCffiExports(pipeline, exports, cffiError)) {
                if (cffiError.empty()) cffiError = "-t cffi requires a package directory";
                std::cerr << diagnostic::format(
                                 "driver", cffiError, filePath, 0, 0,
                                 "pass the directory containing a library luna.package")
                          << "\n";
                return 1;
            }
        } else if (artifactTarget == ArtifactTarget::Native) {
            if (package.kind == PackageKind::Library) {
                size_t mainCount = 0;
                for (const auto& declaration : pipeline.moonModule().declarations) {
                    const auto* function =
                        dynamic_cast<const moon::FunctionDecl*>(declaration.get());
                    if (function && function->packageId == package.id && function->name == "main")
                        ++mainCount;
                }
                if (mainCount != 0) {
                    std::cerr << diagnostic::format(
                                     "driver", "Native library must not contain a package main",
                                     filePath, 0, 0, "remove main or set kind = \"application\"")
                              << "\n";
                    return 1;
                }
            } else {
                size_t mainCount = 0;
                for (const auto& declaration : pipeline.moonModule().declarations) {
                    const auto* function =
                        dynamic_cast<const moon::FunctionDecl*>(declaration.get());
                    if (function && function->packageId == package.id && function->name == "main")
                        ++mainCount;
                }
                if (mainCount != 1) {
                    std::cerr << diagnostic::format(
                                     "driver",
                                     "Native application must contain exactly one package main",
                                     filePath, 0, 0,
                                     "define exactly one `fn main() -> i32` in the root package")
                              << "\n";
                    return 1;
                }
            }
        }
    }

    if (!pipeline.generateCode(std::move(gpuTargets))) {
        printErrors(pipeline.errors());
        return 1;
    }
    auto& cg = pipeline.codeGenerator();

    if (cmd == "run") {
        if (!loadJITLibraries(linkLibraries)) return 1;
        const auto execution = cg.jitRun();
        if (!execution.executed) {
            std::cerr << "JIT: " << execution.error << "\n";
            return 1;
        }
        const int result = execution.exitCode;
        // Keep the CLI marker after all observable program output on every
        // CRT, including MinGW/UCRT under redirected GitHub Actions stdout.
        std::fflush(stdout);
        std::cout << "Program exited with code: " << result << std::endl;
        return result;
    }

    if (cmd == "build" && artifactTarget == ArtifactTarget::Cffi)
        return buildCffiLibrary(pipeline, cg, filePath, linkLibraries, runtimeLibrary, aotCompiler,
                                outputPath, optimizationLevel);

    if (cmd == "build" && artifactTarget == ArtifactTarget::Native &&
        pipeline.analysisSnapshot().packageManifest().kind == PackageKind::Library)
        return buildNativeLibrary(pipeline, cg, filePath, linkLibraries, runtimeLibrary,
                                  aotCompiler, outputPath, optimizationLevel);

    if (cmd == "build") {
        std::string nativeOutputPath = outputPath;
        if (nativeOutputPath.empty()) {
            const auto& package = pipeline.analysisSnapshot().packageManifest();
            std::filesystem::path artifactPath =
                std::filesystem::path(pipeline.analysisSnapshot().packageRootPath()) / "build" /
                "native" / packageArtifactName(package.id);
#ifdef _WIN32
            artifactPath += ".exe";
#endif
            nativeOutputPath = artifactPath.string();
        }
        AotLinkOptions linkOptions{
            filePath,
            pipeline.declaredPackageName(),
            linkLibraries,
            runtimeLibrary,
            aotCompiler,
            nativeOutputPath,
            optimizationLevel,
            AotArtifactKind::Executable,
            std::nullopt,
            {},
        };
        linkOptions.buildCache = std::move(nativeBuildCache);
        linkOptions.buildCacheInputState = std::move(nativeBuildInputState);
        return AotLinker::build(cg, std::move(linkOptions));
    }

    std::cerr << "Unknown command: " << cmd << "\n";
    printUsage();
    return 1;
}

} // namespace luna::driver
