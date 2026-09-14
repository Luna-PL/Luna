#include "moonir/MoonIR.h"
#include "moonir/ContainerModel.h"
#include "moonir/ControlFlowBuilder.h"
#include "moonir/Lowering.h"
#include "moonir/Sealer.h"
#include "moonir/Verifier.h"
#include "codegen/CodeGenerator.h"
#include "diagnostics/Diagnostic.h"
#include "tooling/AnalysisSnapshot.h"
#include "moonir_canonical_test_support.h"

#include <algorithm>
#include <iostream>

namespace canonical_test {

int runFunctionSealingTests(SealingTestContext& context) {
    auto& verifier = context.verifier;

    const std::string sealableFunctionSource = R"luna(
package canonical.sealer;

fn increment(value: i32) -> i32 {
    let result = value + 1;
    return result;
}
)luna";
    auto sealableSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        sealableFunctionSource, "<canonical-sealer>");
    if (!sealableSnapshot.success())
        return fail("frontend rejected the canonical sealer source");
    moon::LunaLowerer sealableLowerer;
    auto sealableModule = sealableLowerer.lower(
        *sealableSnapshot.program(), *sealableSnapshot.symbolTable());
    if (!sealableLowerer.errors().empty() ||
        !verifier.verify(*sealableModule))
        return fail("structured sealer input did not verify");
    moon::FunctionDecl* sealableFunction = nullptr;
    for (auto& declaration : sealableModule->declarations) {
        auto* function = dynamic_cast<moon::FunctionDecl*>(
            declaration.get());
        if (function && function->name == "increment") {
            sealableFunction = function;
            break;
        }
    }
    if (!sealableFunction || !sealableFunction->body ||
        sealableFunction->controlFlow)
        return fail("sealer input does not own exactly one construction body");
    moon::Sealer sealer;
    if (!sealer.sealFunctionBodies(*sealableModule)) {
        for (const auto& error : sealer.errors()) std::cerr << error << '\n';
        return fail("atomic function sealing rejected a canonical source body");
    }
    if (sealableFunction->body || !sealableFunction->controlFlow ||
        !verifier.verify(*sealableModule))
        return fail("atomic function sealing did not install one verified CFG body");
    sealableFunction->body = std::make_unique<moon::BlockStmt>();
    if (verifier.verify(*sealableModule))
        return fail("module verifier accepted simultaneous function bodies");
    sealableFunction->body.reset();
    if (!verifier.verify(*sealableModule))
        return fail("restored sealed function module no longer verifies");
    auto sealedParameter = std::find_if(
        sealableFunction->controlFlow->locals.begin(),
        sealableFunction->controlFlow->locals.end(),
        [](const moon::LocalRecord& local) {
            return local.kind == moon::LocalKind::Parameter;
        });
    if (sealedParameter == sealableFunction->controlFlow->locals.end())
        return fail("sealed function lost its canonical parameter");
    sealedParameter->relation = luna::ownership::Relation::SharedBorrow;
    if (verifier.verify(*sealableModule))
        return fail("module verifier accepted a CFG/signature parameter mismatch");
    sealedParameter->relation = luna::ownership::Relation::Owned;
    if (!verifier.verify(*sealableModule))
        return fail("restored canonical function signature no longer verifies");

    const std::string closureSealerSource = R"luna(
