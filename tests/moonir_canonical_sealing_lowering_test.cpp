#include "moonir/MoonIR.h"
#include "moonir/ContainerModel.h"
#include "moonir/ControlFlowBuilder.h"
#include "moonir/Lowering.h"
#include "moonir/Sealer.h"
#include "moonir/Verifier.h"
#include "codegen/CodeGenerator.h"
#include "diagnostics/Diagnostic.h"
#include "runtime/RuntimeDescriptor.h"
#include "runtime/RuntimeFragment.h"
#include "tooling/AnalysisSnapshot.h"
#include "moonir_canonical_test_support.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace canonical_test {

int runLoweredCompositionTests(SealingTestContext& context) {
    auto& cfgVerifier = context.cfgVerifier;
    auto& verifier = context.verifier;
    auto& reverse = context.reverseModule;
    const auto shortId = context.shortIteratorType;

    // Sema is not the only trust boundary: a structured input may be forged
    // after source analysis. The CFG bridge must reject cyclic static body
    // expansion itself rather than exhausting the compiler's stack.
    auto recursionSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        R"luna(
package canonical.static_recursion;
slot hook();
fragment finite() for hook { resume; }
fn main() -> i32 {
    apply finite { hook() {} }
    return 0;
}
)luna", "<canonical-static-recursion>");
    if (!recursionSnapshot.success())
        return fail("frontend rejected finite static recursion fixture");
    moon::LunaLowerer recursionLowerer;
    auto recursionModule = recursionLowerer.lower(
        *recursionSnapshot.program(), *recursionSnapshot.symbolTable());
    if (!recursionModule || !recursionLowerer.errors().empty())
        return fail("finite static recursion fixture did not lower");
    moon::FunctionDecl* recursionMain = nullptr;
    moon::FragmentDecl* recursionFragment = nullptr;
    moon::SlotDecl* recursionSlot = nullptr;
    for (auto& declaration : recursionModule->declarations) {
        if (auto* function = dynamic_cast<moon::FunctionDecl*>(declaration.get());
            function && function->name == "main")
            recursionMain = function;
        if (auto* fragment = dynamic_cast<moon::FragmentDecl*>(declaration.get()))
            recursionFragment = fragment;
        if (auto* slot = dynamic_cast<moon::SlotDecl*>(declaration.get()))
            recursionSlot = slot;
    }
    if (!recursionMain || !recursionFragment || !recursionSlot)
        return fail("static recursion fixture lost its declarations");
    auto forgedInvocation = std::make_unique<moon::SlotInvokeStmt>();
    forgedInvocation->name = recursionSlot->name;
    forgedInvocation->slotRef = recursionFragment->targetSlot;
    forgedInvocation->structuralType = recursionSlot->structuralType;
    forgedInvocation->continuation = std::make_unique<moon::BlockStmt>();
    recursionFragment->body->stmts.insert(
        recursionFragment->body->stmts.begin(), std::move(forgedInvocation));
    moon::ControlFlowBuilder recursionBuilder;
    auto recursiveCfg = recursionBuilder.build(
        *recursionMain->body, recursionMain->params,
        moon::RegionKind::Function, *recursionModule);
    if (recursiveCfg || !std::any_of(
            recursionBuilder.errors().begin(), recursionBuilder.errors().end(),
            [](const std::string& error) {
                return error.find("recursive static fragment composition") !=
                    std::string::npos;
            }))
        return fail("CFG construction accepted forged recursive static composition");

    auto overrideSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        R"luna(
package canonical.nested_override;
slot hook();
fragment replacement() for hook { resume; print(2); }
fragment wrapper() for hook {
    apply replacement { hook() { print(1); } }
    resume;
}
fn main() -> i32 {
    apply wrapper { hook() {} }
    return 0;
}
)luna", "<canonical-nested-override>");
    if (!overrideSnapshot.success())
        return fail("frontend rejected finite nested Fragment override");
    moon::LunaLowerer overrideLowerer;
    auto overrideModule = overrideLowerer.lower(
        *overrideSnapshot.program(), *overrideSnapshot.symbolTable());
    if (!overrideModule || !overrideLowerer.errors().empty())
        return fail("finite nested Fragment override did not lower");
    moon::FunctionDecl* nestedMain = nullptr;
    for (auto& declaration : overrideModule->declarations) {
        auto* function = dynamic_cast<moon::FunctionDecl*>(declaration.get());
        if (function && function->name == "main") nestedMain = function;
    }
    if (!nestedMain || !nestedMain->body)
        return fail("nested Fragment override lost main");
    moon::ControlFlowBuilder nestedBuilder;
    auto nestedCfg = nestedBuilder.build(
        *nestedMain->body, nestedMain->params,
        moon::RegionKind::Function, *overrideModule);
    if (!nestedCfg || !cfgVerifier.verify(*nestedCfg, *overrideModule))
        return fail("valid nested Fragment entry failed CFG verification");
    const auto enclosingFragment = [&](moon::RegionId start)
        -> const moon::RegionRecord* {
        for (const auto* region = nestedCfg->findRegion(start); region;
             region = nestedCfg->findRegion(region->parent))
            if (region->kind == moon::RegionKind::Fragment) return region;
        return nullptr;
    };
    bool checkedNestedEntry = false;
    for (auto& block : nestedCfg->blocks) {
        if (block.terminator.kind != moon::TerminatorKind::Jump) continue;
        const auto* sourceFragment = enclosingFragment(block.region);
        const auto* target = nestedCfg->findBlock(block.terminator.primary.target);
        const auto* targetFragment = target
            ? enclosingFragment(target->region) : nullptr;
        if (!sourceFragment || !targetFragment ||
            sourceFragment->id == targetFragment->id ||
            enclosingFragment(targetFragment->parent) != sourceFragment ||
            target->id != targetFragment->entry) continue;
        const auto nonEntry = std::find_if(
            nestedCfg->blocks.begin(), nestedCfg->blocks.end(),
            [&](const moon::BasicBlock& candidate) {
                return candidate.region == targetFragment->id &&
                    candidate.id != targetFragment->entry;
            });
        if (nonEntry == nestedCfg->blocks.end())
            return fail("nested Fragment fixture has no non-entry block");
        const auto savedTarget = block.terminator.primary.target;
        block.terminator.primary.target = nonEntry->id;
        if (cfgVerifier.verify(*nestedCfg, *overrideModule) ||
            !std::any_of(cfgVerifier.errors().begin(), cfgVerifier.errors().end(),
                [](const diagnostic::Diagnostic& error) {
                    return error.message.find("jump escapes a fragment through a non-exit edge") !=
                        std::string::npos;
                }))
            return fail("CFG verification allowed a jump into a nested Fragment non-entry");
        block.terminator.primary.target = savedTarget;
        checkedNestedEntry = true;
        break;
    }
    if (!checkedNestedEntry || !cfgVerifier.verify(*nestedCfg, *overrideModule))
        return fail("nested Fragment entry guard did not recover after restoration");

    const std::string loweredCompositionSource = R"luna(
package canonical.integration;

slot hook(value: i32);
slot captured();

fragment passthrough(value: i32) for hook {
    resume;
}

fragment lexical_capture[outer: i32] for captured {
    outer;
    resume;
}

