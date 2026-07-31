#include "driver/CommandLine.h"

#include <string>
#include <utility>

namespace luna::driver {
namespace {

bool parseOptimizationLevel(const std::string& value,
                            LunaOptimizationLevel& optimizationLevel) {
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

bool parseGpuTargets(const std::string& specification,
                     LunaGpuTargetConfig& targets,
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
        const std::string architecture = colon == std::string::npos
            ? "" : item.substr(colon + 1);
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
            error = "unknown GPU target '" + backend +
                "'; expected sim, cuda[:sm_*], or rocm[:gfx*]";
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
    if (command != "run" && command != "build" && command != "check")
        return failure("Unknown command: " + command, true);
    if (argc < 3)
        return failure("Error: Missing file argument", true);

    CommandLineOptions options;
    options.command = command;
    options.inputPath = argv[2];

    for (int i = 3; i < argc; ++i) {
        const std::string option = argv[i];
        if (parseOptimizationLevel(option, options.optimizationLevel)) {
            continue;
        } else if (option == "--opt" && i + 1 < argc) {
            if (!parseOptimizationLevel(argv[++i], options.optimizationLevel))
                return failure("Unsupported optimization level: " +
                    std::string(argv[i]), false);
        } else if (option.rfind("--opt=", 0) == 0) {
            if (!parseOptimizationLevel(option.substr(6), options.optimizationLevel))
                return failure("Unsupported optimization level: " +
                    option.substr(6), false);
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
            if (format != "json")
                return failure("Unsupported message format: " + format, false);
            options.messageFormat = MessageFormat::Json;
        } else if (option.rfind("--message-format=", 0) == 0) {
            const std::string format = option.substr(17);
            if (format != "json")
                return failure("Unsupported message format: " + format, false);
            options.messageFormat = MessageFormat::Json;
        } else {
            return failure("Unknown option: " + option, true);
        }
    }

    if (options.messageFormat == MessageFormat::Json && command != "check")
        return failure("--message-format=json is currently supported only by `check`",
                       false);
    if (options.messageFormat == MessageFormat::Json && options.printMoonCostReport)
        return failure("--moon-cost-report cannot be combined with JSON diagnostics",
                       false);

    return {std::move(options), "", false};
}

} // namespace luna::driver
