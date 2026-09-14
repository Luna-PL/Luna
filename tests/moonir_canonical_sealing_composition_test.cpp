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

int runCompositionSealingTests(SealingTestContext& testContext) {
    auto& cfgBuilder = testContext.builder;
    auto& cfgVerifier = testContext.cfgVerifier;

    moon::Module compositionModule;
    compositionModule.name = "canonical.composition";
    const auto compositionUnit = compositionModule.registerType(TyUnit);
    const auto compositionI32 = compositionModule.registerType(TyI32);
    const auto sharedI32Type = Type::makeReference(TyI32);
    const auto sharedI32TypeId =
        compositionModule.registerType(sharedI32Type);
    const std::string interceptorSlotId =
        "canonical.composition::slot::hook";
    const auto interceptorSlotType = Type::makeSlot(
        {sharedI32Type}, TyUnit, false, ContinuationKind::Interceptor,
        {{luna::ownership::Relation::SharedBorrow,
          luna::ownership::Usage::Copy}});
    interceptorSlotType->identityMode = luna::types::IdentityMode::Nominal;
    interceptorSlotType->nominalId = interceptorSlotId;
    const auto interceptorSlotTypeId =
        compositionModule.registerType(interceptorSlotType);
    moon::DeclarationRecord interceptorSlotRecord;
    interceptorSlotRecord.id = interceptorSlotId;
    interceptorSlotRecord.familyId = interceptorSlotId;
    interceptorSlotRecord.symbolId =
        luna::identity::symbolIdFromCanonical(interceptorSlotId);
    interceptorSlotRecord.sourceName = "hook";
    interceptorSlotRecord.linkageName = "hook";
    interceptorSlotRecord.kind = moon::DeclarationKind::Slot;
    interceptorSlotRecord.type = interceptorSlotTypeId;
    interceptorSlotRecord.sysmeta = interceptorSlotType->sysmeta;
    compositionModule.declarationTable.push_back(interceptorSlotRecord);
    const auto interceptorType = Type::makeFragment(
        {sharedI32Type}, TyUnit, false, ContinuationKind::Interceptor,
        {{luna::ownership::Relation::SharedBorrow,
          luna::ownership::Usage::Copy}});
    interceptorType->identityMode = luna::types::IdentityMode::Nominal;
    interceptorType->nominalId = interceptorSlotId;
    const auto interceptorTypeId =
        compositionModule.registerType(interceptorType);
    const std::string interceptorId =
        "canonical.composition::fragment::guard";
    moon::DeclarationRecord interceptorRecord;
    interceptorRecord.id = interceptorId;
    interceptorRecord.familyId = interceptorId;
    interceptorRecord.symbolId =
        luna::identity::symbolIdFromCanonical(interceptorId);
    interceptorRecord.sourceName = "guard";
    interceptorRecord.linkageName = "guard";
    interceptorRecord.kind = moon::DeclarationKind::Fragment;
    interceptorRecord.type = interceptorTypeId;
    interceptorRecord.sysmeta = interceptorType->sysmeta;
    compositionModule.declarationTable.push_back(interceptorRecord);

    auto interceptor = std::make_unique<moon::FragmentDecl>();
    interceptor->packageId = compositionModule.name;
    interceptor->declarationId = interceptorId;
    interceptor->familyId = interceptorId;
    interceptor->symbolId = interceptorRecord.symbolId;
    interceptor->name = "guard";
    interceptor->generatedSymbolName = "guard";
    interceptor->kind = moon::FragmentKind::Interceptor;
    interceptor->cardinality = moon::FragmentCardinality::Once;
    interceptor->structuralType = interceptorTypeId;
    interceptor->params.push_back({
        "view", false, luna::ownership::Usage::Copy,
        luna::ownership::Relation::SharedBorrow, sharedI32TypeId});
    interceptor->body = std::make_unique<moon::BlockStmt>();
    auto guardedReturn = std::make_unique<moon::IfStmt>();
    auto returnCondition = std::make_unique<moon::BoolLiteralExpr>();
    returnCondition->value = false;
    returnCondition->type = compositionModule.registerType(TyBool);
    guardedReturn->cond = std::move(returnCondition);
    guardedReturn->thenBlock = std::make_unique<moon::BlockStmt>();
    guardedReturn->thenBlock->stmts.push_back(
        std::make_unique<moon::ReturnStmt>());
    interceptor->body->stmts.push_back(std::move(guardedReturn));
    auto guardedAbort = std::make_unique<moon::IfStmt>();
    auto abortCondition = std::make_unique<moon::BoolLiteralExpr>();
    abortCondition->value = true;
    abortCondition->type = compositionModule.registerType(TyBool);
    guardedAbort->cond = std::move(abortCondition);
    guardedAbort->thenBlock = std::make_unique<moon::BlockStmt>();
    guardedAbort->thenBlock->stmts.push_back(
        std::make_unique<moon::AbortStmt>());
    interceptor->body->stmts.push_back(std::move(guardedAbort));
    auto* interceptorBody = interceptor->body.get();
    compositionModule.declarations.push_back(std::move(interceptor));

    const std::string contextSlotId =
        "canonical.composition::slot::around_hook";
    const auto contextSlotType = Type::makeSlot(
        {}, TyUnit, false, ContinuationKind::Context);
    contextSlotType->identityMode = luna::types::IdentityMode::Nominal;
    contextSlotType->nominalId = contextSlotId;
    const auto contextSlotTypeId =
        compositionModule.registerType(contextSlotType);
    moon::DeclarationRecord contextSlotRecord;
    contextSlotRecord.id = contextSlotId;
    contextSlotRecord.familyId = contextSlotId;
    contextSlotRecord.symbolId =
        luna::identity::symbolIdFromCanonical(contextSlotId);
    contextSlotRecord.sourceName = "around_hook";
    contextSlotRecord.linkageName = "around_hook";
    contextSlotRecord.kind = moon::DeclarationKind::Slot;
    contextSlotRecord.type = contextSlotTypeId;
    contextSlotRecord.sysmeta = contextSlotType->sysmeta;
    compositionModule.declarationTable.push_back(contextSlotRecord);
    const auto contextType = Type::makeFragment(
        {}, TyUnit, false, ContinuationKind::Context);
    contextType->identityMode = luna::types::IdentityMode::Nominal;
    contextType->nominalId = contextSlotId;
    const auto contextTypeId = compositionModule.registerType(contextType);
    const std::string contextId =
        "canonical.composition::fragment::around";
    moon::DeclarationRecord contextRecord;
    contextRecord.id = contextId;
    contextRecord.familyId = contextId;
    contextRecord.symbolId =
        luna::identity::symbolIdFromCanonical(contextId);
    contextRecord.sourceName = "around";
    contextRecord.linkageName = "around";
    contextRecord.kind = moon::DeclarationKind::Fragment;
    contextRecord.type = contextTypeId;
    contextRecord.sysmeta = contextType->sysmeta;
    compositionModule.declarationTable.push_back(contextRecord);
    auto context = std::make_unique<moon::FragmentDecl>();
    context->packageId = compositionModule.name;
    context->declarationId = contextId;
    context->familyId = contextId;
    context->symbolId = contextRecord.symbolId;
    context->name = "around";
    context->generatedSymbolName = "around";
    context->kind = moon::FragmentKind::Context;
    context->cardinality = moon::FragmentCardinality::Once;
    context->structuralType = contextTypeId;
    context->body = std::make_unique<moon::BlockStmt>();
    auto shadow = std::make_unique<moon::LetStmt>();
    shadow->name = "value";
    shadow->type = compositionI32;
    auto shadowValue = std::make_unique<moon::IntLiteralExpr>();
    shadowValue->value = 2;
    shadowValue->type = compositionI32;
    shadow->initializer = std::move(shadowValue);
    context->body->stmts.push_back(std::move(shadow));
    context->body->stmts.push_back(std::make_unique<moon::ResumeStmt>());
    auto postResumeEffect = std::make_unique<moon::ExprStmt>();
    auto postResumeUnit = std::make_unique<moon::UnitExpr>();
    postResumeUnit->type = compositionUnit;
    postResumeEffect->expr = std::move(postResumeUnit);
    context->body->stmts.push_back(std::move(postResumeEffect));
    auto* contextBody = context->body.get();
    compositionModule.declarations.push_back(std::move(context));
    compositionModule.sealTypeTable();
    const auto* sealedInterceptor =
        compositionModule.findDeclarationById(interceptorId);
    if (!sealedInterceptor)
        return fail("static composition lost its fragment declaration row");
    auto* executableInterceptor = static_cast<moon::FragmentDecl*>(
        compositionModule.declarations.front().get());
    executableInterceptor->contractId = sealedInterceptor->contractId;
    executableInterceptor->sysmeta = sealedInterceptor->sysmeta;
    const auto* sealedInterceptorSlot =
        compositionModule.findDeclarationById(interceptorSlotId);
    if (!sealedInterceptorSlot)
        return fail("static composition lost its nominal interceptor slot");
    executableInterceptor->targetSlot = {
        sealedInterceptorSlot->symbolId, sealedInterceptorSlot->contractId};
    const auto* sealedContext =
        compositionModule.findDeclarationById(contextId);
    if (!sealedContext)
        return fail("static composition lost its context declaration row");
    auto* executableContext = static_cast<moon::FragmentDecl*>(
        compositionModule.declarations[1].get());
    executableContext->contractId = sealedContext->contractId;
    executableContext->sysmeta = sealedContext->sysmeta;
    const auto* sealedContextSlot =
        compositionModule.findDeclarationById(contextSlotId);
    if (!sealedContextSlot)
        return fail("static composition lost its nominal context slot");
    executableContext->targetSlot = {
        sealedContextSlot->symbolId, sealedContextSlot->contractId};
    compositionModule.rebuildIndexes();
    const moon::DeclarationRef interceptorRef{
        sealedInterceptor->symbolId, sealedInterceptor->contractId};
    const moon::DeclarationRef contextRef{
        sealedContext->symbolId, sealedContext->contractId};

    auto compositionBody = std::make_unique<moon::BlockStmt>();
    auto borrowedSource = std::make_unique<moon::LetStmt>();
    borrowedSource->name = "source";
    borrowedSource->type = compositionI32;
    auto borrowedValue = std::make_unique<moon::IntLiteralExpr>();
    borrowedValue->value = 7;
    borrowedValue->type = compositionI32;
    borrowedSource->initializer = std::move(borrowedValue);
    compositionBody->stmts.push_back(std::move(borrowedSource));
    auto staticApply = std::make_unique<moon::ApplyStmt>();
    staticApply->slotName = "hook";
    staticApply->fragmentName = "guard";
    staticApply->fragmentRef = interceptorRef;
    staticApply->body = std::make_unique<moon::BlockStmt>();
    auto slotInvocation = std::make_unique<moon::SlotInvokeStmt>();
    slotInvocation->name = "hook";
    slotInvocation->acceptedKind = moon::FragmentKind::Interceptor;
    slotInvocation->acceptedCardinality = moon::FragmentCardinality::Once;
    auto sharedArgument = std::make_unique<moon::BorrowExpr>();
    sharedArgument->type = sharedI32TypeId;
    auto sharedSource = std::make_unique<moon::IdentifierExpr>();
    sharedSource->name = "source";
    sharedSource->type = compositionI32;
    sharedArgument->operand = std::move(sharedSource);
    slotInvocation->args.push_back(std::move(sharedArgument));
    slotInvocation->continuation = std::make_unique<moon::BlockStmt>();
    auto continuationEffect = std::make_unique<moon::ExprStmt>();
    auto continuationUnit = std::make_unique<moon::UnitExpr>();
    continuationUnit->type = compositionUnit;
    continuationEffect->expr = std::move(continuationUnit);
    slotInvocation->continuation->stmts.push_back(
        std::move(continuationEffect));
    staticApply->body->stmts.push_back(std::move(slotInvocation));
    compositionBody->stmts.push_back(std::move(staticApply));

    auto compositionCfg = cfgBuilder.build(
        std::move(compositionBody), {}, moon::RegionKind::Function,
        compositionModule);
    size_t applyRegions = 0;
    size_t fragmentRegions = 0;
    size_t continuationRegions = 0;
    size_t resumeEdges = 0;
    size_t abortEdges = 0;
    const moon::LocalRecord* borrowedFragmentLocal = nullptr;
    bool borrowedFragmentCleanup = false;
    for (const auto& region : compositionCfg
             ? compositionCfg->regions
             : std::vector<moon::RegionRecord>{}) {
        applyRegions += region.kind == moon::RegionKind::Apply;
        fragmentRegions += region.kind == moon::RegionKind::Fragment;
        continuationRegions += region.kind == moon::RegionKind::Continuation;
    }
    if (compositionCfg) {
        for (const auto& local : compositionCfg->locals)
            if (local.name == "view") borrowedFragmentLocal = &local;
        if (borrowedFragmentLocal)
            for (const auto& cleanup : compositionCfg->cleanups)
                borrowedFragmentCleanup = borrowedFragmentCleanup ||
                    cleanup.place.root == borrowedFragmentLocal->id;
        for (const auto& block : compositionCfg->blocks) {
            resumeEdges +=
                block.terminator.kind == moon::TerminatorKind::Resume;
            abortEdges +=
                block.terminator.kind == moon::TerminatorKind::Abort;
        }
    }
    if (!compositionCfg ||
        !cfgVerifier.verify(*compositionCfg, compositionModule) ||
        applyRegions != 1 || fragmentRegions != 1 ||
        continuationRegions != 1 || resumeEdges != 0 || abortEdges != 1 ||
        !borrowedFragmentLocal ||
        borrowedFragmentLocal->relation !=
            luna::ownership::Relation::SharedBorrow ||
        borrowedFragmentCleanup ||
        executableInterceptor->body.get() != interceptorBody)
        return fail("static interceptor did not compose into canonical CFG regions and edges");
    moon::LetStmt* borrowedFragmentDefinition = nullptr;
    for (auto& block : compositionCfg->blocks)
        for (auto& operation : block.operations)
            if (auto* declaration = dynamic_cast<moon::LetStmt*>(
                    operation.get());
                declaration && borrowedFragmentLocal &&
                declaration->local == borrowedFragmentLocal->id)
                borrowedFragmentDefinition = declaration;
    if (!borrowedFragmentDefinition)
        return fail("static composition lost its fragment parameter definition");
    const auto borrowedFragmentId = borrowedFragmentLocal->id;
    compositionCfg->locals[borrowedFragmentId.value].relation =
        luna::ownership::Relation::Owned;
    borrowedFragmentDefinition->relation =
        luna::ownership::Relation::Owned;
    if (cfgVerifier.verify(*compositionCfg, compositionModule))
        return fail("CFG verifier accepted a forged fragment parameter relation");
    compositionCfg->locals[borrowedFragmentId.value].relation =
        luna::ownership::Relation::SharedBorrow;
    borrowedFragmentDefinition->relation =
        luna::ownership::Relation::SharedBorrow;
    if (!cfgVerifier.verify(*compositionCfg, compositionModule))
        return fail("restored fragment parameter relation no longer verifies");
    moon::BlockId interceptorContinuationEntry;
    for (const auto& composedRegion : compositionCfg->regions)
        if (composedRegion.kind == moon::RegionKind::Continuation)
            interceptorContinuationEntry = composedRegion.entry;
    moon::Terminator* automaticForward = nullptr;
    moon::Terminator* composedAbort = nullptr;
    for (auto& block : compositionCfg->blocks) {
        if (block.terminator.kind == moon::TerminatorKind::Jump &&
            block.terminator.primary.target == interceptorContinuationEntry)
            automaticForward = &block.terminator;
        if (block.terminator.kind == moon::TerminatorKind::Abort)
            composedAbort = &block.terminator;
    }
    if (!automaticForward || !composedAbort)
        return fail("static composition lost a control terminator");
    const auto fragmentExit = composedAbort->primary.target;
    automaticForward->kind = moon::TerminatorKind::Resume;
    if (cfgVerifier.verify(*compositionCfg, compositionModule))
        return fail("CFG verifier accepted explicit resume in an interceptor");
    automaticForward->kind = moon::TerminatorKind::Jump;
    composedAbort->primary.target = interceptorContinuationEntry;
    if (cfgVerifier.verify(*compositionCfg, compositionModule))
        return fail("CFG verifier accepted abort into the continuation");
    composedAbort->primary.target = fragmentExit;
    if (!cfgVerifier.verify(*compositionCfg, compositionModule))
        return fail("restored static composition no longer verifies");

    auto contextCompositionBody = std::make_unique<moon::BlockStmt>();
    auto outerValue = std::make_unique<moon::LetStmt>();
    outerValue->name = "value";
    outerValue->type = compositionI32;
    auto outerInitializer = std::make_unique<moon::IntLiteralExpr>();
    outerInitializer->value = 1;
    outerInitializer->type = compositionI32;
    outerValue->initializer = std::move(outerInitializer);
    contextCompositionBody->stmts.push_back(std::move(outerValue));
    auto contextApply = std::make_unique<moon::ApplyStmt>();
    contextApply->slotName = "hook";
    contextApply->fragmentName = "around";
    contextApply->fragmentRef = contextRef;
    contextApply->body = std::make_unique<moon::BlockStmt>();
    auto contextInvocation = std::make_unique<moon::SlotInvokeStmt>();
    contextInvocation->name = "hook";
    contextInvocation->acceptedKind = moon::FragmentKind::Context;
    contextInvocation->acceptedCardinality = moon::FragmentCardinality::Once;
    contextInvocation->continuation = std::make_unique<moon::BlockStmt>();
    auto outerUse = std::make_unique<moon::ExprStmt>();
    auto outerIdentifier = std::make_unique<moon::IdentifierExpr>();
    outerIdentifier->name = "value";
    outerIdentifier->type = compositionI32;
    outerUse->expr = std::move(outerIdentifier);
    contextInvocation->continuation->stmts.push_back(
        std::move(outerUse));
    contextApply->body->stmts.push_back(std::move(contextInvocation));
    contextCompositionBody->stmts.push_back(std::move(contextApply));

    auto contextCfg = cfgBuilder.build(
        std::move(contextCompositionBody), {},
        moon::RegionKind::Function, compositionModule);
    moon::RegionRecord* contextRegion = nullptr;
    moon::Terminator* contextResume = nullptr;
    moon::Terminator* contextAbort = nullptr;
    moon::IdentifierExpr* continuationIdentifier = nullptr;
    if (contextCfg) {
        for (auto& composedRegion : contextCfg->regions)
            if (composedRegion.kind == moon::RegionKind::Fragment)
                contextRegion = &composedRegion;
        for (auto& block : contextCfg->blocks) {
            if (block.terminator.kind == moon::TerminatorKind::Resume)
                contextResume = &block.terminator;
            if (block.terminator.kind == moon::TerminatorKind::Abort)
                contextAbort = &block.terminator;
            if (contextCfg->regions[block.region.value].kind ==
                    moon::RegionKind::Continuation) {
                for (auto& operation : block.operations) {
                    auto* effect = dynamic_cast<moon::ExprStmt*>(
                        operation.get());
                    if (effect)
                        continuationIdentifier =
                            dynamic_cast<moon::IdentifierExpr*>(
                                effect->expr.get());
                }
            }
        }
    }
    if (!contextCfg ||
        !cfgVerifier.verify(*contextCfg, compositionModule) ||
        !contextRegion || !contextResume || !contextAbort ||
        !continuationIdentifier || continuationIdentifier->local.empty() ||
        contextCfg->locals[continuationIdentifier->local.value].scope !=
            contextCfg->rootScope ||
        executableContext->body.get() != contextBody)
        return fail("static context did not preserve its stack continuation boundary");

    moon::LocalId fragmentShadow;
    for (const auto& local : contextCfg->locals)
        if (local.name == "value" && local.scope != contextCfg->rootScope)
            fragmentShadow = local.id;
    if (fragmentShadow.empty())
        return fail("context composition lost its fragment-local shadow");
    const auto outerLocal = continuationIdentifier->local;
    continuationIdentifier->local = fragmentShadow;
    if (cfgVerifier.verify(*contextCfg, compositionModule))
        return fail("CFG verifier exposed fragment-local state to its continuation");
    continuationIdentifier->local = outerLocal;
    const auto contextContinuationEntry = contextResume->primary.target;
    contextResume->primary.target = contextAbort->primary.target;
    if (cfgVerifier.verify(*contextCfg, compositionModule))
        return fail("CFG verifier accepted context resume into the fragment exit");
    contextResume->primary.target = contextContinuationEntry;
    contextResume->kind = moon::TerminatorKind::Jump;
    if (cfgVerifier.verify(*contextCfg, compositionModule))
        return fail("CFG verifier accepted ordinary context jump into a continuation");
    contextResume->kind = moon::TerminatorKind::Resume;
    contextRegion->fragment = interceptorRef;
    if (cfgVerifier.verify(*contextCfg, compositionModule))
        return fail("CFG verifier accepted context control under an interceptor contract");
    contextRegion->fragment = contextRef;
    if (!cfgVerifier.verify(*contextCfg, compositionModule))
        return fail("restored static context no longer verifies");

    auto blocklessBody = std::make_unique<moon::BlockStmt>();
    auto blocklessApply = std::make_unique<moon::ApplyStmt>();
    blocklessApply->slotName = "hook";
    blocklessApply->fragmentName = "guard";
    blocklessApply->fragmentRef = interceptorRef;
    blocklessBody->stmts.push_back(std::move(blocklessApply));
    auto blocklessCfg = cfgBuilder.build(
        std::move(blocklessBody), {}, moon::RegionKind::Function,
        compositionModule);
    if (!blocklessCfg ||
        !cfgVerifier.verify(*blocklessCfg, compositionModule))
        return fail("canonical builder rejected lexical statement-form apply");


    return 0;
}

} // namespace canonical_test
