#include "driver/CompilerPipeline.h"

#include "moonir/Lowering.h"
#include "moonir/Optimizer.h"
#include "moonir/Sealer.h"
#include "moonir/Verifier.h"

#include <chrono>
#include <utility>

namespace luna::driver {
namespace {

uint64_t elapsedMicroseconds(std::chrono::steady_clock::time_point start) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count());
}

} // namespace

bool CompilerPipeline::compileToMoonIR(const CompilerPipelineOptions& options) {
    reset(options);

    auto snapshot = luna::tooling::AnalysisSnapshot::analyzePath(options.inputPath);
    mAnalysisSnapshot = std::make_unique<luna::tooling::AnalysisSnapshot>(std::move(snapshot));
    mTimings.analysis = mAnalysisSnapshot->timings();
    if (!mAnalysisSnapshot->success())
        return fail(mAnalysisSnapshot->errors(), mAnalysisSnapshot->errorStage());
    auto* program = mAnalysisSnapshot->program();
    mDeclaredPackageName = program->packageName;
    return lowerAnalyzedProgram(options, mDeclaredPackageName.empty() ? options.inputPath
                                                                      : mDeclaredPackageName);
}

bool CompilerPipeline::compileSourceToMoonIR(const std::string& source,
                                             const std::string& virtualPath,
                                             const CompilerPipelineOptions& options) {
    reset(options);

    auto snapshot = luna::tooling::AnalysisSnapshot::analyzeSource(source, virtualPath);
    mAnalysisSnapshot = std::make_unique<luna::tooling::AnalysisSnapshot>(std::move(snapshot));
    mTimings.analysis = mAnalysisSnapshot->timings();
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
    mTimings = {};
}

bool CompilerPipeline::lowerAnalyzedProgram(const CompilerPipelineOptions& options,
                                            std::string moduleName) {
    mModuleName = std::move(moduleName);
    auto* program = mAnalysisSnapshot->program();

    const auto loweringStart = std::chrono::steady_clock::now();
    moon::LunaLowerer lowerer;
    mMoonModule =
        lowerer.lower(*program, *mAnalysisSnapshot->symbolTable(), options.reserveKernelRuntime);
    mTimings.loweringMicroseconds = elapsedMicroseconds(loweringStart);
    if (!lowerer.errors().empty()) return fail(lowerer.errors(), "moon-lower");

    moon::Verifier verifier;
    auto verificationStart = std::chrono::steady_clock::now();
    const bool initialVerificationSucceeded = verifier.verify(*mMoonModule);
    mTimings.verificationMicroseconds += elapsedMicroseconds(verificationStart);
    if (!initialVerificationSucceeded) return fail(verifier.errors(), "moon-verify");

    // Canonical CFG is the sole executable function-body representation.
    // Sealing is atomic: on failure no function body is partially consumed.
    const auto sealingStart = std::chrono::steady_clock::now();
    moon::Sealer sealer;
    const bool sealingSucceeded = sealer.sealFunctionBodies(*mMoonModule);
    mTimings.sealingMicroseconds = elapsedMicroseconds(sealingStart);
    if (!sealingSucceeded) {
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
    // Sealer verifies every newly built canonical CFG before committing it,
    // while the pre-seal pass above covers functions that were canonical on
    // entry. Avoid an unchanged whole-module pass here; the post-optimizer
    // verification below remains the final mandatory safety boundary.

    moon::OptimizationLevel moonOptimizationLevel = moon::OptimizationLevel::None;
    if (options.optimizationLevel == LunaOptimizationLevel::O2)
        moonOptimizationLevel = moon::OptimizationLevel::Standard;
    else if (options.optimizationLevel == LunaOptimizationLevel::O3)
        moonOptimizationLevel = moon::OptimizationLevel::Aggressive;

    moon::Optimizer optimizer;
    const auto optimizationStart = std::chrono::steady_clock::now();
    const bool optimizationSucceeded =
        optimizer.run(*mMoonModule, {moonOptimizationLevel,
                                     options.aheadOfTime ? moon::OptimizationPurpose::AheadOfTime
                                                         : moon::OptimizationPurpose::JustInTime});
    mTimings.optimizationMicroseconds = elapsedMicroseconds(optimizationStart);
    if (!optimizationSucceeded) { return fail(optimizer.errors(), "moon-opt"); }
    verificationStart = std::chrono::steady_clock::now();
    const bool optimizedVerificationSucceeded = verifier.verify(*mMoonModule);
    mTimings.verificationMicroseconds += elapsedMicroseconds(verificationStart);
    if (!optimizedVerificationSucceeded) return fail(verifier.errors(), "moon-verify");

    return true;
}

bool CompilerPipeline::generateCode(LunaGpuTargetConfig gpuTargets) {
    mErrors.clear();
    mErrorStage.clear();
    mCodeGenerator = std::make_unique<CodeGenerator>(mModuleName);
    mCodeGenerator->setOptimizationLevel(mOptimizationLevel);
    mCodeGenerator->setGpuTargets(std::move(gpuTargets));
    const auto codegenStart = std::chrono::steady_clock::now();
    const bool codegenSucceeded = mCodeGenerator->generate(mMoonModule.get());
    mTimings.codegenMicroseconds = elapsedMicroseconds(codegenStart);
    if (!codegenSucceeded) return fail(mCodeGenerator->errors());
    return true;
}

const moon::Module& CompilerPipeline::moonModule() const { return *mMoonModule; }

CodeGenerator& CompilerPipeline::codeGenerator() { return *mCodeGenerator; }

const std::string& CompilerPipeline::declaredPackageName() const { return mDeclaredPackageName; }

const std::vector<diagnostic::Diagnostic>& CompilerPipeline::errors() const { return mErrors; }

const std::string& CompilerPipeline::errorStage() const { return mErrorStage; }

const luna::tooling::AnalysisSnapshot& CompilerPipeline::analysisSnapshot() const {
    return *mAnalysisSnapshot;
}

bool CompilerPipeline::fail(const std::vector<diagnostic::Diagnostic>& errors, std::string stage) {
    mErrors = errors;
    mErrorStage = std::move(stage);
    return false;
}

} // namespace luna::driver