fn main() -> i32 {
    let outer = 7;
    apply passthrough {
        hook(outer) {
            outer;
        }
    }
    apply lexical_capture[outer] {
        captured() {
            outer;
        }
        captured() {
            outer;
        }
    }
    return 0;
}
)luna";
    auto compositionSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            loweredCompositionSource, "<canonical-composition>");
    if (!compositionSnapshot.success()) {
        for (const auto& diagnostic : compositionSnapshot.errors())
            std::cerr << diagnostic << '\n';
        return fail("frontend rejected canonical fragment integration source");
    }
    moon::LunaLowerer integrationLowerer;
    auto integrationModule = integrationLowerer.lower(
        *compositionSnapshot.program(), *compositionSnapshot.symbolTable());
    if (!integrationLowerer.errors().empty()) {
        for (const auto& diagnostic : integrationLowerer.errors())
            std::cerr << diagnostic << '\n';
        return fail("MoonIR lowering rejected canonical fragment source");
    }
    if (!verifier.verify(*integrationModule)) {
        for (const auto& diagnostic : verifier.errors())
            std::cerr << diagnostic << '\n';
        return fail("lowered fragment module failed structured verification");
    }
    moon::FunctionDecl* integrationMain = nullptr;
    const moon::FragmentDecl* integrationFragment = nullptr;
    const moon::FragmentDecl* integrationCaptureFragment = nullptr;
    for (auto& declaration : integrationModule->declarations) {
        if (auto* function = dynamic_cast<moon::FunctionDecl*>(
                declaration.get());
            function && function->name == "main")
            integrationMain = function;
        if (auto* fragment = dynamic_cast<moon::FragmentDecl*>(
                declaration.get());
            fragment && fragment->name == "passthrough")
            integrationFragment = fragment;
        if (auto* fragment = dynamic_cast<moon::FragmentDecl*>(
                declaration.get());
            fragment && fragment->name == "lexical_capture")
            integrationCaptureFragment = fragment;
    }
    if (!integrationMain || !integrationMain->body ||
        !integrationFragment || !integrationFragment->body ||
        !integrationCaptureFragment || !integrationCaptureFragment->body)
        return fail("lowered integration module lost main or its fragment");
    const auto* integrationFragmentBody = integrationFragment->body.get();
    const auto* integrationCaptureBody =
        integrationCaptureFragment->body.get();
    moon::ControlFlowBuilder integrationBuilder;
    auto integrationCfg = integrationBuilder.build(
        std::move(integrationMain->body), integrationMain->params,
        moon::RegionKind::Function, *integrationModule);
    if (!integrationCfg) {
        for (const auto& error : integrationBuilder.errors())
            std::cerr << error << '\n';
        return fail("lowered fragment did not enter canonical CFG construction");
    }
    if (!cfgVerifier.verify(*integrationCfg, *integrationModule)) {
        for (const auto& diagnostic : cfgVerifier.errors())
            std::cerr << diagnostic << '\n';
        return fail("lowered fragment failed canonical CFG verification");
    }
    size_t integrationApplyRegions = 0;
    size_t integrationFragmentRegions = 0;
    size_t integrationContinuationRegions = 0;
    size_t integrationResumeEdges = 0;
    size_t fragmentOuterCaptures = 0;
    size_t continuationOuterCaptures = 0;
    size_t environmentStorageLocals = 0;
    bool environmentEscapedApply = false;
    for (const auto& composedRegion : integrationCfg->regions) {
        integrationApplyRegions +=
            composedRegion.kind == moon::RegionKind::Apply;
        integrationFragmentRegions +=
            composedRegion.kind == moon::RegionKind::Fragment;
        integrationContinuationRegions +=
            composedRegion.kind == moon::RegionKind::Continuation;
    }
    for (const auto& local : integrationCfg->locals)
        environmentStorageLocals +=
            local.name.rfind("$fragment.environment.", 0) == 0;
    for (auto& block : integrationCfg->blocks) {
        integrationResumeEdges +=
            block.terminator.kind == moon::TerminatorKind::Resume;
        for (auto& operation : block.operations) {
            auto* effect = dynamic_cast<moon::ExprStmt*>(operation.get());
            auto* identifier = effect
                ? dynamic_cast<moon::IdentifierExpr*>(effect->expr.get())
                : nullptr;
            if (!identifier || identifier->name != "outer") continue;
            const auto kind =
                integrationCfg->regions[block.region.value].kind;
            fragmentOuterCaptures += kind == moon::RegionKind::Fragment;
            continuationOuterCaptures +=
                kind == moon::RegionKind::Continuation;
            if (identifier->local.empty()) {
                environmentEscapedApply = true;
                continue;
            }
            const auto& local = integrationCfg->locals[identifier->local.value];
            const auto localRegion = integrationCfg->scopes[local.scope.value].region;
            if (kind == moon::RegionKind::Fragment) {
                if (local.name.rfind("$fragment.environment.", 0) != 0 ||
                    integrationCfg->regions[localRegion.value].kind !=
                        moon::RegionKind::Apply)
                    environmentEscapedApply = true;
            } else if (kind == moon::RegionKind::Continuation &&
                       (local.name != "outer" || local.scope != integrationCfg->rootScope)) {
                environmentEscapedApply = true;
            }
        }
    }
    if (integrationApplyRegions != 2 ||
        integrationFragmentRegions != 3 ||
        integrationContinuationRegions != 3 ||
        integrationResumeEdges != 3 ||
        fragmentOuterCaptures != 2 ||
        continuationOuterCaptures != 3 ||
        environmentStorageLocals != 1 || environmentEscapedApply ||
        integrationFragment->body.get() != integrationFragmentBody ||
        integrationCaptureFragment->body.get() != integrationCaptureBody)
        return fail("frontend-to-CFG composition lost its fragment, environment, or construction body");

    const std::string loweredRuntimeCompositionSource = R"luna(
package canonical.runtime_boundary;

slot pipeline(value: i32);

fragment trace(value: i32) for pipeline {
    value;
    resume;
}

fn stable_entry() -> i32 {
    return 1;
}

