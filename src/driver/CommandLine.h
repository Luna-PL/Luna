#pragma once

#include "codegen/CodeGenerator.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace luna::driver {

enum class MessageFormat {
    Human,
    Json,
};

enum class ArtifactTarget {
    Native,
    Moon,
    Cffi,
};

struct CommandLineOptions {
    std::string command;
    std::string inputPath;
    std::vector<std::string> linkLibraries;
    std::string runtimeLibrary;
    std::string aotCompiler;
    std::string outputPath;
    std::string moonIrOutput;
    std::string overlayPath;
    bool overlaysFromStdin = false;
    LunaGpuTargetConfig gpuTargets;
    MessageFormat messageFormat = MessageFormat::Human;
    ArtifactTarget artifactTarget = ArtifactTarget::Native;
    bool printMoonCostReport = false;
    bool reserveKernelRuntime = false;
    bool replNoPrompt = false;
    bool replShowHelp = false;
    bool replShowTimings = false;
    unsigned replTimeoutSeconds = 30;
    unsigned replMemoryLimitMiB = 1024;
    unsigned replOutputLimitMiB = 16;
    uint64_t replWorkerRequestChannel = 0;
    uint64_t replWorkerResultChannel = 0;
    uint64_t replWorkerStdoutChannel = 0;
    uint64_t replWorkerStderrChannel = 0;
    uint64_t replWorkerReadySignal = 0;
    uint64_t replWorkerGateSignal = 0;
    uint64_t replWorkerCompletionSignal = 0;
    unsigned replWorkerParentProcessId = 0;
    unsigned replWorkerMemoryLimitMiB = 0;
    bool replWorkerMemoryLimitSpecified = false;
    LunaOptimizationLevel optimizationLevel = LunaOptimizationLevel::O0;
};

struct CommandLineParseResult {
    std::optional<CommandLineOptions> options;
    std::string error;
    bool showUsage = false;
};

CommandLineParseResult parseCommandLine(int argc, char* argv[]);

} // namespace luna::driver
