#include "driver/CompilerPipeline.h"

#include "lexer/Lexer.h"
#include "moonir/Lowering.h"
#include "moonir/Optimizer.h"
#include "moonir/Verifier.h"
#include "package/Package.h"
#include "parser/Parser.h"
#include "sema/OwnershipChecker.h"
#include "sema/SemanticAnalyzer.h"
#include "sema/TraitChecker.h"

#include <utility>

namespace luna::driver {

bool CompilerPipeline::compileToMoonIR(
    const CompilerPipelineOptions& options) {
    reset(options);

    LoadedPackage loaded;
    std::vector<diagnostic::Diagnostic> packageErrors;
    if (!PackageLoader::load(options.inputPath, loaded, packageErrors))
        return fail(packageErrors);
    auto* program = loaded.program.get();
    mDeclaredPackageName = program->packageName;
    return compileProgram(
        program, options,
        mDeclaredPackageName.empty()
            ? options.inputPath : mDeclaredPackageName);
}

bool CompilerPipeline::compileSourceToMoonIR(
    const std::string& source, const std::string& virtualPath,
    const CompilerPipelineOptions& options) {
    reset(options);

    Lexer lexer(source, virtualPath);
    auto tokens = lexer.tokenize();
    if (!lexer.errors().empty()) return fail(lexer.errors(), "lexer");

    Parser parser(std::move(tokens), virtualPath, source);
    auto program = parser.parse();
    if (!parser.errors().empty()) return fail(parser.errors(), "parser");
    mDeclaredPackageName = program->packageName;
    return compileProgram(program.get(), options, virtualPath);
}

void CompilerPipeline::reset(const CompilerPipelineOptions& options) {
    mOptimizationLevel = options.optimizationLevel;
    mModuleName.clear();
    mDeclaredPackageName.clear();
    mMoonModule.reset();
    mCodeGenerator.reset();
    mErrors.clear();
    mErrorStage.clear();
}

bool CompilerPipeline::compileProgram(
    Program* program, const CompilerPipelineOptions& options,
    std::string moduleName) {
    mModuleName = std::move(moduleName);

    SemanticAnalyzer sema;
    if (!sema.analyze(program)) return fail(sema.errors());

    TraitChecker traits;
    if (!traits.check(program)) return fail(traits.errors());

    OwnershipChecker owner;
    if (!owner.check(program, sema.symTable())) return fail(owner.errors());

    moon::LunaLowerer lowerer;
    mMoonModule = lowerer.lower(
        *program, sema.symTable(), options.reserveKernelRuntime);
    if (!lowerer.errors().empty())
        return fail(lowerer.errors(), "moon-lower");

    moon::Verifier verifier;
    if (!verifier.verify(*mMoonModule))
        return fail(verifier.errors(), "moon-verify");

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

bool CompilerPipeline::fail(const std::vector<diagnostic::Diagnostic>& errors,
                            std::string stage) {
    mErrors = errors;
    mErrorStage = std::move(stage);
    return false;
}

} // namespace luna::driver