fn dynamic_entry() -> i32 {
    apply trace {
        pipeline(1) {
            2;
        }
    }
    return 0;
}
)luna";
    auto runtimeCompositionSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            loweredRuntimeCompositionSource,
            "<canonical-runtime-composition>");
    if (!runtimeCompositionSnapshot.success()) {
        for (const auto& diagnostic : runtimeCompositionSnapshot.errors())
            std::cerr << diagnostic << '\n';
        return fail("frontend rejected the canonical runtime-boundary source");
    }
    moon::LunaLowerer runtimeIntegrationLowerer;
    auto runtimeIntegrationModule = runtimeIntegrationLowerer.lower(
        *runtimeCompositionSnapshot.program(),
        *runtimeCompositionSnapshot.symbolTable());
    if (!runtimeIntegrationLowerer.errors().empty()) {
        for (const auto& diagnostic : runtimeIntegrationLowerer.errors())
            std::cerr << diagnostic << '\n';
        return fail("MoonIR lowering rejected the runtime-boundary source");
    }
    if (!verifier.verify(*runtimeIntegrationModule)) {
        for (const auto& diagnostic : verifier.errors())
            std::cerr << diagnostic << '\n';
        return fail("runtime-boundary module failed structured verification");
    }
    moon::FunctionDecl* dynamicEntry = nullptr;
    moon::FunctionDecl* stableEntry = nullptr;
    for (auto& declaration : runtimeIntegrationModule->declarations) {
        auto* function = dynamic_cast<moon::FunctionDecl*>(declaration.get());
        if (function && function->name == "dynamic_entry") {
            dynamicEntry = function;
        } else if (function && function->name == "stable_entry") {
            stableEntry = function;
        }
    }
    if (!dynamicEntry || !dynamicEntry->body ||
        !stableEntry || !stableEntry->body)
        return fail("runtime-boundary module lost its entry body");
    moon::Sealer runtimeBoundarySealer;
    if (!runtimeBoundarySealer.sealFunctionBodies(
            *runtimeIntegrationModule)) {
        for (const auto& diagnostic : runtimeBoundarySealer.errors())
            std::cerr << diagnostic << '\n';
        return fail("canonical function sealing rejected a linked fragment");
    }
    if (dynamicEntry->body || !dynamicEntry->controlFlow ||
        stableEntry->body || !stableEntry->controlFlow)
        return fail("fragment sealing did not atomically consume the function set");
    if (!verifier.verify(*runtimeIntegrationModule))
        return fail("sealed fragment module failed canonical verification");
    size_t runtimeFragmentRegions = 0;
    size_t runtimeContinuationRegions = 0;
    size_t runtimeResumeEdges = 0;
    for (const auto& region : dynamicEntry->controlFlow->regions) {
        runtimeFragmentRegions += region.kind == moon::RegionKind::Fragment;
        runtimeContinuationRegions +=
            region.kind == moon::RegionKind::Continuation;
    }
    for (const auto& block : dynamicEntry->controlFlow->blocks)
        runtimeResumeEdges +=
            block.terminator.kind == moon::TerminatorKind::Resume;
    if (runtimeFragmentRegions != 1 ||
        runtimeContinuationRegions != 1 || runtimeResumeEdges != 1)
        return fail("fragment lost its Fragment/Continuation/resume CFG");

    // Positive: an ordinary lexical fragment apply seals into a verified
    // canonical CFG with one Fragment region and one shared continuation.
    const std::string staticInterceptorSource = R"luna(
package canonical.dynamic_interceptor;

slot pipeline(value: i32);

fragment trace(value: i32) for pipeline {
    print(value + 1);
    resume;
}

fragment audit(value: i32) for pipeline {
    print(value + 2);
    resume;
}

fn main() -> i32 {
    apply trace {
        pipeline(41) {
            print(42);
        }
    }
    return 0;
}
)luna";
    auto interceptorSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            staticInterceptorSource,
            "<canonical-dynamic-interceptor>");
    if (!interceptorSnapshot.success()) {
        for (const auto& diagnostic : interceptorSnapshot.errors())
            std::cerr << diagnostic << '\n';
        return fail("frontend rejected the static fragment source");
    }
    moon::LunaLowerer interceptorLowerer;
    auto interceptorModule = interceptorLowerer.lower(
        *interceptorSnapshot.program(),
        *interceptorSnapshot.symbolTable());
    if (!interceptorLowerer.errors().empty()) {
        for (const auto& diagnostic : interceptorLowerer.errors())
            std::cerr << diagnostic << '\n';
        return fail("MoonIR lowering rejected the static fragment source");
    }
    if (!verifier.verify(*interceptorModule)) {
        for (const auto& diagnostic : verifier.errors())
            std::cerr << diagnostic << '\n';
        return fail("static fragment module failed structured verification");
    }
    moon::FunctionDecl* interceptorMain = nullptr;
    for (auto& declaration : interceptorModule->declarations) {
        auto* function = dynamic_cast<moon::FunctionDecl*>(declaration.get());
        if (function && function->name == "main")
            interceptorMain = function;
    }
    if (!interceptorMain || !interceptorMain->body)
        return fail("static fragment module lost its main body");
    moon::Sealer interceptorSealer;
    if (!interceptorSealer.sealFunctionBodies(*interceptorModule)) {
        for (const auto& diagnostic : interceptorSealer.errors())
            std::cerr << diagnostic << '\n';
        return fail("canonical sealing rejected a valid static fragment apply");
    }
    if (!interceptorMain->controlFlow)
        return fail("static fragment main was not sealed to a canonical CFG");
    size_t interceptorFragmentRegions = 0;
    size_t interceptorContinuationRegions = 0;
    for (const auto& region : interceptorMain->controlFlow->regions) {
        if (region.kind == moon::RegionKind::Fragment) ++interceptorFragmentRegions;
        if (region.kind == moon::RegionKind::Continuation) ++interceptorContinuationRegions;
    }
    if (interceptorFragmentRegions != 1)
        return fail("static fragment apply did not materialize one Fragment region");
    if (interceptorContinuationRegions != 1)
        return fail("static fragment apply did not materialize one shared Continuation region");

    // An unbound exported Slot remains a nominal runtime dispatch boundary.
    // The same-shaped private Slot remains a compile-time identity so static
    // programs do not acquire an execution-context dependency.
    const std::string runtimeSlotSource = R"luna(
package canonical.runtime_slot;

export slot published(value: i32);
slot private_hook(value: i32);

export fragment local_impl(value) for published {
    resume;
}

fn dynamic_path() -> i32 {
    let captured = 40;
    published(41) {
        captured += 2;
    }
    return captured;
}

runtime fn transitive_path() -> i32 {
    return dynamic_path();
}

fn static_path() -> i32 {
    private_hook(41) {
        42;
    }
    return 0;
}

