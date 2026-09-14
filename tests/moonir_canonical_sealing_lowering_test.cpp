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

int runLoweredCompositionTests(SealingTestContext& context) {
    auto& cfgVerifier = context.cfgVerifier;
    auto& verifier = context.verifier;
    auto& reverse = context.reverseModule;
    const auto shortId = context.shortIteratorType;

    const std::string loweredCompositionSource = R"luna(
package canonical.integration;

slot context hook(value: i32) default passthrough;
slot context captured();

context passthrough(value: i32) for hook {
    resume();
}

context lexical_capture for captured {
    outer;
    resume();
}

fn main() -> i32 {
    let outer = 7;
    hook(outer) {
        outer;
    }
    apply lexical_capture {
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
        return fail("frontend rejected canonical default-fragment integration source");
    }
    moon::LunaLowerer integrationLowerer;
    auto integrationModule = integrationLowerer.lower(
        *compositionSnapshot.program(), *compositionSnapshot.symbolTable());
    if (!integrationLowerer.errors().empty()) {
        for (const auto& diagnostic : integrationLowerer.errors())
            std::cerr << diagnostic << '\n';
        return fail("MoonIR lowering rejected canonical default-fragment source");
    }
    if (!verifier.verify(*integrationModule)) {
        for (const auto& diagnostic : verifier.errors())
            std::cerr << diagnostic << '\n';
        return fail("lowered default-fragment module failed structured verification");
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
        return fail("lowered integration module lost main or its default fragment");
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
        return fail("lowered default fragment did not enter canonical CFG construction");
    }
    if (!cfgVerifier.verify(*integrationCfg, *integrationModule)) {
        for (const auto& diagnostic : cfgVerifier.errors())
            std::cerr << diagnostic << '\n';
        return fail("lowered default fragment failed canonical CFG verification");
    }
    size_t integrationApplyRegions = 0;
    size_t integrationFragmentRegions = 0;
    size_t integrationContinuationRegions = 0;
    size_t integrationResumeEdges = 0;
    size_t fragmentOuterCaptures = 0;
    size_t continuationOuterCaptures = 0;
    bool captureEscapedRoot = false;
    for (const auto& composedRegion : integrationCfg->regions) {
        integrationApplyRegions +=
            composedRegion.kind == moon::RegionKind::Apply;
        integrationFragmentRegions +=
            composedRegion.kind == moon::RegionKind::Fragment;
        integrationContinuationRegions +=
            composedRegion.kind == moon::RegionKind::Continuation;
    }
    for (auto& block : integrationCfg->blocks) {
        integrationResumeEdges +=
            block.terminator.kind == moon::TerminatorKind::Resume;
        for (auto& operation : block.operations) {
            auto* effect = dynamic_cast<moon::ExprStmt*>(operation.get());
            auto* identifier = effect
                ? dynamic_cast<moon::IdentifierExpr*>(effect->expr.get())
                : nullptr;
            if (!identifier || identifier->name != "outer") continue;
            if (identifier->local.empty() ||
                integrationCfg->locals[identifier->local.value].name !=
                    "outer" ||
                integrationCfg->locals[identifier->local.value].scope !=
                    integrationCfg->rootScope)
                captureEscapedRoot = true;
            const auto kind =
                integrationCfg->regions[block.region.value].kind;
            fragmentOuterCaptures += kind == moon::RegionKind::Fragment;
            continuationOuterCaptures +=
                kind == moon::RegionKind::Continuation;
        }
    }
    if (integrationApplyRegions != 2 ||
        integrationFragmentRegions != 2 ||
        integrationContinuationRegions != 2 ||
        integrationResumeEdges != 2 ||
        fragmentOuterCaptures != 1 ||
        continuationOuterCaptures != 2 || captureEscapedRoot ||
        integrationFragment->body.get() != integrationFragmentBody ||
        integrationCaptureFragment->body.get() != integrationCaptureBody)
        return fail("frontend-to-CFG composition lost its default, capture, or construction body");

    const std::string loweredRuntimeCompositionSource = R"luna(
package canonical.runtime_boundary;

slot context pipeline(value: i32);

runtime context trace(value: i32) for pipeline {
    value;
    resume();
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
        return fail("canonical function sealing rejected a linked runtime context");
    }
    if (dynamicEntry->body || !dynamicEntry->controlFlow ||
        stableEntry->body || !stableEntry->controlFlow)
        return fail("runtime context sealing did not atomically consume the function set");
    if (!verifier.verify(*runtimeIntegrationModule))
        return fail("sealed runtime-context module failed canonical verification");
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
        return fail("runtime context lost its Fragment/Continuation/resume CFG");

    // Positive: an ordinary lexical interceptor apply seals into a verified
    // canonical CFG with one Fragment region and one shared continuation.
    const std::string staticInterceptorSource = R"luna(
package canonical.dynamic_interceptor;

slot interceptor pipeline(value: i32);

runtime interceptor trace(value: i32) for pipeline {
    print(value + 1);
}

runtime interceptor audit(value: i32) for pipeline {
    print(value + 2);
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
        return fail("frontend rejected the static interceptor source");
    }
    moon::LunaLowerer interceptorLowerer;
    auto interceptorModule = interceptorLowerer.lower(
        *interceptorSnapshot.program(),
        *interceptorSnapshot.symbolTable());
    if (!interceptorLowerer.errors().empty()) {
        for (const auto& diagnostic : interceptorLowerer.errors())
            std::cerr << diagnostic << '\n';
        return fail("MoonIR lowering rejected the static interceptor source");
    }
    if (!verifier.verify(*interceptorModule)) {
        for (const auto& diagnostic : verifier.errors())
            std::cerr << diagnostic << '\n';
        return fail("static interceptor module failed structured verification");
    }
    moon::FunctionDecl* interceptorMain = nullptr;
    for (auto& declaration : interceptorModule->declarations) {
        auto* function = dynamic_cast<moon::FunctionDecl*>(declaration.get());
        if (function && function->name == "main")
            interceptorMain = function;
    }
    if (!interceptorMain || !interceptorMain->body)
        return fail("static interceptor module lost its main body");
    moon::Sealer interceptorSealer;
    if (!interceptorSealer.sealFunctionBodies(*interceptorModule)) {
        for (const auto& diagnostic : interceptorSealer.errors())
            std::cerr << diagnostic << '\n';
        return fail("canonical sealing rejected a valid static interceptor apply");
    }
    if (!interceptorMain->controlFlow)
        return fail("static interceptor main was not sealed to a canonical CFG");
    size_t interceptorFragmentRegions = 0;
    size_t interceptorContinuationRegions = 0;
    for (const auto& region : interceptorMain->controlFlow->regions) {
        if (region.kind == moon::RegionKind::Fragment) ++interceptorFragmentRegions;
        if (region.kind == moon::RegionKind::Continuation) ++interceptorContinuationRegions;
    }
    if (interceptorFragmentRegions != 1)
        return fail("static interceptor apply did not materialize one Fragment region");
    if (interceptorContinuationRegions != 1)
        return fail("static interceptor apply did not materialize one shared Continuation region");

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