fn main() -> i32 {
    let captured = 21;
    let outer = fn() -> i32 {
        let inner = fn() -> i32 {
            return captured * 2;
        };
        return inner();
    };
    return outer();
}
)luna";
    auto closureSealerSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        closureSealerSource, "<closure-sealer>");
    if (!closureSealerSnapshot.success())
        return fail("frontend rejected the closure sealer source");
    moon::LunaLowerer closureSealerLowerer;
    auto closureSealerModule = closureSealerLowerer.lower(
        *closureSealerSnapshot.program(),
        *closureSealerSnapshot.symbolTable());
    moon::Sealer closureSealer;
    if (!closureSealerLowerer.errors().empty() ||
        !closureSealer.sealFunctionBodies(*closureSealerModule) ||
        !verifier.verify(*closureSealerModule)) {
        for (const auto& diagnostic : closureSealerLowerer.errors())
            std::cerr << diagnostic << '\n';
        for (const auto& error : closureSealer.errors())
            std::cerr << error << '\n';
        return fail("closure-bearing function did not survive Sealer cloning");
    }
    const moon::MakeClosureExpr* sealedOuterClosure = nullptr;
    const moon::LambdaExpr* sealedOuterLambda = nullptr;
    const moon::MakeClosureExpr* sealedInnerClosure = nullptr;
    for (const auto& declaration : closureSealerModule->declarations) {
        const auto* function = dynamic_cast<const moon::FunctionDecl*>(
            declaration.get());
        if (!function || function->name != "main" ||
            !function->controlFlow)
            continue;
        for (const auto& block : function->controlFlow->blocks) {
            for (const auto& operation : block.operations) {
                const auto* let = dynamic_cast<const moon::LetStmt*>(
                    operation.get());
                if (!let || !let->initializer) continue;
                if (const auto* closure = dynamic_cast<
                        const moon::MakeClosureExpr*>(
                        let->initializer.get())) {
                    sealedOuterClosure = closure;
                    sealedOuterLambda = closure->lambda.get();
                }
            }
        }
    }
    if (!sealedOuterClosure || !sealedOuterLambda ||
        !sealedOuterLambda->controlFlow)
        return fail("sealed function dropped its outer closure construction");
    for (const auto& block : sealedOuterLambda->controlFlow->blocks) {
        for (const auto& operation : block.operations) {
            const auto* let = dynamic_cast<const moon::LetStmt*>(
                operation.get());
            if (!let || !let->initializer) continue;
            if (const auto* closure = dynamic_cast<
                    const moon::MakeClosureExpr*>(
                    let->initializer.get()))
                sealedInnerClosure = closure;
        }
    }
    if (!sealedInnerClosure || !sealedInnerClosure->lambda ||
        !sealedInnerClosure->lambda->controlFlow)
        return fail("sealed function dropped its nested closure construction");
    bool sealedEnvLoad = false;
    for (const auto& block : sealedInnerClosure->lambda->controlFlow->blocks) {
        for (const auto& operation : block.operations) {
            if (dynamic_cast<const moon::EnvLoadExpr*>(
                    operation.get()))
                sealedEnvLoad = true;
        }
        if (block.terminator.operand &&
            dynamic_cast<const moon::EnvLoadExpr*>(
                block.terminator.operand.get()))
            sealedEnvLoad = true;
        if (auto* binary = dynamic_cast<const moon::BinaryExpr*>(
                block.terminator.operand.get())) {
            if (dynamic_cast<const moon::EnvLoadExpr*>(
                    binary->lhs.get()) ||
                dynamic_cast<const moon::EnvLoadExpr*>(
                    binary->rhs.get()))
                sealedEnvLoad = true;
        }
    }
    if (!sealedEnvLoad)
        return fail("sealed nested closure lost its environment load");
    if (sealedInnerClosure->capturedValues.size() != 1 ||
        !dynamic_cast<const moon::EnvLoadExpr*>(
            sealedInnerClosure->capturedValues.front().get()))
        return fail("sealed nested closure did not rewrite its transitive capture into an environment load");

    const std::string cfgCodegenSource = R"luna(
package canonical.codegen;

