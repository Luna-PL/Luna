#pragma once

#include "codegen/CodeGenerator.h"

#include <memory>
#include <string>
#include <vector>

namespace luna::driver {

struct CompilerPipelineOptions {
    std::string inputPath;
    LunaOptimizationLevel optimizationLevel = LunaOptimizationLevel::O0;
    bool reserveKernelRuntime = false;
    bool aheadOfTime = false;
};

class CompilerPipeline {
public:
    bool compileToMoonIR(const CompilerPipelineOptions& options);
    bool generateCode(LunaGpuTargetConfig gpuTargets);

    const moon::Module& moonModule() const;
    CodeGenerator& codeGenerator();
    const std::string& declaredPackageName() const;
    const std::vector<std::string>& errors() const;
    const std::string& errorStage() const;

private:
    bool fail(const std::vector<std::string>& errors,
              std::string stage = {});

    LunaOptimizationLevel mOptimizationLevel = LunaOptimizationLevel::O0;
    std::string mModuleName;
    std::string mDeclaredPackageName;
    std::unique_ptr<moon::Module> mMoonModule;
    std::unique_ptr<CodeGenerator> mCodeGenerator;
    std::vector<std::string> mErrors;
    std::string mErrorStage;
};

} // namespace luna::driver
