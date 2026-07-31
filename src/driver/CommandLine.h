#pragma once

#include "codegen/CodeGenerator.h"

#include <optional>
#include <string>
#include <vector>

namespace luna::driver {

struct CommandLineOptions {
    std::string command;
    std::string inputPath;
    std::vector<std::string> linkLibraries;
    std::string runtimeLibrary;
    std::string aotCompiler;
    std::string moonIrOutput;
    LunaGpuTargetConfig gpuTargets;
    bool printMoonCostReport = false;
    bool reserveKernelRuntime = false;
    LunaOptimizationLevel optimizationLevel = LunaOptimizationLevel::O0;
};

struct CommandLineParseResult {
    std::optional<CommandLineOptions> options;
    std::string error;
    bool showUsage = false;
};

CommandLineParseResult parseCommandLine(int argc, char* argv[]);

} // namespace luna::driver