fn main() -> i32 {
    let value = 40;
    if value == 40 {
        let value = 41;
        return value + 1;
    }
    return 0;
}
)luna";
    auto cfgCodegenSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        cfgCodegenSource, "<canonical-codegen>");
    if (!cfgCodegenSnapshot.success())
        return fail("frontend rejected the canonical codegen source");
    moon::LunaLowerer cfgCodegenLowerer;
    auto cfgCodegenModule = cfgCodegenLowerer.lower(
        *cfgCodegenSnapshot.program(), *cfgCodegenSnapshot.symbolTable());
    CodeGenerator missingEntryCodegen("missing.jit.entry");
    const auto missingEntryExecution = missingEntryCodegen.jitRun();
    if (missingEntryExecution.executed || missingEntryExecution.error.empty())
        return fail("JIT lookup failure was confused with a program exit code");
    CodeGenerator structuredCodegen("structured.codegen.rejected");
    if (structuredCodegen.generate(cfgCodegenModule.get()))
        return fail("LLVM backend accepted a structured executable body");
    const bool diagnosedStructuredBody = std::any_of(
        structuredCodegen.errors().begin(), structuredCodegen.errors().end(),
        [](const diagnostic::Diagnostic& diagnostic) {
            return diagnostic.message.find("exclusive canonical CFG body") !=
                   std::string::npos;
        });
    if (!diagnosedStructuredBody)
        return fail("LLVM backend rejected a structured body without the canonical-boundary diagnostic");
    moon::Sealer cfgCodegenSealer;
    const bool cfgCodegenSealed =
        cfgCodegenSealer.sealFunctionBodies(*cfgCodegenModule);
    const bool cfgCodegenVerified =
        cfgCodegenSealed && verifier.verify(*cfgCodegenModule);
    if (!cfgCodegenLowerer.errors().empty() ||
        !cfgCodegenSealed || !cfgCodegenVerified) {
        for (const auto& diagnostic : cfgCodegenLowerer.errors())
            std::cerr << diagnostic << '\n';
        for (const auto& error : cfgCodegenSealer.errors())
            std::cerr << error << '\n';
        if (cfgCodegenSealed)
            for (const auto& diagnostic : verifier.errors())
                std::cerr << diagnostic << '\n';
        return fail("canonical codegen fixture did not seal and verify");
    }
    CodeGenerator cfgCodegen("canonical.codegen");
    if (!cfgCodegen.generate(cfgCodegenModule.get())) {
        for (const auto& diagnostic : cfgCodegen.errors())
            std::cerr << diagnostic << '\n';
        return fail("LLVM backend rejected the initial canonical CFG slice");
    }
    const auto cfgExecution = cfgCodegen.jitRun();
    if (!cfgExecution.executed || cfgExecution.exitCode != 42)
        return fail("canonical CFG JIT did not preserve branch/local semantics");

    const std::string cfgSwitchCodegenSource = R"luna(
package canonical.switch_codegen;

enum Choice {
    None;
    Some(i32);
}

fn inspect_choice(value: Choice) -> i32 {
    match value {
        Choice::None => {
            return 0;
        },
        Choice::Some(number) => {
            return number;
        }
    }
}

fn inspect_result(value: Result<i32, i32>) -> i32 {
    match value {
        Ok(number) => {
            return number;
        },
        Err(error) => {
            return error;
        }
    }
}

fn main() -> i32 {
    return inspect_choice(Choice::Some(19))
        + inspect_result(Ok::<i32, i32>(23));
}
)luna";
    auto cfgSwitchSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            cfgSwitchCodegenSource, "<canonical-switch-codegen>");
    if (!cfgSwitchSnapshot.success())
        return fail("frontend rejected the canonical switch codegen source");
    moon::LunaLowerer cfgSwitchLowerer;
    auto cfgSwitchModule = cfgSwitchLowerer.lower(
        *cfgSwitchSnapshot.program(), *cfgSwitchSnapshot.symbolTable());
    moon::Sealer cfgSwitchSealer;
    const bool cfgSwitchSealed =
        cfgSwitchSealer.sealFunctionBodies(*cfgSwitchModule);
    const bool cfgSwitchVerified =
        cfgSwitchSealed && verifier.verify(*cfgSwitchModule);
    if (!cfgSwitchLowerer.errors().empty() ||
        !cfgSwitchSealed || !cfgSwitchVerified) {
        for (const auto& diagnostic : cfgSwitchLowerer.errors())
            std::cerr << diagnostic << '\n';
        for (const auto& error : cfgSwitchSealer.errors())
            std::cerr << error << '\n';
        if (cfgSwitchSealed)
            for (const auto& diagnostic : verifier.errors())
                std::cerr << diagnostic << '\n';
        return fail("canonical switch codegen fixture did not seal and verify");
    }
    size_t switchCount = 0;
    size_t bindingCount = 0;
    for (const auto& declaration : cfgSwitchModule->declarations) {
        const auto* function = dynamic_cast<const moon::FunctionDecl*>(
            declaration.get());
        if (!function || !function->controlFlow) continue;
        for (const auto& block : function->controlFlow->blocks) {
            if (block.terminator.kind != moon::TerminatorKind::Switch)
                continue;
            ++switchCount;
            for (const auto& item : block.terminator.cases)
                bindingCount += item.bindings.size();
        }
    }
    if (switchCount != 2 || bindingCount != 3)
        return fail("canonical switch fixture lost enum/Result pattern bindings");
    CodeGenerator cfgSwitchCodegen("canonical.switch_codegen");
    if (!cfgSwitchCodegen.generate(cfgSwitchModule.get())) {
        for (const auto& diagnostic : cfgSwitchCodegen.errors())
            std::cerr << diagnostic << '\n';
        return fail("LLVM backend rejected canonical enum/Result switches");
    }
    const auto cfgSwitchExecution = cfgSwitchCodegen.jitRun();
    if (!cfgSwitchExecution.executed || cfgSwitchExecution.exitCode != 42)
        return fail("canonical switch JIT did not preserve payload bindings");

    const std::string cfgCleanupCodegenSource = R"luna(
