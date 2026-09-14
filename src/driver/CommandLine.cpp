#include "driver/CommandLine.h"

#include <charconv>
#include <limits>
#include <string>
#include <utility>

namespace luna::driver {
namespace {

bool parseOptimizationLevel(const std::string& value, LunaOptimizationLevel& optimizationLevel) {
    if (value == "-O0" || value == "O0")
        optimizationLevel = LunaOptimizationLevel::O0;
    else if (value == "-O2" || value == "O2")
        optimizationLevel = LunaOptimizationLevel::O2;
    else if (value == "-O3" || value == "O3")
        optimizationLevel = LunaOptimizationLevel::O3;
    else
        return false;
    return true;
}

bool parseUnsignedInRange(const std::string& value, unsigned minimum, unsigned maximum,
                          unsigned& parsedValue) {
    unsigned parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (value.empty() || result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        parsed < minimum || parsed > maximum)
        return false;
    parsedValue = parsed;
    return true;
}

bool parseWorkerSignal(const std::string& value, uint64_t& parsedValue) {
    uint64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (value.empty() || result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        parsed == 0)
        return false;
    parsedValue = parsed;
    return true;
}

bool parseReplTimeout(const std::string& value, unsigned& timeout) {
    return parseUnsignedInRange(value, 1, 3600, timeout);
}

bool parseGpuTargets(const std::string& specification, LunaGpuTargetConfig& targets,
                     std::string& error) {
    if (specification.empty()) {
        error = "GPU target list must not be empty";
        return false;
    }
    size_t start = 0;
    while (start <= specification.size()) {
        const size_t comma = specification.find(',', start);
        const std::string item = specification.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start);
        if (item.empty()) {
            error = "GPU target list contains an empty target";
            return false;
        }
        const size_t colon = item.find(':');
        const std::string backend = item.substr(0, colon);
        const std::string architecture = colon == std::string::npos ? "" : item.substr(colon + 1);
        if (backend == "sim") {
            if (colon != std::string::npos) {
                error = "sim GPU target does not accept an architecture";
                return false;
            }
        } else if (backend == "cuda") {
            const std::string selected = architecture.empty() ? "sm_52" : architecture;
            if (colon != std::string::npos && architecture.empty()) {
                error = "CUDA GPU target requires an architecture after ':'";
                return false;
            }
            if (selected.rfind("sm_", 0) != 0) {
                error = "CUDA architecture must use the sm_* spelling";
                return false;
            }
            if (targets.emitPTX && targets.cudaArchitecture != selected) {
                error = "one artifact cannot contain multiple CUDA architectures yet";
                return false;
            }
            targets.emitPTX = true;
            targets.cudaArchitecture = selected;
        } else if (backend == "rocm") {
            const std::string selected = architecture.empty() ? "gfx1101" : architecture;
            if (colon != std::string::npos && architecture.empty()) {
                error = "ROCm GPU target requires an architecture after ':'";
                return false;
            }
            if (selected.rfind("gfx", 0) != 0) {
                error = "ROCm architecture must use the gfx* spelling";
                return false;
            }
            if (targets.emitHSACO && targets.rocmArchitecture != selected) {
                error = "one artifact cannot contain multiple ROCm architectures yet";
                return false;
            }
            targets.emitHSACO = true;
            targets.rocmArchitecture = selected;
        } else {
            error =
                "unknown GPU target '" + backend + "'; expected sim, cuda[:sm_*], or rocm[:gfx*]";
            return false;
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return true;
}

CommandLineParseResult failure(std::string error, bool showUsage) {
    return {std::nullopt, std::move(error), showUsage};
}

} // namespace

CommandLineParseResult parseCommandLine(int argc, char* argv[]) {
    const std::string command = argv[1];
    if (command != "run" && command != "build" && command != "check" && command != "analyze" &&
        command != "repl" && command != "__repl-worker")
        return failure("Unknown command: " + command, true);

    if (command == "repl" || command == "__repl-worker") {
        CommandLineOptions options;
        options.command = command;
        for (int i = 2; i < argc; ++i) {
            const std::string option = argv[i];
            if (parseOptimizationLevel(option, options.optimizationLevel)) { continue; }
            if (option == "--opt") {
                if (i + 1 >= argc) return failure("Missing value for --opt", false);
                if (!parseOptimizationLevel(argv[++i], options.optimizationLevel))
                    return failure("Unsupported optimization level: " + std::string(argv[i]),
                                   false);
                continue;
            }
            if (option.rfind("--opt=", 0) == 0) {
                if (!parseOptimizationLevel(option.substr(6), options.optimizationLevel))
                    return failure("Unsupported optimization level: " + option.substr(6), false);
                continue;
            }
            if (option == "--link") {
                if (i + 1 >= argc) return failure("Missing value for --link", false);
                options.linkLibraries.push_back(argv[++i]);
                continue;
            }
            if (option.rfind("--link=", 0) == 0) {
                if (option.size() == 7)
                    return failure("--link requires a non-empty library path", false);
                options.linkLibraries.push_back(option.substr(7));
                continue;
            }
            if (command == "repl" && option == "--no-prompt") {
                options.replNoPrompt = true;
                continue;
            }
            if (command == "repl" && (option == "--help" || option == "-h")) {
                options.replShowHelp = true;
                continue;
            }
            if (command == "repl" && option == "--timings") {
                options.replShowTimings = true;
                continue;
            }
            if (command == "repl" && option == "--timeout") {
                if (i + 1 >= argc) return failure("Missing value for --timeout", false);
                if (!parseReplTimeout(argv[++i], options.replTimeoutSeconds))
                    return failure("Invalid REPL timeout: " + std::string(argv[i]) +
                                       "; expected 1..3600 seconds",
                                   false);
                continue;
            }
            if (command == "repl" && option.rfind("--timeout=", 0) == 0) {
                if (!parseReplTimeout(option.substr(10), options.replTimeoutSeconds))
                    return failure("Invalid REPL timeout: " + option.substr(10) +
                                       "; expected 1..3600 seconds",
                                   false);
                continue;
            }
            if (command == "repl" && option == "--memory-limit") {
                if (i + 1 >= argc) return failure("Missing value for --memory-limit", false);
                if (!parseUnsignedInRange(argv[++i], 256, 65536, options.replMemoryLimitMiB))
                    return failure("Invalid REPL memory limit: " + std::string(argv[i]) +
                                       "; expected 256..65536 MiB",
                                   false);
                continue;
            }
            if (command == "repl" && option.rfind("--memory-limit=", 0) == 0) {
                if (!parseUnsignedInRange(option.substr(15), 256, 65536,
                                          options.replMemoryLimitMiB))
                    return failure("Invalid REPL memory limit: " + option.substr(15) +
                                       "; expected 256..65536 MiB",
                                   false);
                continue;
            }
            if (command == "repl" && option == "--output-limit") {
                if (i + 1 >= argc) return failure("Missing value for --output-limit", false);
                if (!parseUnsignedInRange(argv[++i], 1, 1024, options.replOutputLimitMiB))
                    return failure("Invalid REPL output limit: " + std::string(argv[i]) +
                                       "; expected 1..1024 MiB",
                                   false);
                continue;
            }
            if (command == "repl" && option.rfind("--output-limit=", 0) == 0) {
                if (!parseUnsignedInRange(option.substr(15), 1, 1024, options.replOutputLimitMiB))
                    return failure("Invalid REPL output limit: " + option.substr(15) +
                                       "; expected 1..1024 MiB",
                                   false);
                continue;
            }
            if (command == "__repl-worker" && option == "--request-channel") {
                if (i + 1 >= argc) return failure("Missing value for --request-channel", false);
                if (!parseWorkerSignal(argv[++i], options.replWorkerRequestChannel))
                    return failure("Invalid REPL worker request channel: " + std::string(argv[i]),
                                   false);
                continue;
            }
            if (command == "__repl-worker" && option.rfind("--request-channel=", 0) == 0) {
                if (!parseWorkerSignal(option.substr(18), options.replWorkerRequestChannel))
                    return failure("Invalid REPL worker request channel: " + option.substr(18),
                                   false);
                continue;
            }
            if (command == "__repl-worker" && option == "--result-channel") {
                if (i + 1 >= argc) return failure("Missing value for --result-channel", false);
                if (!parseWorkerSignal(argv[++i], options.replWorkerResultChannel))
                    return failure("Invalid REPL worker result channel: " + std::string(argv[i]),
                                   false);
                continue;
            }
            if (command == "__repl-worker" && option.rfind("--result-channel=", 0) == 0) {
                if (!parseWorkerSignal(option.substr(17), options.replWorkerResultChannel))
                    return failure("Invalid REPL worker result channel: " + option.substr(17),
                                   false);
                continue;
            }
            if (command == "__repl-worker" && option == "--stdout-channel") {
                if (i + 1 >= argc) return failure("Missing value for --stdout-channel", false);
                if (!parseWorkerSignal(argv[++i], options.replWorkerStdoutChannel))
                    return failure("Invalid REPL worker stdout channel: " + std::string(argv[i]),
                                   false);
                continue;
            }
            if (command == "__repl-worker" && option.rfind("--stdout-channel=", 0) == 0) {
                if (!parseWorkerSignal(option.substr(17), options.replWorkerStdoutChannel))
                    return failure("Invalid REPL worker stdout channel: " + option.substr(17),
                                   false);
                continue;
            }
            if (command == "__repl-worker" && option == "--stderr-channel") {
                if (i + 1 >= argc) return failure("Missing value for --stderr-channel", false);
                if (!parseWorkerSignal(argv[++i], options.replWorkerStderrChannel))
                    return failure("Invalid REPL worker stderr channel: " + std::string(argv[i]),
                                   false);
                continue;
            }
            if (command == "__repl-worker" && option.rfind("--stderr-channel=", 0) == 0) {
                if (!parseWorkerSignal(option.substr(17), options.replWorkerStderrChannel))
                    return failure("Invalid REPL worker stderr channel: " + option.substr(17),
                                   false);
                continue;
            }
            if (command == "__repl-worker" && option == "--ready") {
                if (i + 1 >= argc) return failure("Missing value for --ready", false);
                if (!parseWorkerSignal(argv[++i], options.replWorkerReadySignal))
                    return failure("Invalid REPL worker ready signal: " + std::string(argv[i]),
                                   false);
                continue;
            }
            if (command == "__repl-worker" && option.rfind("--ready=", 0) == 0) {
                if (!parseWorkerSignal(option.substr(8), options.replWorkerReadySignal))
                    return failure("Invalid REPL worker ready signal: " + option.substr(8), false);
                continue;
            }
            if (command == "__repl-worker" && option == "--gate") {
                if (i + 1 >= argc) return failure("Missing value for --gate", false);
                if (!parseWorkerSignal(argv[++i], options.replWorkerGateSignal))
                    return failure("Invalid REPL worker gate signal: " + std::string(argv[i]),
                                   false);
                continue;
            }
            if (command == "__repl-worker" && option.rfind("--gate=", 0) == 0) {
                if (!parseWorkerSignal(option.substr(7), options.replWorkerGateSignal))
                    return failure("Invalid REPL worker gate signal: " + option.substr(7), false);
                continue;
            }
            if (command == "__repl-worker" && option == "--complete") {
                if (i + 1 >= argc) return failure("Missing value for --complete", false);
                if (!parseWorkerSignal(argv[++i], options.replWorkerCompletionSignal))
                    return failure("Invalid REPL worker completion signal: " + std::string(argv[i]),
                                   false);
                continue;
            }
            if (command == "__repl-worker" && option.rfind("--complete=", 0) == 0) {
                if (!parseWorkerSignal(option.substr(11), options.replWorkerCompletionSignal))
                    return failure("Invalid REPL worker completion signal: " + option.substr(11),
                                   false);
                continue;
            }
            if (command == "__repl-worker" && option == "--worker-memory-limit") {
                if (i + 1 >= argc) return failure("Missing value for --worker-memory-limit", false);
                if (!parseUnsignedInRange(argv[++i], 0, 65536, options.replWorkerMemoryLimitMiB))
                    return failure("Invalid REPL worker memory limit: " + std::string(argv[i]),
                                   false);
                options.replWorkerMemoryLimitSpecified = true;
                continue;
            }
            if (command == "__repl-worker" && option.rfind("--worker-memory-limit=", 0) == 0) {
                if (!parseUnsignedInRange(option.substr(22), 0, 65536,
                                          options.replWorkerMemoryLimitMiB))
                    return failure("Invalid REPL worker memory limit: " + option.substr(22), false);
                options.replWorkerMemoryLimitSpecified = true;
                continue;
            }
            if (command == "__repl-worker" && option == "--parent-pid") {
                if (i + 1 >= argc) return failure("Missing value for --parent-pid", false);
                if (!parseUnsignedInRange(argv[++i], 1, std::numeric_limits<unsigned>::max(),
                                          options.replWorkerParentProcessId))
                    return failure("Invalid REPL worker parent pid: " + std::string(argv[i]),
                                   false);
                continue;
            }
            if (command == "__repl-worker" && option.rfind("--parent-pid=", 0) == 0) {
                if (!parseUnsignedInRange(option.substr(13), 1,
                                          std::numeric_limits<unsigned>::max(),
                                          options.replWorkerParentProcessId))
                    return failure("Invalid REPL worker parent pid: " + option.substr(13), false);
                continue;
            }
            return failure(command == "repl" ? "Unknown REPL option: " + option
                                             : "Unknown REPL worker option: " + option,
                           command == "repl");
        }
        if (command == "__repl-worker" && options.replWorkerRequestChannel == 0)
            return failure("REPL worker requires --request-channel", false);
        if (command == "__repl-worker" && options.replWorkerResultChannel == 0)
            return failure("REPL worker requires --result-channel", false);
        if (command == "__repl-worker" && options.replWorkerStdoutChannel == 0)
            return failure("REPL worker requires --stdout-channel", false);
        if (command == "__repl-worker" && options.replWorkerStderrChannel == 0)
            return failure("REPL worker requires --stderr-channel", false);
        if (command == "__repl-worker" && options.replWorkerReadySignal == 0)
            return failure("REPL worker requires --ready", false);
        if (command == "__repl-worker" && options.replWorkerGateSignal == 0)
            return failure("REPL worker requires --gate", false);
        if (command == "__repl-worker" && options.replWorkerCompletionSignal == 0)
            return failure("REPL worker requires --complete", false);
        if (command == "__repl-worker" && options.replWorkerParentProcessId == 0)
            return failure("REPL worker requires --parent-pid", false);
        if (command == "__repl-worker" && !options.replWorkerMemoryLimitSpecified)
            return failure("REPL worker requires --worker-memory-limit", false);
        return {std::move(options), "", false};
    }

    if (argc < 3) return failure("Error: Missing file argument", true);

    CommandLineOptions options;
    options.command = command;
    options.inputPath = argv[2];
    bool artifactTargetExplicit = false;

    for (int i = 3; i < argc; ++i) {
        const std::string option = argv[i];
        if (parseOptimizationLevel(option, options.optimizationLevel)) {
            continue;
        } else if (option == "--opt" && i + 1 < argc) {
            if (!parseOptimizationLevel(argv[++i], options.optimizationLevel))
                return failure("Unsupported optimization level: " + std::string(argv[i]), false);
        } else if (option.rfind("--opt=", 0) == 0) {
            if (!parseOptimizationLevel(option.substr(6), options.optimizationLevel))
                return failure("Unsupported optimization level: " + option.substr(6), false);
        } else if (option == "--link" && i + 1 < argc) {
            options.linkLibraries.push_back(argv[++i]);
        } else if (option.rfind("--link=", 0) == 0) {
            options.linkLibraries.push_back(option.substr(7));
        } else if (option == "--runtime-lib" && i + 1 < argc) {
            options.runtimeLibrary = argv[++i];
        } else if (option.rfind("--runtime-lib=", 0) == 0) {
            options.runtimeLibrary = option.substr(14);
        } else if (option == "--cc" && i + 1 < argc) {
            options.aotCompiler = argv[++i];
        } else if (option.rfind("--cc=", 0) == 0) {
            options.aotCompiler = option.substr(5);
        } else if ((option == "-t" || option == "--target") && i + 1 < argc) {
            artifactTargetExplicit = true;
            const std::string target = argv[++i];
            if (target == "native")
                options.artifactTarget = ArtifactTarget::Native;
            else if (target == "moon")
                options.artifactTarget = ArtifactTarget::Moon;
            else if (target == "cffi")
                options.artifactTarget = ArtifactTarget::Cffi;
            else
                return failure("Unsupported artifact target: " + target, false);
        } else if (option.rfind("--target=", 0) == 0) {
            artifactTargetExplicit = true;
            const std::string target = option.substr(9);
            if (target == "native")
                options.artifactTarget = ArtifactTarget::Native;
            else if (target == "moon")
                options.artifactTarget = ArtifactTarget::Moon;
            else if (target == "cffi")
                options.artifactTarget = ArtifactTarget::Cffi;
            else
                return failure("Unsupported artifact target: " + target, false);
        } else if (option == "-o" && i + 1 < argc) {
            options.outputPath = argv[++i];
        } else if (option.rfind("--output=", 0) == 0) {
            options.outputPath = option.substr(9);
        } else if (option == "--gpu-target" && i + 1 < argc) {
            std::string targetError;
            if (!parseGpuTargets(argv[++i], options.gpuTargets, targetError))
                return failure("Invalid --gpu-target: " + targetError, false);
        } else if (option.rfind("--gpu-target=", 0) == 0) {
            std::string targetError;
            if (!parseGpuTargets(option.substr(13), options.gpuTargets, targetError))
                return failure("Invalid --gpu-target: " + targetError, false);
        } else if (option == "--reserve-kernel-runtime") {
            options.reserveKernelRuntime = true;
        } else if (option == "--moon-cost-report") {
            options.printMoonCostReport = true;
        } else if (option == "--emit-moonir" && i + 1 < argc) {
            options.moonIrOutput = argv[++i];
        } else if (option.rfind("--emit-moonir=", 0) == 0) {
            options.moonIrOutput = option.substr(14);
        } else if (option == "--message-format" && i + 1 < argc) {
            const std::string format = argv[++i];
            if (format != "json") return failure("Unsupported message format: " + format, false);
            options.messageFormat = MessageFormat::Json;
        } else if (option.rfind("--message-format=", 0) == 0) {
            const std::string format = option.substr(17);
            if (format != "json") return failure("Unsupported message format: " + format, false);
            options.messageFormat = MessageFormat::Json;
        } else if (option == "--overlay" && i + 1 < argc) {
            options.overlayPath = argv[++i];
        } else if (option.rfind("--overlay=", 0) == 0) {
            options.overlayPath = option.substr(10);
        } else if (option == "--overlays-from-stdin") {
            options.overlaysFromStdin = true;
        } else {
            return failure("Unknown option: " + option, true);
        }
    }

    if (options.messageFormat == MessageFormat::Json && command != "check" && command != "analyze")
        return failure("--message-format=json is currently supported only by `check` and `analyze`",
                       false);
    if (command == "analyze" && options.messageFormat != MessageFormat::Json)
        return failure("`analyze` requires --message-format=json", false);
    if (!options.overlayPath.empty() && command != "analyze")
        return failure("--overlay is supported only by `analyze`", false);
    if (options.overlaysFromStdin && command != "analyze")
        return failure("--overlays-from-stdin is supported only by `analyze`", false);
    if (options.overlaysFromStdin && !options.overlayPath.empty())
        return failure("--overlay and --overlays-from-stdin cannot be combined", false);
    if (options.messageFormat == MessageFormat::Json && options.printMoonCostReport)
        return failure("--moon-cost-report cannot be combined with JSON diagnostics", false);
    if (command != "build" && artifactTargetExplicit)
        return failure("-t/--target is supported only by `build`", false);
    if (command != "build" && !options.outputPath.empty())
        return failure("-o/--output is supported only by `build`", false);
    if (options.artifactTarget == ArtifactTarget::Moon &&
        (!options.linkLibraries.empty() || !options.runtimeLibrary.empty() ||
         !options.aotCompiler.empty() || options.gpuTargets.emitPTX ||
         options.gpuTargets.emitHSACO))
        return failure("-t moon cannot be combined with native linker or GPU artifact options",
                       false);

    return {std::move(options), "", false};
}

} // namespace luna::driver
