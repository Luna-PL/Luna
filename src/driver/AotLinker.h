#pragma once

#include "codegen/CodeGenerator.h"

#include <string>
#include <vector>

namespace luna::driver {

struct AotLinkOptions {
    std::string inputPath;
    std::string declaredPackageName;
    std::vector<std::string> linkLibraries;
    std::string runtimeLibrary;
    std::string compiler;
    LunaOptimizationLevel optimizationLevel = LunaOptimizationLevel::O0;
};

class AotLinker {
public:
    static int build(CodeGenerator& codeGenerator, AotLinkOptions options);
};

} // namespace luna::driver
