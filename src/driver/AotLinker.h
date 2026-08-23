#pragma once

#include "codegen/CodeGenerator.h"

#include <string>
#include <vector>

namespace luna::driver {

enum class AotArtifactKind {
    Executable,
    SharedLibrary,
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
};

class AotLinker {
public:
    static int build(CodeGenerator& codeGenerator, AotLinkOptions options);
};

} // namespace luna::driver
