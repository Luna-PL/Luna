#pragma once

#include "codegen/CodeGenerator.h"

#include <optional>
#include <string>
#include <vector>

namespace luna::driver {

enum class AotArtifactKind {
    Executable,
    SharedLibrary,
};

struct AotBuildCacheOptions {
    std::string inputPath;
    std::vector<std::string> linkLibraries;
    std::string runtimeLibrary;
    std::string compiler;
    std::string outputPath;
    std::string compilerExecutable;
    LunaOptimizationLevel optimizationLevel = LunaOptimizationLevel::O0;
    LunaGpuTargetConfig gpuTargets;
    bool reserveKernelRuntime = false;
    bool forceRelink = false;
};

struct AotBuildCacheProbe {
    bool current = false;
    bool forceRelink = false;
    std::string inputState;
    std::string artifactPath;
};

struct AotLinkOptions {
    std::string inputPath;
    std::string declaredPackageName;
    std::vector<std::string> linkLibraries;
    std::string runtimeLibrary;
    std::string compiler;
    std::string outputPath;
    LunaOptimizationLevel optimizationLevel = LunaOptimizationLevel::O0;
    AotArtifactKind artifactKind = AotArtifactKind::Executable;
    std::optional<AotBuildCacheOptions> buildCache;
    std::string buildCacheInputState;
};

class AotLinker {
public:
    static AotBuildCacheProbe probeBuildCache(const AotBuildCacheOptions& options);
    static int build(CodeGenerator& codeGenerator, AotLinkOptions options);
};

} // namespace luna::driver