package canonical.cleanup_codegen;

fn release(affine value: string) -> i32 {
    return 7;
}

fn branch_release(flag: bool) -> i32 {
    if flag {
        let value = "temporary";
        value;
    }
    return 3;
}
)luna";
    auto cfgCleanupSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            cfgCleanupCodegenSource, "<canonical-cleanup-codegen>");
    if (!cfgCleanupSnapshot.success())
        return fail("frontend rejected the canonical cleanup codegen source");
    moon::LunaLowerer cfgCleanupLowerer;
    auto cfgCleanupModule = cfgCleanupLowerer.lower(
        *cfgCleanupSnapshot.program(), *cfgCleanupSnapshot.symbolTable());
    moon::Sealer cfgCleanupSealer;
    if (!cfgCleanupLowerer.errors().empty() ||
        !cfgCleanupSealer.sealFunctionBodies(*cfgCleanupModule) ||
        !verifier.verify(*cfgCleanupModule))
        return fail("canonical cleanup codegen fixture did not seal and verify");
    const moon::FunctionDecl* cleanupFunction = nullptr;
    const moon::FunctionDecl* branchCleanupFunction = nullptr;
    for (const auto& declaration : cfgCleanupModule->declarations) {
        const auto* function = dynamic_cast<const moon::FunctionDecl*>(
            declaration.get());
        if (function && function->name == "release") {
            cleanupFunction = function;
        } else if (function && function->name == "branch_release") {
            branchCleanupFunction = function;
        }
    }
    bool hasReturnCleanup = false;
    bool hasBranchCleanup = false;
    if (cleanupFunction && cleanupFunction->controlFlow)
        for (const auto& block : cleanupFunction->controlFlow->blocks)
            hasReturnCleanup = hasReturnCleanup ||
                (block.terminator.kind == moon::TerminatorKind::Return &&
                 !block.terminator.exitCleanups.empty());
    if (branchCleanupFunction && branchCleanupFunction->controlFlow)
        for (const auto& block : branchCleanupFunction->controlFlow->blocks)
            hasBranchCleanup = hasBranchCleanup ||
                !block.terminator.primary.cleanups.empty() ||
                !block.terminator.secondary.cleanups.empty();
    if (!hasReturnCleanup || !hasBranchCleanup)
        return fail("cleanup codegen fixture has no canonical cleanup edge");
    CodeGenerator cfgCleanupCodegen("canonical.cleanup_codegen");
    if (!cfgCleanupCodegen.generate(cfgCleanupModule.get())) {
        for (const auto& diagnostic : cfgCleanupCodegen.errors())
            std::cerr << diagnostic << '\n';
        return fail("LLVM backend rejected a canonical root cleanup edge");
    }


    return 0;
}

} // namespace canonical_test