fn static_exported_path() -> i32 {
    apply local_impl {
        published(41) {
            42;
        }
    }
    return 0;
}
)luna";
    auto runtimeSlotSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            runtimeSlotSource, "<canonical-runtime-slot>");
    if (!runtimeSlotSnapshot.success()) {
        for (const auto& diagnostic : runtimeSlotSnapshot.errors())
            std::cerr << diagnostic << '\n';
        return fail("frontend rejected the runtime Slot source");
    }
    moon::LunaLowerer runtimeSlotLowerer;
    auto runtimeSlotModule = runtimeSlotLowerer.lower(
        *runtimeSlotSnapshot.program(),
        *runtimeSlotSnapshot.symbolTable());
    if (!runtimeSlotLowerer.errors().empty()) {
        for (const auto& diagnostic : runtimeSlotLowerer.errors())
            std::cerr << diagnostic << '\n';
        return fail("MoonIR lowering rejected the runtime Slot source");
    }
    moon::Sealer runtimeSlotSealer;
    if (!runtimeSlotSealer.sealFunctionBodies(*runtimeSlotModule)) {
        for (const auto& diagnostic : runtimeSlotSealer.errors())
            std::cerr << diagnostic << '\n';
        return fail("canonical sealing rejected the runtime Slot source");
    }
    if (!verifier.verify(*runtimeSlotModule))
        return fail("sealed runtime Slot module failed verification");
    moon::FunctionDecl* dynamicPath = nullptr;
    moon::FunctionDecl* mutableTransitivePath = nullptr;
    const moon::FunctionDecl* transitivePath = nullptr;
    const moon::FunctionDecl* staticPath = nullptr;
    const moon::FunctionDecl* staticExportedPath = nullptr;
    for (auto& declaration : runtimeSlotModule->declarations) {
        auto* function = dynamic_cast<moon::FunctionDecl*>(
            declaration.get());
        if (function && function->name == "dynamic_path")
            dynamicPath = function;
        else if (function && function->name == "transitive_path") {
            mutableTransitivePath = function;
            transitivePath = function;
        }
        else if (function && function->name == "static_path")
            staticPath = function;
        else if (function && function->name == "static_exported_path")
            staticExportedPath = function;
    }
    if (!dynamicPath || !dynamicPath->controlFlow ||
        !transitivePath || !transitivePath->controlFlow ||
        !staticPath || !staticPath->controlFlow ||
        !staticExportedPath || !staticExportedPath->controlFlow)
        return fail("runtime Slot fixture lost its sealed functions");
    moon::Terminator* runtimeSlot = nullptr;
    size_t privateRuntimeSlots = 0;
    for (auto& block : dynamicPath->controlFlow->blocks)
        if (block.terminator.kind == moon::TerminatorKind::RuntimeSlot)
            runtimeSlot = &block.terminator;
    for (const auto& block : staticPath->controlFlow->blocks)
        privateRuntimeSlots +=
            block.terminator.kind == moon::TerminatorKind::RuntimeSlot;
    for (const auto& block : staticExportedPath->controlFlow->blocks)
        privateRuntimeSlots +=
            block.terminator.kind == moon::TerminatorKind::RuntimeSlot;
    const auto* packedArguments = runtimeSlot
        ? dynamic_cast<const moon::RecordLiteralExpr*>(
              runtimeSlot->operand.get())
        : nullptr;
    if (!runtimeSlot || !runtimeSlot->runtimeSlot.complete() ||
        runtimeSlot->runtimeArgumentsType.empty() || !packedArguments ||
        packedArguments->type != runtimeSlot->runtimeArgumentsType ||
        packedArguments->fields.size() != 1 || privateRuntimeSlots != 0)
        return fail("runtime Slot sealing lost nominal identity or static erasure");
    if (!dynamicPath->requiresFragmentContext ||
        !transitivePath->requiresFragmentContext ||
        staticPath->requiresFragmentContext ||
        staticExportedPath->requiresFragmentContext)
        return fail("runtime Slot context effect did not reach a direct caller fixed point");
    mutableTransitivePath->requiresFragmentContext = false;
    if (verifier.verify(*runtimeSlotModule))
        return fail("verifier accepted a forged fragment-context effect");
    mutableTransitivePath->requiresFragmentContext = true;
    if (!verifier.verify(*runtimeSlotModule))
        return fail("verifier rejected the restored fragment-context effect");
    dynamicPath->isExported = true;
    if (verifier.verify(*runtimeSlotModule))
        return fail("verifier accepted a context-requiring ordinary export");
    dynamicPath->isExported = false;
    if (!verifier.verify(*runtimeSlotModule))
        return fail("verifier rejected the restored internal context ABI");
    const auto* publicSlotRecord = runtimeSlotModule->findDeclaration(
        runtimeSlot->runtimeSlot);
    const auto publicSlotFound = publicSlotRecord
        ? runtimeSlotModule->declarationsById.find(publicSlotRecord->id)
        : runtimeSlotModule->declarationsById.end();
    auto* publishedSlot = publicSlotFound ==
            runtimeSlotModule->declarationsById.end()
        ? nullptr : dynamic_cast<moon::SlotDecl*>(publicSlotFound->second);
    if (!publishedSlot)
        return fail("runtime Slot fixture lost its executable Slot declaration");
    publishedSlot->isExported = false;
    if (verifier.verify(*runtimeSlotModule))
        return fail("forged export row published a private runtime Slot");
    publishedSlot->isExported = true;
    if (!verifier.verify(*runtimeSlotModule))
        return fail("verifier rejected the restored public runtime Slot");
    const auto ownPackage = publishedSlot->packageId;
    publishedSlot->packageId = "canonical.foreign_package";
    runtimeSlotModule->packageUses.push_back({
        runtimeSlotModule->name, publishedSlot->packageId, "foreign"});
    const bool acceptedForeignSlot = verifier.verify(*runtimeSlotModule);
    const bool rejectedRuntimeTarget = std::any_of(
        verifier.errors().begin(), verifier.errors().end(),
        [](const auto& diagnostic) {
            return diagnostic.message.find(
                "runtime Slot target is not an exported control") !=
                std::string::npos;
        });
    if (acceptedForeignSlot || !rejectedRuntimeTarget)
        return fail("forged local export row re-exported a foreign runtime Slot");
    runtimeSlotModule->packageUses.pop_back();
    publishedSlot->packageId = ownPackage;
    if (!verifier.verify(*runtimeSlotModule))
        return fail("verifier rejected the restored owning Slot package");

    // The first dispatch-lowering slice accepts a closed, side-effect-free
    // continuation. This fixture drives the real Runtime ABI rather than
    // replacing the terminator with a backend sentinel.
    CodeGenerator contextAbiCodegen("canonical-runtime-context-abi");
    if (!contextAbiCodegen.generate(runtimeSlotModule.get())) {
        for (const auto& diagnostic : contextAbiCodegen.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("hidden fragment-context direct-call ABI is inconsistent");
    }
    const auto contextAbiPath =
        std::filesystem::temp_directory_path() /
        "luna-moonir-fragment-context-abi.ll";
    if (!contextAbiCodegen.emitObjectFile(contextAbiPath.string()))
        return fail("could not inspect the hidden fragment-context ABI");
    std::ifstream contextAbiInput(contextAbiPath, std::ios::binary);
    if (!contextAbiInput.is_open())
        return fail("could not reopen the hidden fragment-context LLVM IR");
    std::ostringstream contextAbiBuffer;
    contextAbiBuffer << contextAbiInput.rdbuf();
    std::error_code removeError;
    std::filesystem::remove(contextAbiPath, removeError);
    const std::string contextAbiIr = contextAbiBuffer.str();
    const auto firstContext = contextAbiIr.find("fragment.context");
    const auto forwardedContext = firstContext == std::string::npos
        ? std::string::npos
        : contextAbiIr.find("fragment.context", firstContext + 1);
    if (firstContext == std::string::npos ||
        forwardedContext == std::string::npos ||
        contextAbiIr.find("luna_runtime_fragment_dispatch_v1") ==
            std::string::npos)
        return fail("LLVM IR did not retain, forward, and dispatch the Fragment context");
    std::string contextAbiError;
    auto contextAbiLease =
        contextAbiCodegen.materializeJitModule(contextAbiError);
    if (!contextAbiLease)
        return fail("could not materialize the context-aware Runtime entry");
    const void* registryAddress = contextAbiLease->lookup(
        luna::runtime::runtimeDescriptorRegistrySymbol(runtimeSlotModule->name),
        contextAbiError);
    luna::runtime::RuntimeDescriptorRegistryView contextAbiRegistry;
    if (!registryAddress ||
        !contextAbiRegistry.bind(
            static_cast<const LunaRuntimeDescriptorRegistryV1*>(
                registryAddress),
            contextAbiError))
        return fail("could not bind the context-aware Runtime descriptor");
    const auto* contextEntryDescriptor = contextAbiRegistry.find(
        transitivePath->symbolId.value, transitivePath->contractId.value,
        LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
        LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1 |
            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1);
    if (!contextEntryDescriptor || !contextEntryDescriptor->entry)
        return fail("runtime function descriptor lost its Fragment-context ABI flag");
    luna::runtime::RuntimeFragmentBindingSet emptyBindings;
    std::vector<luna::runtime::RuntimeFragmentRef> noFragments;
    if (!luna::runtime::makeRuntimeFragmentBindingSet(
            std::move(noFragments), emptyBindings, contextAbiError))
        return fail("could not construct the empty runtime Fragment policy");
    luna::runtime::RuntimeFragmentExecutionContext executionContext;
    if (!luna::runtime::makeRuntimeFragmentExecutionContext(
            emptyBindings, executionContext, contextAbiError))
        return fail("could not construct the runtime Fragment execution context");
    using ContextEntry = int32_t (*)(const void*);
    const auto contextEntry = reinterpret_cast<ContextEntry>(
        const_cast<void*>(contextEntryDescriptor->entry));
    if (contextEntry(executionContext.opaque()) != 42)
        return fail("context-aware Runtime entry did not write back its captured frame");

    const auto* slotDeclaration = runtimeSlotModule->findDeclaration(
        runtimeSlot->runtimeSlot);
    const moon::DeclarationRecord* fragmentDeclaration = nullptr;
    for (const auto& declaration : runtimeSlotModule->declarationTable)
        if (declaration.kind == moon::DeclarationKind::Fragment &&
            declaration.sourceName == "local_impl")
            fragmentDeclaration = &declaration;
    const auto* fragmentDescriptor = fragmentDeclaration
        ? contextAbiRegistry.find(
              fragmentDeclaration->symbolId.value,
              fragmentDeclaration->contractId.value,
              LUNA_RUNTIME_DECLARATION_FRAGMENT_V1,
              LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_EXECUTABLE_V1 |
                  LUNA_RUNTIME_DESCRIPTOR_PUBLIC_CONTROL_V1)
        : nullptr;
    if (!slotDeclaration || !fragmentDescriptor || !fragmentDescriptor->entry)
        return fail("generated runtime Fragment was not discoverable");

    luna::runtime::MoonRuntime fragmentRuntime;
    luna::runtime::GenerationStagingRequest fragmentRequest{
        "canonical.runtime_slot.selected", std::string(64, 'f'),
        contextAbiLease};
    luna::runtime::MoonRuntime::StagedGeneration fragmentStaged;
    if (!fragmentRuntime.stage(
            fragmentRequest,
            [](const auto&, std::string&) { return true; },
            [&](const auto&, auto& bindings, std::string&) {
                bindings.push_back({
                    fragmentDescriptor->symbol_id,
                    fragmentDescriptor->contract_id,
                    fragmentDescriptor->entry,
                    fragmentDescriptor->declaration_kind,
                    luna::runtime::GenerationBindingFragmentExecutable |
                        luna::runtime::GenerationBindingPublicControl});
                return true;
            },
            {}, fragmentStaged, contextAbiError))
        return fail("generated runtime Fragment did not stage");
    luna::runtime::MoonRuntime::PinnedGeneration fragmentGeneration;
    if (!fragmentRuntime.loadOnce(
            fragmentStaged, fragmentGeneration, contextAbiError))
        return fail("generated runtime Fragment did not load");
    const luna::runtime::RuntimeSlotRequirement slotRequirement{
        slotDeclaration->symbolId.value,
        slotDeclaration->contractId.value};
    luna::runtime::RuntimeFragmentCandidateSnapshot candidates;
    if (!luna::runtime::snapshotRuntimeFragmentCandidates(
            fragmentGeneration, slotRequirement, candidates,
            contextAbiError) || candidates.size() != 1)
        return fail("generated runtime Fragment was not an exact Slot candidate");
    luna::runtime::RuntimeFragmentRef selectedFragment;
    const luna::runtime::RuntimeFragmentFactoryArguments noFactory{
        "", nullptr};
    if (!luna::runtime::makeOwnedRuntimeFragmentRef(
            *candidates.at(0), slotRequirement, noFactory,
            selectedFragment, contextAbiError))
        return fail("host could not select the generated runtime Fragment");
    std::vector<luna::runtime::RuntimeFragmentRef> selectedFragments;
    selectedFragments.push_back(std::move(selectedFragment));
    luna::runtime::RuntimeFragmentBindingSet selectedBindings;
    if (!luna::runtime::makeRuntimeFragmentBindingSet(
            std::move(selectedFragments), selectedBindings,
            contextAbiError))
        return fail("host selection did not become an immutable BindingSet");
    luna::runtime::RuntimeFragmentExecutionContext selectedContext;
    if (!luna::runtime::makeRuntimeFragmentExecutionContext(
            selectedBindings, selectedContext, contextAbiError) ||
        contextEntry(selectedContext.opaque()) != 42)
        return fail("selected runtime Fragment did not resume the outlined continuation");

    const std::string escapingRuntimeSlotSource = R"luna(
package canonical.runtime_slot_escape;

struct ContinuationResource {
    marker: i32;
}

impl Drop for ContinuationResource {
    fn drop(resource: &mut ContinuationResource) -> unit {
        print(resource.marker);
    }
}

export slot published(value: i32);

fn dynamic_path() -> i32 {
    published(41) {
        let resource = new ContinuationResource(64);
        return 42;
    }
    return 0;
}

runtime fn entry() -> i32 {
    return dynamic_path();
}

fn source_error() -> Result<i32, i32> {
    return Err(7);
}

fn try_path() -> Result<i32, i32> {
    published(41) {
        let resource = new ContinuationResource(65);
        let value = source_error()?;
        print(value);
    }
    return Ok(0);
}

runtime fn try_entry() -> i32 {
    let result = try_path();
    return unwrap_err(move result);
}
)luna";
    auto escapingSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        escapingRuntimeSlotSource, "<canonical-runtime-slot-escape>");
    if (!escapingSnapshot.success())
        return fail("frontend rejected the escaping Runtime Slot fixture");
    moon::LunaLowerer escapingLowerer;
    auto escapingModule = escapingLowerer.lower(
        *escapingSnapshot.program(), *escapingSnapshot.symbolTable());
    moon::Sealer escapingSealer;
    if (!escapingLowerer.errors().empty() ||
        !escapingSealer.sealFunctionBodies(*escapingModule) ||
        !verifier.verify(*escapingModule))
        return fail("escaping Runtime Slot fixture did not reach code generation");
    CodeGenerator escapingCodegen("canonical-runtime-slot-escape");
    if (!escapingCodegen.generate(escapingModule.get()))
        return fail("codegen rejected a resource-cleaning Runtime Slot return escape");
    const moon::FunctionDecl* escapingEntryDeclaration = nullptr;
    const moon::FunctionDecl* tryEntryDeclaration = nullptr;
    for (const auto& declaration : escapingModule->declarations) {
        const auto* function = dynamic_cast<const moon::FunctionDecl*>(
            declaration.get());
        if (function && function->name == "entry")
            escapingEntryDeclaration = function;
        else if (function && function->name == "try_entry")
            tryEntryDeclaration = function;
    }
    auto escapingLease = escapingCodegen.materializeJitModule(contextAbiError);
    if (!escapingLease || !escapingEntryDeclaration || !tryEntryDeclaration)
        return fail("could not materialize the escaping Runtime entry");
    const void* escapingRegistryAddress = escapingLease->lookup(
        luna::runtime::runtimeDescriptorRegistrySymbol(escapingModule->name),
        contextAbiError);
    luna::runtime::RuntimeDescriptorRegistryView escapingRegistry;
    if (!escapingRegistryAddress ||
        !escapingRegistry.bind(
            static_cast<const LunaRuntimeDescriptorRegistryV1*>(
                escapingRegistryAddress),
            contextAbiError))
        return fail("could not bind the escaping Runtime registry");
    const auto* escapingEntry = escapingRegistry.find(
        escapingEntryDeclaration->symbolId.value,
        escapingEntryDeclaration->contractId.value,
        LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
        LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1 |
            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1);
    if (!escapingEntry || !escapingEntry->entry ||
        reinterpret_cast<ContextEntry>(
            const_cast<void*>(escapingEntry->entry))(
                executionContext.opaque()) != 42)
        return fail("Runtime Slot continuation return did not clean up and escape its entry");
    const auto* tryEntry = escapingRegistry.find(
        tryEntryDeclaration->symbolId.value,
        tryEntryDeclaration->contractId.value,
        LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
        LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1 |
            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1);
    if (!tryEntry || !tryEntry->entry ||
        reinterpret_cast<ContextEntry>(
            const_cast<void*>(tryEntry->entry))(
                executionContext.opaque()) != 7)
        return fail("Runtime Slot continuation '?' did not clean up and escape its entry");

    const std::string nestedRuntimeSlotSource = R"luna(
