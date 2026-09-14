#include "moonir/MoonIR.h"
#include "moonir/ControlFlowBuilder.h"
#include "moonir/Verifier.h"
#include "core/TypeLayout.h"
#include "moonir_canonical_test_support.h"

#include <algorithm>
#include <iostream>

namespace canonical_test {

int runControlFlowTests(ControlFlowTestContext& context) {
    auto& module = context.module;
    auto& cfgVerifier = context.verifier;
    auto& cfgBuilder = context.builder;
    const auto i32Id = context.i32Id;
    const auto stringId = context.stringId;
    const auto guardedArrayId = context.guardedArrayId;
    const auto boolId = context.boolId;
    const auto unitId = context.unitId;
    const auto resultI32BoolId = context.resultI32BoolId;
    const auto unitConsumerTypeId = context.unitConsumerTypeId;
    const auto choiceId = context.choiceId;

    auto structured = std::make_unique<moon::BlockStmt>();
    auto binding = std::make_unique<moon::LetStmt>();
    binding->name = "owned";
    binding->usage = luna::ownership::Usage::Affine;
    binding->type = stringId;
    auto text = std::make_unique<moon::StringLiteralExpr>();
    text->value = "cfg";
    text->type = stringId;
    binding->initializer = std::move(text);
    structured->stmts.push_back(std::move(binding));
    auto use = std::make_unique<moon::ExprStmt>();
    auto usedIdentifier = std::make_unique<moon::IdentifierExpr>();
    usedIdentifier->name = "owned";
    usedIdentifier->type = stringId;
    use->expr = std::move(usedIdentifier);
    structured->stmts.push_back(std::move(use));
    auto branch = std::make_unique<moon::IfStmt>();
    auto condition = std::make_unique<moon::BoolLiteralExpr>();
    condition->value = true;
    condition->type = boolId;
    branch->cond = std::move(condition);
    branch->thenBlock = std::make_unique<moon::BlockStmt>();
    branch->elseBranch = std::make_unique<moon::BlockStmt>();
    structured->stmts.push_back(std::move(branch));
    auto loop = std::make_unique<moon::WhileStmt>();
    auto loopCondition = std::make_unique<moon::BoolLiteralExpr>();
    loopCondition->value = false;
    loopCondition->type = boolId;
    loop->cond = std::move(loopCondition);
    loop->body = std::make_unique<moon::BlockStmt>();
    structured->stmts.push_back(std::move(loop));
    auto match = std::make_unique<moon::MatchStmt>();
    match->matchedType = choiceId;
    auto selected = std::make_unique<moon::VariantConstructExpr>();
    selected->typeName = "Choice";
    selected->variantName = "Some";
    selected->constructedType = choiceId;
    selected->type = choiceId;
    auto selectedValue = std::make_unique<moon::IntLiteralExpr>();
    selectedValue->value = 7;
    selectedValue->type = i32Id;
    selected->args.push_back(std::move(selectedValue));
    match->scrutinee = std::move(selected);
    moon::MatchArm noneArm;
    noneArm.variantName = "None";
    noneArm.variantIndex = 0;
    noneArm.body = std::make_unique<moon::BlockStmt>();
    match->arms.push_back(std::move(noneArm));
    moon::MatchArm someArm;
    someArm.variantName = "Some";
    someArm.variantIndex = 1;
    someArm.bindings = {"value"};
    someArm.bindingTypes = {i32Id};
    someArm.bindingUsages = {luna::ownership::Usage::Copy};
    someArm.body = std::make_unique<moon::BlockStmt>();
    auto patternUse = std::make_unique<moon::ExprStmt>();
    auto patternIdentifier = std::make_unique<moon::IdentifierExpr>();
    patternIdentifier->name = "value";
    patternIdentifier->type = i32Id;
    patternUse->expr = std::move(patternIdentifier);
    someArm.body->stmts.push_back(std::move(patternUse));
    match->arms.push_back(std::move(someArm));
    structured->stmts.push_back(std::move(match));
    auto implicitCleanup = std::make_unique<moon::FreeStmt>();
    implicitCleanup->isImplicit = true;
    implicitCleanup->action = luna::ownership::CleanupAction::Deallocate;
    auto cleanedIdentifier = std::make_unique<moon::IdentifierExpr>();
    cleanedIdentifier->name = "owned";
    cleanedIdentifier->type = stringId;
    implicitCleanup->operand = std::move(cleanedIdentifier);
    structured->stmts.push_back(std::move(implicitCleanup));

    auto loweredCfg = cfgBuilder.build(
        std::move(structured), {}, moon::RegionKind::Function, module);
    if (!loweredCfg || !cfgVerifier.verify(*loweredCfg, module))
        return fail("structured if/fallthrough cleanup did not become canonical CFG");
    if (loweredCfg->blocks.size() != 11 || loweredCfg->cleanups.size() != 1 ||
        loweredCfg->blocks.front().operations.size() != 2 ||
        loweredCfg->blocks.back().terminator.kind !=
            moon::TerminatorKind::Return)
        return fail("canonical CFG builder retained structured control or cleanup operations");
    auto* loweredUse = dynamic_cast<moon::ExprStmt*>(
        loweredCfg->blocks.front().operations[1].get());
    auto* loweredUseId = loweredUse
        ? dynamic_cast<moon::IdentifierExpr*>(loweredUse->expr.get()) : nullptr;
    if (!loweredUseId || loweredUseId->local != moon::LocalId{0})
        return fail("canonical CFG builder did not bind a stable LocalId");
    loweredUseId->local = moon::LocalId{99};
    if (cfgVerifier.verify(*loweredCfg, module))
        return fail("CFG verifier accepted a forged LocalId use");
    loweredUseId->local = {};
    if (cfgVerifier.verify(*loweredCfg, module))
        return fail("CFG verifier accepted an unresolved local identifier");
    loweredUseId->local = moon::LocalId{0};

    moon::Param guardedArrayParameter;
    guardedArrayParameter.name = "values";
    guardedArrayParameter.usage = luna::ownership::Usage::Affine;
    guardedArrayParameter.relation = luna::ownership::Relation::Owned;
    guardedArrayParameter.type = guardedArrayId;
    auto guardedLoopBody = std::make_unique<moon::BlockStmt>();
    auto guardedLoop = std::make_unique<moon::ForStmt>();
    guardedLoop->varName = "value";
    guardedLoop->elementType = stringId;
    guardedLoop->bindingUsage = luna::ownership::Usage::Affine;
    guardedLoop->recipeStateName = "$for.recipe.values";
    guardedLoop->recipeSourceType = guardedArrayId;
    auto guardedSourceMove = std::make_unique<moon::MoveExpr>();
    guardedSourceMove->type = guardedArrayId;
    auto guardedSourceIdentifier =
        std::make_unique<moon::IdentifierExpr>();
    guardedSourceIdentifier->name = "values";
    guardedSourceIdentifier->type = guardedArrayId;
    guardedSourceMove->operand = std::move(guardedSourceIdentifier);
    guardedLoop->iterable = std::move(guardedSourceMove);
    guardedLoop->body = std::make_unique<moon::BlockStmt>();
    auto guardedEarlyReturn = std::make_unique<moon::IfStmt>();
    auto guardedEarlyReturnCondition =
        std::make_unique<moon::BoolLiteralExpr>();
    guardedEarlyReturnCondition->value = false;
    guardedEarlyReturnCondition->type = boolId;
    guardedEarlyReturn->cond = std::move(guardedEarlyReturnCondition);
    guardedEarlyReturn->thenBlock = std::make_unique<moon::BlockStmt>();
    auto guardedLoopReturn = std::make_unique<moon::ReturnStmt>();
    guardedLoopReturn->cleanups.push_back({
        "value", luna::ownership::CleanupAction::Deallocate, stringId});
    guardedEarlyReturn->thenBlock->stmts.push_back(
        std::move(guardedLoopReturn));
    guardedLoop->body->stmts.push_back(
        std::move(guardedEarlyReturn));
    auto guardedItemCleanup = std::make_unique<moon::FreeStmt>();
    guardedItemCleanup->isImplicit = true;
    guardedItemCleanup->action =
        luna::ownership::CleanupAction::Deallocate;
    auto guardedItemIdentifier =
        std::make_unique<moon::IdentifierExpr>();
    guardedItemIdentifier->name = "value";
    guardedItemIdentifier->type = stringId;
    guardedItemCleanup->operand = std::move(guardedItemIdentifier);
    guardedLoop->body->stmts.push_back(std::move(guardedItemCleanup));
    guardedLoopBody->stmts.push_back(std::move(guardedLoop));
    auto guardedLoopCfg = cfgBuilder.build(
        std::move(guardedLoopBody), {guardedArrayParameter},
        moon::RegionKind::Function, module);
    moon::MoveExpr* guardedElementTransfer = nullptr;
    moon::LocalId guardedNextUnread;
    std::vector<moon::CleanupId> guardedTailCleanups;
    std::vector<moon::CleanupId>* guardedExhaustionCleanups = nullptr;
    std::vector<moon::CleanupId>* guardedReturnCleanups = nullptr;
    if (guardedLoopCfg) {
        for (const auto& cleanup : guardedLoopCfg->cleanups) {
            if (!cleanup.guard) continue;
            guardedTailCleanups.push_back(cleanup.id);
            guardedNextUnread = cleanup.guard->nextUnread;
        }
        for (auto& block : guardedLoopCfg->blocks) {
            for (auto& operation : block.operations) {
                auto* declaration = dynamic_cast<moon::LetStmt*>(
                    operation.get());
                auto* transfer = declaration
                    ? dynamic_cast<moon::MoveExpr*>(
                          declaration->initializer.get()) : nullptr;
                if (transfer && !transfer->nextUnread.empty())
                    guardedElementTransfer = transfer;
            }
            const auto* condition = dynamic_cast<moon::BinaryExpr*>(
                block.terminator.operand.get());
            if (condition && condition->op == moon::Operator::Less)
                guardedExhaustionCleanups =
                    &block.terminator.secondary.cleanups;
            if (block.terminator.kind == moon::TerminatorKind::Return &&
                !block.terminator.exitCleanups.empty())
                guardedReturnCleanups =
                    &block.terminator.exitCleanups;
        }
    }
    const auto cleanupElementOrder = [&guardedLoopCfg](
        const std::vector<moon::CleanupId>& cleanups) {
        std::vector<uint64_t> result;
        if (!guardedLoopCfg) return result;
        for (const auto cleanupId : cleanups) {
            const auto* cleanup = guardedLoopCfg->findCleanup(cleanupId);
            if (cleanup && cleanup->guard)
                result.push_back(cleanup->guard->elementIndex);
        }
        return result;
    };
    const auto exhaustionElementOrder = guardedExhaustionCleanups
        ? cleanupElementOrder(*guardedExhaustionCleanups)
        : std::vector<uint64_t>{};
    const bool returnClosesTail = guardedReturnCleanups &&
        std::all_of(
            guardedTailCleanups.begin(), guardedTailCleanups.end(),
            [&](moon::CleanupId cleanup) {
                return std::find(
                    guardedReturnCleanups->begin(),
                    guardedReturnCleanups->end(), cleanup) !=
                    guardedReturnCleanups->end();
            });
    if (!guardedLoopCfg ||
        !cfgVerifier.verify(*guardedLoopCfg, module) ||
        guardedNextUnread.empty() || !guardedElementTransfer ||
        guardedElementTransfer->nextUnread != guardedNextUnread ||
        guardedTailCleanups.size() != 2 ||
        exhaustionElementOrder != std::vector<uint64_t>{0, 1} ||
        !returnClosesTail)
        return fail("move-only array loop did not lower to guarded tail cleanup state");
    guardedElementTransfer->nextUnread = {};
    if (cfgVerifier.verify(*guardedLoopCfg, module))
        return fail("CFG verifier accepted an unguarded dynamic element move");
    guardedElementTransfer->nextUnread = guardedNextUnread;
    const auto savedGuardedExhaustion = *guardedExhaustionCleanups;
    guardedExhaustionCleanups->pop_back();
    if (cfgVerifier.verify(*guardedLoopCfg, module))
        return fail("CFG verifier accepted an unread array-tail leak");
    *guardedExhaustionCleanups = savedGuardedExhaustion;
    if (!cfgVerifier.verify(*guardedLoopCfg, module))
        return fail("restored guarded consuming-array CFG no longer verifies");

    auto standaloneBlockRoot = std::make_unique<moon::BlockStmt>();
    auto standaloneBlockUse = std::make_unique<moon::ExprStmt>();
    auto standaloneBlock = std::make_unique<moon::BlockExpr>();
    standaloneBlock->type = unitId;
    standaloneBlock->block = std::make_unique<moon::BlockStmt>();
    standaloneBlockUse->expr = std::move(standaloneBlock);
    standaloneBlockRoot->stmts.push_back(std::move(standaloneBlockUse));
    auto standaloneBlockCfg = cfgBuilder.build(
        std::move(standaloneBlockRoot), {},
        moon::RegionKind::Function, module);
    if (!standaloneBlockCfg ||
        !cfgVerifier.verify(*standaloneBlockCfg, module))
        return fail(
            "unit block expression did not expand into lexical CFG");

    auto ifExpressionRoot = std::make_unique<moon::BlockStmt>();
    auto ifExpressionUse = std::make_unique<moon::ExprStmt>();
    auto ifExpression = std::make_unique<moon::IfExpr>();
    ifExpression->type = unitId;
    auto outerCondition = std::make_unique<moon::BoolLiteralExpr>();
    outerCondition->value = true;
    outerCondition->type = boolId;
    ifExpression->cond = std::move(outerCondition);
    auto outerThen = std::make_unique<moon::BlockExpr>();
    outerThen->type = unitId;
    outerThen->block = std::make_unique<moon::BlockStmt>();
    ifExpression->thenExpr = std::move(outerThen);
    auto nestedIf = std::make_unique<moon::IfExpr>();
    nestedIf->type = unitId;
    auto nestedCondition = std::make_unique<moon::BoolLiteralExpr>();
    nestedCondition->value = false;
    nestedCondition->type = boolId;
    nestedIf->cond = std::move(nestedCondition);
    auto nestedThen = std::make_unique<moon::BlockExpr>();
    nestedThen->type = unitId;
    nestedThen->block = std::make_unique<moon::BlockStmt>();
    nestedIf->thenExpr = std::move(nestedThen);
    auto nestedElse = std::make_unique<moon::BlockExpr>();
    nestedElse->type = unitId;
    nestedElse->block = std::make_unique<moon::BlockStmt>();
    nestedIf->elseExpr = std::move(nestedElse);
    ifExpression->elseExpr = std::move(nestedIf);
    ifExpressionUse->expr = std::move(ifExpression);
    ifExpressionRoot->stmts.push_back(std::move(ifExpressionUse));
    auto ifExpressionCfg = cfgBuilder.build(
        std::move(ifExpressionRoot), {},
        moon::RegionKind::Function, module);
    size_t expressionBranches = 0;
    size_t unitResults = 0;
    if (ifExpressionCfg) {
        for (const auto& block : ifExpressionCfg->blocks) {
            if (block.terminator.kind == moon::TerminatorKind::Branch)
                ++expressionBranches;
            for (const auto& operation : block.operations) {
                const auto* statement =
                    dynamic_cast<const moon::ExprStmt*>(operation.get());
                if (statement &&
                    dynamic_cast<const moon::UnitExpr*>(statement->expr.get()))
                    ++unitResults;
            }
        }
    }
    if (!ifExpressionCfg ||
        !cfgVerifier.verify(*ifExpressionCfg, module) ||
        expressionBranches != 2 || unitResults != 1)
        return fail(
            "nested unit if expression did not become canonical branch CFG");

    auto argumentControlRoot = std::make_unique<moon::BlockStmt>();
    auto argumentControlUse = std::make_unique<moon::ExprStmt>();
    auto argumentControlCall = std::make_unique<moon::CallExpr>();
    argumentControlCall->type = unitId;
    auto argumentConsumer = std::make_unique<moon::IdentifierExpr>();
    argumentConsumer->name = "unitConsumer";
    argumentConsumer->type = unitConsumerTypeId;
    argumentControlCall->callee = std::move(argumentConsumer);
    auto argumentIf = std::make_unique<moon::IfExpr>();
    argumentIf->type = unitId;
    auto argumentCondition = std::make_unique<moon::BoolLiteralExpr>();
    argumentCondition->value = true;
    argumentCondition->type = boolId;
    argumentIf->cond = std::move(argumentCondition);
    auto argumentThen = std::make_unique<moon::BlockExpr>();
    argumentThen->type = unitId;
    argumentThen->block = std::make_unique<moon::BlockStmt>();
    argumentIf->thenExpr = std::move(argumentThen);
    auto argumentElse = std::make_unique<moon::BlockExpr>();
    argumentElse->type = unitId;
    argumentElse->block = std::make_unique<moon::BlockStmt>();
    argumentIf->elseExpr = std::move(argumentElse);
    argumentControlCall->args.push_back(std::move(argumentIf));
    argumentControlUse->expr = std::move(argumentControlCall);
    argumentControlRoot->stmts.push_back(std::move(argumentControlUse));
    moon::Param unitConsumerParameter;
    unitConsumerParameter.name = "unitConsumer";
    unitConsumerParameter.type = unitConsumerTypeId;
    auto argumentControlCfg = cfgBuilder.build(
        std::move(argumentControlRoot), {unitConsumerParameter},
        moon::RegionKind::Function, module);
    if (!argumentControlCfg ||
        !cfgVerifier.verify(*argumentControlCfg, module)) {
        for (const auto& message : cfgBuilder.errors())
            std::cerr << message << '\n';
        for (const auto& message : cfgVerifier.errors())
            std::cerr << message << '\n';
        return fail(
            "if expression argument did not preserve ordered CFG evaluation");
    }

    auto invalidIfRoot = std::make_unique<moon::BlockStmt>();
    auto invalidIfUse = std::make_unique<moon::ExprStmt>();
    auto invalidIf = std::make_unique<moon::IfExpr>();
    invalidIf->type = i32Id;
    auto invalidCondition = std::make_unique<moon::BoolLiteralExpr>();
    invalidCondition->value = true;
    invalidCondition->type = boolId;
    invalidIf->cond = std::move(invalidCondition);
    auto invalidThen = std::make_unique<moon::BlockExpr>();
    invalidThen->type = i32Id;
    invalidThen->block = std::make_unique<moon::BlockStmt>();
    invalidIf->thenExpr = std::move(invalidThen);
    auto invalidElse = std::make_unique<moon::BlockExpr>();
    invalidElse->type = i32Id;
    invalidElse->block = std::make_unique<moon::BlockStmt>();
    invalidIf->elseExpr = std::move(invalidElse);
    invalidIfUse->expr = std::move(invalidIf);
    invalidIfRoot->stmts.push_back(std::move(invalidIfUse));
    if (cfgBuilder.build(
            std::move(invalidIfRoot), {},
            moon::RegionKind::Function, module))
        return fail(
            "CFG builder accepted a non-unit block-style if expression");

    auto tryRoot = std::make_unique<moon::BlockStmt>();
    auto tryBinding = std::make_unique<moon::LetStmt>();
    tryBinding->name = "unwrapped";
    tryBinding->type = i32Id;
    auto propagation = std::make_unique<moon::TryExpr>();
    propagation->type = i32Id;
    propagation->resultType = resultI32BoolId;
    propagation->propagatedResultType = resultI32BoolId;
    propagation->valueType = i32Id;
    propagation->errorType = boolId;
    propagation->propagatedErrorType = boolId;
    auto tryOperand = std::make_unique<moon::IdentifierExpr>();
    tryOperand->name = "input";
    tryOperand->type = resultI32BoolId;
    propagation->operand = std::move(tryOperand);
    tryBinding->initializer = std::move(propagation);
    tryRoot->stmts.push_back(std::move(tryBinding));
    moon::Param tryParameter;
    tryParameter.name = "input";
    tryParameter.type = resultI32BoolId;
    auto tryCfg = cfgBuilder.build(
        std::move(tryRoot), {tryParameter},
        moon::RegionKind::Function, module);
    moon::ResultConstructExpr* propagatedResult = nullptr;
    size_t trySwitches = 0;
    if (tryCfg) {
        for (auto& block : tryCfg->blocks) {
            if (block.terminator.kind == moon::TerminatorKind::Switch)
                ++trySwitches;
            if (block.terminator.kind == moon::TerminatorKind::Return)
                if (auto* result =
                        dynamic_cast<moon::ResultConstructExpr*>(
                            block.terminator.operand.get()))
                    propagatedResult = result;
        }
    }
    if (!tryCfg || !cfgVerifier.verify(*tryCfg, module) ||
        trySwitches != 1 || !propagatedResult ||
        propagatedResult->isOk)
        return fail(
            "Try expression did not become Result switch and early return CFG");
    propagatedResult->isOk = true;
    if (cfgVerifier.verify(*tryCfg, module))
        return fail(
            "CFG verifier accepted a Result tag/payload mismatch");
    propagatedResult->isOk = false;
    if (!cfgVerifier.verify(*tryCfg, module))
        return fail(
            "restored Try expression CFG did not verify");

    moon::Module convertedTryModule;
    convertedTryModule.name = "canonical.try";
    const auto convertedI32Id = convertedTryModule.registerType(TyI32);
    const auto convertedBoolId = convertedTryModule.registerType(TyBool);
    const auto convertedStringId = convertedTryModule.registerType(TyString);
    auto sourceResultType = Type::makeResult(TyI32, TyString);
    auto targetResultType = Type::makeResult(TyI32, TyBool);
    const auto sourceResultId = convertedTryModule.registerType(
        sourceResultType);
    const auto targetResultId = convertedTryModule.registerType(
        targetResultType);
    auto fromType = Type::makeFunction(
        {TyString}, TyBool,
        {{luna::ownership::Relation::Owned,
          luna::ownership::Usage::Affine}},
        {luna::ownership::Relation::Owned,
         luna::ownership::Usage::Copy});
    const auto fromTypeId = convertedTryModule.registerType(fromType);
    moon::DeclarationRecord fromRecord;
    fromRecord.id = "canonical.try::fn::from_string";
    fromRecord.familyId = fromRecord.id;
    fromRecord.symbolId = luna::identity::symbolIdFromCanonical(
        fromRecord.id);
    fromRecord.sourceName = "from_string";
    fromRecord.linkageName = "from_string";
    fromRecord.kind = moon::DeclarationKind::Function;
    fromRecord.type = fromTypeId;
    fromRecord.sysmeta = fromType->sysmeta;
    convertedTryModule.declarationTable.push_back(
        std::move(fromRecord));
    convertedTryModule.sealTypeTable();
    const auto* frozenFrom = convertedTryModule.findDeclarationById(
        "canonical.try::fn::from_string");
    if (!frozenFrom)
        return fail("converted Try fixture lost its From witness");
    const moon::DeclarationRef fromRef{
        frozenFrom->symbolId, frozenFrom->contractId};

    auto convertedTryRoot = std::make_unique<moon::BlockStmt>();
    auto convertedTryBinding = std::make_unique<moon::LetStmt>();
    convertedTryBinding->name = "converted";
    convertedTryBinding->type = convertedI32Id;
    auto convertedPropagation = std::make_unique<moon::TryExpr>();
    convertedPropagation->type = convertedI32Id;
    convertedPropagation->resultType = sourceResultId;
    convertedPropagation->propagatedResultType = targetResultId;
    convertedPropagation->valueType = convertedI32Id;
    convertedPropagation->errorType = convertedStringId;
    convertedPropagation->propagatedErrorType = convertedBoolId;
    convertedPropagation->errorConversion = fromRef;
    convertedPropagation->cleanups.push_back({
        "outer", luna::ownership::CleanupAction::Deallocate,
        convertedStringId});
    auto convertedOperand = std::make_unique<moon::IdentifierExpr>();
    convertedOperand->name = "fallible";
    convertedOperand->type = sourceResultId;
    convertedPropagation->operand = std::move(convertedOperand);
    convertedTryBinding->initializer = std::move(convertedPropagation);
    convertedTryRoot->stmts.push_back(std::move(convertedTryBinding));
    auto outerCleanup = std::make_unique<moon::FreeStmt>();
    outerCleanup->isImplicit = true;
    outerCleanup->action = luna::ownership::CleanupAction::Deallocate;
    auto outerCleanupOperand = std::make_unique<moon::IdentifierExpr>();
    outerCleanupOperand->name = "outer";
    outerCleanupOperand->type = convertedStringId;
    outerCleanup->operand = std::move(outerCleanupOperand);
    convertedTryRoot->stmts.push_back(std::move(outerCleanup));
    moon::Param fallibleParameter;
    fallibleParameter.name = "fallible";
    fallibleParameter.type = sourceResultId;
    fallibleParameter.usage = convertedTryModule.findType(
        sourceResultId)->sysmeta.resource.usage;
    moon::Param outerParameter;
    outerParameter.name = "outer";
    outerParameter.type = convertedStringId;
    outerParameter.usage = luna::ownership::Usage::Affine;
    auto convertedTryCfg = cfgBuilder.build(
        std::move(convertedTryRoot),
        {fallibleParameter, outerParameter},
        moon::RegionKind::Function, convertedTryModule);
    moon::Terminator* convertedFailure = nullptr;
    moon::CallExpr* convertedCall = nullptr;
    if (convertedTryCfg) {
        for (auto& block : convertedTryCfg->blocks) {
            auto* result = dynamic_cast<moon::ResultConstructExpr*>(
                block.terminator.operand.get());
            if (!result) continue;
            convertedFailure = &block.terminator;
            convertedCall = dynamic_cast<moon::CallExpr*>(
                result->payload.get());
        }
    }
    if (!convertedTryCfg) {
        for (const auto& message : cfgBuilder.errors())
            std::cerr << message << '\n';
        return fail(
            "converted Try did not build canonical CFG");
    }
    if (!cfgVerifier.verify(*convertedTryCfg, convertedTryModule)) {
        for (const auto& message : cfgVerifier.errors())
            std::cerr << message << '\n';
        return fail("converted Try CFG did not verify");
    }
    if (!convertedFailure || !convertedCall ||
        convertedCall->calleeRef != fromRef ||
        convertedFailure->exitCleanups.size() != 1)
        return fail(
            "converted Try did not preserve From and cleanup evidence");
    convertedFailure->exitCleanups.clear();
    if (cfgVerifier.verify(*convertedTryCfg, convertedTryModule))
        return fail(
            "CFG verifier accepted a converted Try without return cleanup");

    auto unresolvedStructured = std::make_unique<moon::BlockStmt>();
    auto unresolvedUse = std::make_unique<moon::ExprStmt>();
    auto unresolvedIdentifier = std::make_unique<moon::IdentifierExpr>();
    unresolvedIdentifier->name = "missing";
    unresolvedIdentifier->type = i32Id;
    unresolvedUse->expr = std::move(unresolvedIdentifier);
    unresolvedStructured->stmts.push_back(std::move(unresolvedUse));
    if (cfgBuilder.build(
            std::move(unresolvedStructured), {},
            moon::RegionKind::Function, module))
        return fail("CFG builder accepted an unresolved local identifier");

    auto movedStructured = std::make_unique<moon::BlockStmt>();
    auto sourceBinding = std::make_unique<moon::LetStmt>();
    sourceBinding->name = "source";
    sourceBinding->usage = luna::ownership::Usage::Affine;
    sourceBinding->type = stringId;
    auto sourceValue = std::make_unique<moon::StringLiteralExpr>();
    sourceValue->value = "move";
    sourceValue->type = stringId;
    sourceBinding->initializer = std::move(sourceValue);
    movedStructured->stmts.push_back(std::move(sourceBinding));
    auto destinationBinding = std::make_unique<moon::LetStmt>();
    destinationBinding->name = "destination";
    destinationBinding->usage = luna::ownership::Usage::Affine;
    destinationBinding->type = stringId;
    auto movedValue = std::make_unique<moon::MoveExpr>();
    auto movedIdentifier = std::make_unique<moon::IdentifierExpr>();
    movedIdentifier->name = "source";
    movedIdentifier->type = stringId;
    movedValue->operand = std::move(movedIdentifier);
    movedValue->type = stringId;
    destinationBinding->initializer = std::move(movedValue);
    movedStructured->stmts.push_back(std::move(destinationBinding));
    auto destinationCleanup = std::make_unique<moon::FreeStmt>();
    destinationCleanup->isImplicit = true;
    destinationCleanup->action = luna::ownership::CleanupAction::Deallocate;
    auto destinationIdentifier = std::make_unique<moon::IdentifierExpr>();
    destinationIdentifier->name = "destination";
    destinationIdentifier->type = stringId;
    destinationCleanup->operand = std::move(destinationIdentifier);
    movedStructured->stmts.push_back(std::move(destinationCleanup));
    auto movedCfg = cfgBuilder.build(
        std::move(movedStructured), {}, moon::RegionKind::Function, module);
    if (!movedCfg || !cfgVerifier.verify(*movedCfg, module) ||
        movedCfg->cleanups.size() != 2 ||
        movedCfg->blocks.front().terminator.exitCleanups !=
            std::vector<moon::CleanupId>{moon::CleanupId{1}})
        return fail("CFG cleanup dataflow lost a whole-place move transfer");
    movedCfg->blocks.front().terminator.exitCleanups = {moon::CleanupId{0}};
    if (cfgVerifier.verify(*movedCfg, module))
        return fail("CFG verifier accepted cleanup of a moved source place");

    if (const int result = runIteratorTests(context)) return result;
    if (const int result = runReferenceAndClosureTests(context)) return result;

    return 0;
}

} // namespace canonical_test
