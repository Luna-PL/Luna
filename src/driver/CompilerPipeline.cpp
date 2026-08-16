#include "driver/CompilerPipeline.h"

#include "moonir/Lowering.h"
#include "moonir/Optimizer.h"
#include "moonir/Sealer.h"
#include "moonir/Verifier.h"

#include <cstdlib>
#include <utility>

namespace luna::driver {

bool CompilerPipeline::compileToMoonIR(
    const CompilerPipelineOptions& options) {
    reset(options);

    auto snapshot = luna::tooling::AnalysisSnapshot::analyzePath(
        options.inputPath);
    mAnalysisSnapshot =
        std::make_unique<luna::tooling::AnalysisSnapshot>(std::move(snapshot));
    if (!mAnalysisSnapshot->success())
        return fail(mAnalysisSnapshot->errors(), mAnalysisSnapshot->errorStage());
    auto* program = mAnalysisSnapshot->program();
    mDeclaredPackageName = program->packageName;
    return lowerAnalyzedProgram(
        options,
        mDeclaredPackageName.empty()
            ? options.inputPath : mDeclaredPackageName);
}

bool CompilerPipeline::compileSourceToMoonIR(
    const std::string& source, const std::string& virtualPath,
    const CompilerPipelineOptions& options) {
    reset(options);

    auto snapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        source, virtualPath);
    mAnalysisSnapshot =
        std::make_unique<luna::tooling::AnalysisSnapshot>(std::move(snapshot));
    if (!mAnalysisSnapshot->success())
        return fail(mAnalysisSnapshot->errors(), mAnalysisSnapshot->errorStage());
    auto* program = mAnalysisSnapshot->program();
    mDeclaredPackageName = program->packageName;
    return lowerAnalyzedProgram(options, virtualPath);
}

void CompilerPipeline::reset(const CompilerPipelineOptions& options) {
    mOptimizationLevel = options.optimizationLevel;
    mModuleName.clear();
    mDeclaredPackageName.clear();
    mCodeGenerator.reset();
    mMoonModule.reset();
    mAnalysisSnapshot.reset();
    mErrors.clear();
    mErrorStage.clear();
}

bool CompilerPipeline::lowerAnalyzedProgram(
    const CompilerPipelineOptions& options, std::string moduleName) {
    mModuleName = std::move(moduleName);
    auto* program = mAnalysisSnapshot->program();

    moon::LunaLowerer lowerer;
    mMoonModule = lowerer.lower(
        *program, *mAnalysisSnapshot->symbolTable(),
        options.reserveKernelRuntime);
    if (!lowerer.errors().empty())
        return fail(lowerer.errors(), "moon-lower");

    moon::Verifier verifier;
    if (!verifier.verify(*mMoonModule))
        return fail(verifier.errors(), "moon-verify");

    // Optional canonical sealing gate (C016 / item 10). When
    // LUNA_SEAL_CANONICAL=1 is set, the Sealer converts structured function
    // bodies into canonical CFGs before optimization. This is an incremental
    // testing path: the structured-body backend remains the default until the
    // canonical path covers the full program surface.
    if (const char* sealEnv = std::getenv("LUNA_SEAL_CANONICAL")) {
        if (sealEnv[0] == '1' && sealEnv[1] == '\0') {
            moon::Sealer sealer;
            if (!sealer.sealFunctionBodies(*mMoonModule)) {
                std::vector<diagnostic::Diagnostic> sealErrors;
                for (const auto& message : sealer.errors()) {
                    diagnostic::Diagnostic diag;
                    diag.phase = "moon-seal";
                    diag.code = diagnostic::errorCode("moon", message);
                    diag.message = message;
                    sealErrors.push_back(std::move(diag));
                }
                return fail(sealErrors, "moon-seal");
            }
            if (!verifier.verify(*mMoonModule))
                return fail(verifier.errors(), "moon-verify");
        }
    }

    moon::OptimizationLevel moonOptimizationLevel =
        moon::OptimizationLevel::None;
    if (options.optimizationLevel == LunaOptimizationLevel::O2)
        moonOptimizationLevel = moon::OptimizationLevel::Standard;
    else if (options.optimizationLevel == LunaOptimizationLevel::O3)
        moonOptimizationLevel = moon::OptimizationLevel::Aggressive;

    moon::Optimizer optimizer;
    if (!optimizer.run(*mMoonModule, {
            moonOptimizationLevel,
            options.aheadOfTime
                ? moon::OptimizationPurpose::AheadOfTime
                : moon::OptimizationPurpose::JustInTime})) {
        return fail(optimizer.errors(), "moon-opt");
    }
    if (!verifier.verify(*mMoonModule))
        return fail(verifier.errors(), "moon-verify");

    return true;
}

bool CompilerPipeline::generateCode(LunaGpuTargetConfig gpuTargets) {
    mErrors.clear();
    mErrorStage.clear();
    mCodeGenerator = std::make_unique<CodeGenerator>(mModuleName);
    mCodeGenerator->setOptimizationLevel(mOptimizationLevel);
    mCodeGenerator->setGpuTargets(std::move(gpuTargets));
    if (!mCodeGenerator->generate(mMoonModule.get()))
        return fail(mCodeGenerator->errors());
    return true;
}

const moon::Module& CompilerPipeline::moonModule() const {
    return *mMoonModule;
}

CodeGenerator& CompilerPipeline::codeGenerator() {
    return *mCodeGenerator;
}

const std::string& CompilerPipeline::declaredPackageName() const {
    return mDeclaredPackageName;
}

const std::vector<diagnostic::Diagnostic>& CompilerPipeline::errors() const {
    return mErrors;
}

const std::string& CompilerPipeline::errorStage() const {
    return mErrorStage;
}

const luna::tooling::AnalysisSnapshot&
CompilerPipeline::analysisSnapshot() const {
    return *mAnalysisSnapshot;
}

bool CompilerPipeline::fail(const std::vector<diagnostic::Diagnostic>& errors,
                            std::string stage) {
    mErrors = errors;
    mErrorStage = std::move(stage);
    return false;
}

} // namespace luna::driver