package canonical.runtime_slot_nested;

export slot outer_hook(value: i32);
export slot inner_hook(value: i32);

fn inner_path() -> i32 {
    let result = 1;
    inner_hook(2) {
        result += 3;
    }
    return result;
}

fn outer_path() -> i32 {
    let result = 40;
    outer_hook(1) {
        result += inner_path();
    }
    return result;
}

runtime fn entry() -> i32 {
    return outer_path();
}
)luna";
    auto nestedSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        nestedRuntimeSlotSource, "<canonical-runtime-slot-nested>");
    if (!nestedSnapshot.success())
        return fail("frontend rejected nested Runtime Slot calls");
    moon::LunaLowerer nestedLowerer;
    auto nestedModule = nestedLowerer.lower(
        *nestedSnapshot.program(), *nestedSnapshot.symbolTable());
    moon::Sealer nestedSealer;
    if (!nestedLowerer.errors().empty() ||
        !nestedSealer.sealFunctionBodies(*nestedModule) ||
        !verifier.verify(*nestedModule))
        return fail("nested Runtime Slot calls did not verify");
    const moon::FunctionDecl* nestedEntryDeclaration = nullptr;
    for (const auto& declaration : nestedModule->declarations) {
        const auto* function = dynamic_cast<const moon::FunctionDecl*>(
            declaration.get());
        if (function && function->name == "entry")
            nestedEntryDeclaration = function;
    }
    CodeGenerator nestedCodegen("canonical-runtime-slot-nested");
    if (!nestedCodegen.generate(nestedModule.get())) {
        for (const auto& diagnostic : nestedCodegen.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("nested Runtime Slot calls lost the explicit context");
    }
    auto nestedLease = nestedCodegen.materializeJitModule(contextAbiError);
    if (!nestedLease || !nestedEntryDeclaration)
        return fail("could not materialize nested Runtime Slot calls");
    const void* nestedRegistryAddress = nestedLease->lookup(
        luna::runtime::runtimeDescriptorRegistrySymbol(nestedModule->name),
        contextAbiError);
    luna::runtime::RuntimeDescriptorRegistryView nestedRegistry;
    if (!nestedRegistryAddress ||
        !nestedRegistry.bind(
            static_cast<const LunaRuntimeDescriptorRegistryV1*>(
                nestedRegistryAddress),
            contextAbiError))
        return fail("could not bind the nested Runtime Slot registry");
    const auto* nestedEntry = nestedRegistry.find(
        nestedEntryDeclaration->symbolId.value,
        nestedEntryDeclaration->contractId.value,
        LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
        LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1 |
            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1);
    if (!nestedEntry || !nestedEntry->entry ||
        reinterpret_cast<ContextEntry>(
            const_cast<void*>(nestedEntry->entry))(
                executionContext.opaque()) != 44)
        return fail("nested direct call lost its pinned Fragment execution context");

    const std::string lexicalNestedSlotSource = R"luna(
package canonical.runtime_slot_lexical_nested;

export slot outer_hook(value: i32);
export slot inner_hook(value: i32);
export slot deepest_hook(value: i32);

struct CapturedResource {
    marker: i32;
}

impl Drop for CapturedResource {
    fn drop(resource: &mut CapturedResource) -> unit {
        print(resource.marker);
    }
}

fn path() -> i32 {
    let result = 4;
    outer_hook(1) {
        result += 1;
        inner_hook(2) {
            deepest_hook(3) {
                result += 2;
            }
        }
        result += 3;
    }
    return result;
}

fn escape_path() -> i32 {
    let result = 4;
    outer_hook(1) {
        result += 1;
        inner_hook(2) {
            return result + 2;
        }
        result += 100;
    }
    return 0;
}

fn source_error() -> Result<i32, i32> {
    return Err(7);
}

fn try_path() -> Result<i32, i32> {
    outer_hook(1) {
        inner_hook(2) {
            let value = source_error()?;
            print(value);
        }
    }
    return Ok(0);
}

fn resource_path() -> i32 {
    let resource = new CapturedResource(5);
    outer_hook(1) {
        resource.marker += 1;
        inner_hook(2) {
            resource.marker += 2;
        }
    }
    return resource.marker;
}

fn resource_escape_path() -> i32 {
    let outer_resource = new CapturedResource(10);
    outer_hook(1) {
        let local_resource = new CapturedResource(20);
        inner_hook(2) {
            return outer_resource.marker + local_resource.marker;
        }
    }
    return 0;
}

runtime fn entry() -> i32 {
    return path();
}

runtime fn escape_entry() -> i32 {
    return escape_path();
}

runtime fn try_entry() -> i32 {
    let result = try_path();
    return unwrap_err(move result);
}

runtime fn resource_entry() -> i32 {
    return resource_path();
}

runtime fn resource_escape_entry() -> i32 {
    return resource_escape_path();
}

)luna";
    auto lexicalNestedSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            lexicalNestedSlotSource,
            "<canonical-runtime-slot-lexical-nested>");
    if (!lexicalNestedSnapshot.success())
        return fail("frontend rejected the lexical nested Slot fixture");
    moon::LunaLowerer lexicalNestedLowerer;
    auto lexicalNestedModule = lexicalNestedLowerer.lower(
        *lexicalNestedSnapshot.program(),
        *lexicalNestedSnapshot.symbolTable());
    moon::Sealer lexicalNestedSealer;
    const bool lexicalNestedSealed =
        lexicalNestedSealer.sealFunctionBodies(*lexicalNestedModule);
    const bool lexicalNestedVerified = lexicalNestedSealed &&
        verifier.verify(*lexicalNestedModule);
    if (!lexicalNestedLowerer.errors().empty() ||
        !lexicalNestedSealed || !lexicalNestedVerified) {
        for (const auto& diagnostic : lexicalNestedLowerer.errors())
            std::cerr << diagnostic.message << '\n';
        for (const auto& diagnostic : lexicalNestedSealer.errors())
            std::cerr << diagnostic << '\n';
        if (lexicalNestedSealed)
            for (const auto& diagnostic : verifier.errors())
                std::cerr << diagnostic.message << '\n';
        return fail("lexical nested Slot fixture did not reach code generation");
    }
    CodeGenerator lexicalNestedCodegen(
        "canonical-runtime-slot-lexical-nested");
    if (!lexicalNestedCodegen.generate(lexicalNestedModule.get())) {
        for (const auto& diagnostic : lexicalNestedCodegen.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("lexical nested Runtime Slot did not recursively outline");
    }
    auto lexicalNestedLease = lexicalNestedCodegen.materializeJitModule(
        contextAbiError);
    if (!lexicalNestedLease)
        return fail("could not materialize lexical nested Runtime Slot calls");
    const moon::FunctionDecl* lexicalNestedEntryDeclaration = nullptr;
    const moon::FunctionDecl* lexicalNestedEscapeDeclaration = nullptr;
    const moon::FunctionDecl* lexicalNestedTryDeclaration = nullptr;
    const moon::FunctionDecl* lexicalNestedResourceDeclaration = nullptr;
    const moon::FunctionDecl* lexicalNestedResourceEscapeDeclaration = nullptr;
    for (const auto& declaration : lexicalNestedModule->declarations) {
        const auto* function = dynamic_cast<const moon::FunctionDecl*>(
            declaration.get());
        if (function && function->name == "entry")
            lexicalNestedEntryDeclaration = function;
        else if (function && function->name == "escape_entry")
            lexicalNestedEscapeDeclaration = function;
        else if (function && function->name == "try_entry")
            lexicalNestedTryDeclaration = function;
        else if (function && function->name == "resource_entry")
            lexicalNestedResourceDeclaration = function;
        else if (function && function->name == "resource_escape_entry")
            lexicalNestedResourceEscapeDeclaration = function;
    }
    const void* lexicalNestedRegistryAddress = lexicalNestedLease->lookup(
        luna::runtime::runtimeDescriptorRegistrySymbol(
            lexicalNestedModule->name),
        contextAbiError);
    luna::runtime::RuntimeDescriptorRegistryView lexicalNestedRegistry;
    if (!lexicalNestedEntryDeclaration ||
        !lexicalNestedEscapeDeclaration ||
        !lexicalNestedTryDeclaration ||
        !lexicalNestedResourceDeclaration ||
        !lexicalNestedResourceEscapeDeclaration ||
        !lexicalNestedRegistryAddress ||
        !lexicalNestedRegistry.bind(
            static_cast<const LunaRuntimeDescriptorRegistryV1*>(
                lexicalNestedRegistryAddress),
            contextAbiError))
        return fail("could not bind the lexical nested Runtime registry");
    const auto* lexicalNestedEntry = lexicalNestedRegistry.find(
        lexicalNestedEntryDeclaration->symbolId.value,
        lexicalNestedEntryDeclaration->contractId.value,
        LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
        LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1 |
            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1);
    if (!lexicalNestedEntry || !lexicalNestedEntry->entry ||
        reinterpret_cast<ContextEntry>(
            const_cast<void*>(lexicalNestedEntry->entry))(
                executionContext.opaque()) != 10)
        return fail("lexical nested Runtime Slot lost transitive capture writeback");
    const auto* lexicalNestedEscape = lexicalNestedRegistry.find(
        lexicalNestedEscapeDeclaration->symbolId.value,
        lexicalNestedEscapeDeclaration->contractId.value,
        LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
        LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1 |
            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1);
    if (!lexicalNestedEscape || !lexicalNestedEscape->entry ||
        reinterpret_cast<ContextEntry>(
            const_cast<void*>(lexicalNestedEscape->entry))(
                executionContext.opaque()) != 7)
        return fail("lexical nested Runtime Slot lost escaped return propagation");
    const auto* lexicalNestedTry = lexicalNestedRegistry.find(
        lexicalNestedTryDeclaration->symbolId.value,
        lexicalNestedTryDeclaration->contractId.value,
        LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
        LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1 |
            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1);
    if (!lexicalNestedTry || !lexicalNestedTry->entry ||
        reinterpret_cast<ContextEntry>(
            const_cast<void*>(lexicalNestedTry->entry))(
                executionContext.opaque()) != 7)
        return fail("lexical nested Runtime Slot lost '?' escape propagation");
    const auto* lexicalNestedResource = lexicalNestedRegistry.find(
        lexicalNestedResourceDeclaration->symbolId.value,
        lexicalNestedResourceDeclaration->contractId.value,
        LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
        LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1 |
            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1);
    if (!lexicalNestedResource || !lexicalNestedResource->entry ||
        reinterpret_cast<ContextEntry>(
            const_cast<void*>(lexicalNestedResource->entry))(
                executionContext.opaque()) != 8)
        return fail("lexical nested Runtime Slot lost affine capture writeback");
    const auto* lexicalNestedResourceEscape = lexicalNestedRegistry.find(
        lexicalNestedResourceEscapeDeclaration->symbolId.value,
        lexicalNestedResourceEscapeDeclaration->contractId.value,
        LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
        LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1 |
            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1);
    if (!lexicalNestedResourceEscape ||
        !lexicalNestedResourceEscape->entry ||
        reinterpret_cast<ContextEntry>(
            const_cast<void*>(lexicalNestedResourceEscape->entry))(
                executionContext.opaque()) != 30)
        return fail("lexical nested Runtime Slot lost outer resource cleanup escape");
    const std::string outerResourceEscapeSource = R"luna(
package canonical.runtime_slot_outer_resource_escape;

struct Resource { marker: i32; }
impl Drop for Resource {
    fn drop(resource: &mut Resource) -> unit {
        print(resource.marker);
    }
}
export slot hook(value: i32);
export fragment skip(value: i32) for hook {
    print(value);
}
fn path() -> i32 {
    let resource = new Resource(9);
    hook(1) {
        return resource.marker;
    }
    return 0;
}
runtime fn entry() -> i32 { return path(); }
)luna";
    auto outerResourceEscapeSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            outerResourceEscapeSource,
            "<canonical-runtime-slot-outer-resource-escape>");
    if (!outerResourceEscapeSnapshot.success())
        return fail("frontend rejected the outer resource escape fixture");
    moon::LunaLowerer outerResourceEscapeLowerer;
    auto outerResourceEscapeModule = outerResourceEscapeLowerer.lower(
        *outerResourceEscapeSnapshot.program(),
        *outerResourceEscapeSnapshot.symbolTable());
    if (!outerResourceEscapeLowerer.errors().empty())
        return fail("outer resource escape failed before sealing");
    moon::Sealer outerResourceEscapeSealer;
    if (!outerResourceEscapeSealer.sealFunctionBodies(
            *outerResourceEscapeModule) ||
        !verifier.verify(*outerResourceEscapeModule))
        return fail("outer resource escape did not preserve exact cleanup edges");
    const moon::FunctionDecl* outerResourceEscapeEntryDeclaration = nullptr;
    for (const auto& declaration : outerResourceEscapeModule->declarations) {
        const auto* function = dynamic_cast<const moon::FunctionDecl*>(
            declaration.get());
        if (function && function->name == "entry")
            outerResourceEscapeEntryDeclaration = function;
    }
    CodeGenerator outerResourceEscapeCodegen(
        "canonical-runtime-slot-outer-resource-escape");
    if (!outerResourceEscapeCodegen.generate(
            outerResourceEscapeModule.get())) {
        for (const auto& diagnostic : outerResourceEscapeCodegen.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("outer resource escape did not outline its cleanup");
    }
    auto outerResourceEscapeLease =
        outerResourceEscapeCodegen.materializeJitModule(contextAbiError);
    if (!outerResourceEscapeLease || !outerResourceEscapeEntryDeclaration)
        return fail("could not materialize the outer resource escape entry");
    const void* outerResourceEscapeRegistryAddress =
        outerResourceEscapeLease->lookup(
            luna::runtime::runtimeDescriptorRegistrySymbol(
                outerResourceEscapeModule->name),
            contextAbiError);
    luna::runtime::RuntimeDescriptorRegistryView outerResourceEscapeRegistry;
    if (!outerResourceEscapeRegistryAddress ||
        !outerResourceEscapeRegistry.bind(
            static_cast<const LunaRuntimeDescriptorRegistryV1*>(
                outerResourceEscapeRegistryAddress),
            contextAbiError))
        return fail("could not bind the outer resource escape registry");
    const auto* outerResourceEscapeEntry = outerResourceEscapeRegistry.find(
        outerResourceEscapeEntryDeclaration->symbolId.value,
        outerResourceEscapeEntryDeclaration->contractId.value,
        LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
        LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1 |
            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1);
    if (!outerResourceEscapeEntry || !outerResourceEscapeEntry->entry ||
        reinterpret_cast<ContextEntry>(
            const_cast<void*>(outerResourceEscapeEntry->entry))(
                executionContext.opaque()) != 9)
        return fail("outer resource cleanup did not run on Slot escape");
    const moon::DeclarationRecord* outerResourceSlot = nullptr;
    const moon::DeclarationRecord* skippingFragment = nullptr;
    for (const auto& declaration : outerResourceEscapeModule->declarationTable) {
        if (declaration.kind == moon::DeclarationKind::Slot &&
            declaration.sourceName == "hook")
            outerResourceSlot = &declaration;
        else if (declaration.kind == moon::DeclarationKind::Fragment &&
                 declaration.sourceName == "skip")
            skippingFragment = &declaration;
    }
    const auto* skippingDescriptor = skippingFragment
        ? outerResourceEscapeRegistry.find(
              skippingFragment->symbolId.value,
              skippingFragment->contractId.value,
              LUNA_RUNTIME_DECLARATION_FRAGMENT_V1,
              LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_EXECUTABLE_V1 |
                  LUNA_RUNTIME_DESCRIPTOR_PUBLIC_CONTROL_V1)
        : nullptr;
    if (!outerResourceSlot || !skippingDescriptor ||
        !skippingDescriptor->entry)
        return fail("skipping Fragment was not published as a Slot candidate");
    luna::runtime::MoonRuntime skippingRuntime;
    luna::runtime::GenerationStagingRequest skippingRequest{
        "canonical.runtime_slot.skipping", std::string(64, 'a'),
        outerResourceEscapeLease};
    luna::runtime::MoonRuntime::StagedGeneration skippingStaged;
    if (!skippingRuntime.stage(
            skippingRequest,
            [](const auto&, std::string&) { return true; },
            [&](const auto&, auto& bindings, std::string&) {
                bindings.push_back({
                    skippingDescriptor->symbol_id,
                    skippingDescriptor->contract_id,
                    skippingDescriptor->entry,
                    skippingDescriptor->declaration_kind,
                    luna::runtime::GenerationBindingFragmentExecutable |
                        luna::runtime::GenerationBindingPublicControl});
                return true;
            },
            {}, skippingStaged, contextAbiError))
        return fail("skipping Fragment did not stage");
    luna::runtime::MoonRuntime::PinnedGeneration skippingGeneration;
    if (!skippingRuntime.loadOnce(
            skippingStaged, skippingGeneration, contextAbiError))
        return fail("skipping Fragment did not load");
    const luna::runtime::RuntimeSlotRequirement skippingSlot{
        outerResourceSlot->symbolId.value,
        outerResourceSlot->contractId.value};
    luna::runtime::RuntimeFragmentCandidateSnapshot skippingCandidates;
    if (!luna::runtime::snapshotRuntimeFragmentCandidates(
            skippingGeneration, skippingSlot, skippingCandidates,
            contextAbiError) || skippingCandidates.size() != 1)
        return fail("skipping Fragment was not an exact Slot candidate");
    luna::runtime::RuntimeFragmentRef skippingRef;
    if (!luna::runtime::makeOwnedRuntimeFragmentRef(
            *skippingCandidates.at(0), skippingSlot, noFactory,
            skippingRef, contextAbiError))
        return fail("host could not select the skipping Fragment");
    std::vector<luna::runtime::RuntimeFragmentRef> skippingFragments;
    skippingFragments.push_back(std::move(skippingRef));
    luna::runtime::RuntimeFragmentBindingSet skippingBindings;
    if (!luna::runtime::makeRuntimeFragmentBindingSet(
            std::move(skippingFragments), skippingBindings, contextAbiError))
        return fail("skipping Fragment did not form an immutable BindingSet");
    luna::runtime::RuntimeFragmentExecutionContext skippingContext;
    if (!luna::runtime::makeRuntimeFragmentExecutionContext(
            skippingBindings, skippingContext, contextAbiError) ||
        reinterpret_cast<ContextEntry>(
            const_cast<void*>(outerResourceEscapeEntry->entry))(
                skippingContext.opaque()) != 0)
        return fail("skipping Fragment did not take the post-Slot cleanup path");
    const std::string divergentSlotOwnershipSource = R"luna(
package canonical.runtime_slot_divergent_ownership;
struct Resource { marker: i32; }
impl Drop for Resource {
    fn drop(resource: &mut Resource) -> unit { print(resource.marker); }
}
export slot hook(value: i32);
fn path() -> i32 {
    let resource = new Resource(1);
    hook(0) { free resource; }
    return 0;
}
)luna";
    auto divergentSlotOwnershipSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            divergentSlotOwnershipSource,
            "<canonical-runtime-slot-divergent-ownership>");
    const bool diagnosedDivergentSlotOwnership = std::any_of(
        divergentSlotOwnershipSnapshot.errors().begin(),
        divergentSlotOwnershipSnapshot.errors().end(),
        [](const diagnostic::Diagnostic& diagnostic) {
            return diagnostic.message.find(
                "owned heap value 'resource' is freed or moved on only some paths through `slot`") !=
                std::string::npos;
        });
    if (divergentSlotOwnershipSnapshot.success() ||
        !diagnosedDivergentSlotOwnership) {
        for (const auto& diagnostic : divergentSlotOwnershipSnapshot.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("runtime Slot accepted divergent ownership after a skipped continuation");
    }
    const auto reverseIterator = reverse.typesById.find(shortId.value);
    if (reverseIterator == reverse.typesById.end())
        return fail("sealed type index lost the iterator type");
    reverse.typeTable[reverseIterator->second].sysmeta.resource.usage =
        luna::ownership::Usage::Copy;
    if (verifier.verify(reverse))
        return fail("verifier accepted a forged derived Resource contract");


    return 0;
}

} // namespace canonical_test
