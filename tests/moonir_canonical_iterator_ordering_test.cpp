#include "core/TypeLayout.h"
#include "moonir/ControlFlowBuilder.h"
#include "moonir/MoonIR.h"
#include "moonir/Verifier.h"
#include "moonir_canonical_test_support.h"

#include <algorithm>
#include <iostream>

namespace canonical_test {

int runIteratorOrderingTests(ControlFlowTestContext& context) {
    auto& module = context.module;
    auto& cfgVerifier = context.verifier;
    auto& cfgBuilder = context.builder;
    const auto i32Id = context.i32Id;
    const auto stringId = context.stringId;
    const auto boolId = context.boolId;
    const auto unitId = context.unitId;
    const auto resultI32BoolId = context.resultI32BoolId;
    const auto reducerTypeId = context.reducerTypeId;
    const auto affineReducerTypeId = context.affineReducerTypeId;
    const auto linearReducerTypeId = context.linearReducerTypeId;
    const auto affineValueProducerTypeId = context.affineValueProducerTypeId;
    const auto actionTypeId = context.actionTypeId;
    const auto unitOrderedConsumerTypeId = context.unitOrderedConsumerTypeId;

    moon::Param tryParameter;
    tryParameter.name = "input";
    tryParameter.type = resultI32BoolId;
    moon::Param reducerParameter;
    reducerParameter.name = "reducer";
    reducerParameter.type = reducerTypeId;
    moon::Param affineReducerParameter;
    affineReducerParameter.name = "affineReducer";
    affineReducerParameter.type = affineReducerTypeId;

    auto directCountStructured = std::make_unique<moon::BlockStmt>();
    auto directCountReturn = std::make_unique<moon::ReturnStmt>();
    directCountReturn->value =
        makeDirectRangeTerminal(context, IteratorOp::Count, i32Id, i32Id, false);
    directCountStructured->stmts.push_back(std::move(directCountReturn));
    auto directCountCfg =
        cfgBuilder.build(std::move(directCountStructured), {}, moon::RegionKind::Function, module);
    bool directCountReachedReturn = false;
    if (directCountCfg)
        for (const auto& block : directCountCfg->blocks)
            if (block.terminator.kind == moon::TerminatorKind::Return)
                if (const auto* value =
                        dynamic_cast<const moon::IdentifierExpr*>(block.terminator.operand.get()))
                    directCountReachedReturn = value->name.rfind("$terminal.count.", 0) == 0;
    if (!directCountCfg || !cfgVerifier.verify(*directCountCfg, module) ||
        !directCountReachedReturn)
        return fail("direct count did not normalize before return");

    auto siblingBoundary = std::make_unique<moon::BlockStmt>();
    auto siblingUse = std::make_unique<moon::ExprStmt>();
    auto siblingCall = std::make_unique<moon::CallExpr>();
    auto siblingReducer = std::make_unique<moon::IdentifierExpr>();
    siblingReducer->name = "reducer";
    siblingReducer->type = reducerTypeId;
    siblingCall->callee = std::move(siblingReducer);
    siblingCall->type = i32Id;
    auto earlierSibling = std::make_unique<moon::IntLiteralExpr>();
    earlierSibling->value = 9;
    earlierSibling->type = i32Id;
    siblingCall->args.push_back(std::move(earlierSibling));
    siblingCall->args.push_back(
        makeDirectRangeTerminal(context, IteratorOp::Count, i32Id, i32Id, false));
    siblingUse->expr = std::move(siblingCall);
    siblingBoundary->stmts.push_back(std::move(siblingUse));
    auto siblingCfg = cfgBuilder.build(std::move(siblingBoundary), {reducerParameter},
                                       moon::RegionKind::Function, module);
    bool hoistedSiblingCallee = false;
    bool siblingTerminalReachedCall = false;
    if (siblingCfg)
        for (const auto& block : siblingCfg->blocks)
            for (const auto& operation : block.operations) {
                if (const auto* declaration = dynamic_cast<const moon::LetStmt*>(operation.get());
                    declaration && declaration->name.rfind("$expression.hoist.", 0) == 0) {
                    const auto* source =
                        dynamic_cast<const moon::IdentifierExpr*>(declaration->initializer.get());
                    hoistedSiblingCallee =
                        hoistedSiblingCallee || (source && source->name == "reducer");
                }
                const auto* expression = dynamic_cast<const moon::ExprStmt*>(operation.get());
                const auto* call = expression
                                       ? dynamic_cast<const moon::CallExpr*>(expression->expr.get())
                                       : nullptr;
                if (!call || call->args.size() != 2) continue;
                const auto* result = dynamic_cast<const moon::IdentifierExpr*>(call->args[1].get());
                siblingTerminalReachedCall =
                    result && result->name.rfind("$terminal.count.", 0) == 0;
            }
    if (!siblingCfg || !cfgVerifier.verify(*siblingCfg, module) || !hoistedSiblingCallee ||
        !siblingTerminalReachedCall)
        return fail("Copy call sibling did not hoist before its terminal CFG");

    auto unitSiblingBoundary = std::make_unique<moon::BlockStmt>();
    auto unitSiblingUse = std::make_unique<moon::ExprStmt>();
    auto unitSiblingCall = std::make_unique<moon::CallExpr>();
    unitSiblingCall->type = unitId;
    auto unitSiblingConsumer = std::make_unique<moon::IdentifierExpr>();
    unitSiblingConsumer->name = "unitOrderedConsumer";
    unitSiblingConsumer->type = unitOrderedConsumerTypeId;
    unitSiblingCall->callee = std::move(unitSiblingConsumer);
    auto earlierUnitCall = std::make_unique<moon::CallExpr>();
    earlierUnitCall->type = unitId;
    auto earlierUnitAction = std::make_unique<moon::IdentifierExpr>();
    earlierUnitAction->name = "action";
    earlierUnitAction->type = actionTypeId;
    earlierUnitCall->callee = std::move(earlierUnitAction);
    auto earlierUnitArgument = std::make_unique<moon::IntLiteralExpr>();
    earlierUnitArgument->value = 7;
    earlierUnitArgument->type = i32Id;
    earlierUnitCall->args.push_back(std::move(earlierUnitArgument));
    unitSiblingCall->args.push_back(std::move(earlierUnitCall));
    unitSiblingCall->args.push_back(
        makeDirectRangeTerminal(context, IteratorOp::Count, i32Id, i32Id, false));
    unitSiblingUse->expr = std::move(unitSiblingCall);
    unitSiblingBoundary->stmts.push_back(std::move(unitSiblingUse));
    moon::Param unitOrderedConsumerParameter;
    unitOrderedConsumerParameter.name = "unitOrderedConsumer";
    unitOrderedConsumerParameter.type = unitOrderedConsumerTypeId;
    moon::Param unitSiblingActionParameter;
    unitSiblingActionParameter.name = "action";
    unitSiblingActionParameter.type = actionTypeId;
    auto unitSiblingCfg = cfgBuilder.build(
        std::move(unitSiblingBoundary), {unitOrderedConsumerParameter, unitSiblingActionParameter},
        moon::RegionKind::Function, module);
    size_t sequencedUnitCalls = 0;
    bool unitPlaceholderReachedParent = false;
    if (unitSiblingCfg)
        for (const auto& block : unitSiblingCfg->blocks)
            for (const auto& operation : block.operations) {
                const auto* statement = dynamic_cast<const moon::ExprStmt*>(operation.get());
                const auto* call = statement
                                       ? dynamic_cast<const moon::CallExpr*>(statement->expr.get())
                                       : nullptr;
                if (!call) continue;
                const auto* callee = dynamic_cast<const moon::IdentifierExpr*>(call->callee.get());
                if (callee && callee->name == "action") ++sequencedUnitCalls;
                if (call->args.size() != 2) continue;
                const auto* result = dynamic_cast<const moon::IdentifierExpr*>(call->args[1].get());
                unitPlaceholderReachedParent =
                    dynamic_cast<const moon::UnitExpr*>(call->args[0].get()) && result &&
                    result->name.rfind("$terminal.count.", 0) == 0;
            }
    if (!unitSiblingCfg || !cfgVerifier.verify(*unitSiblingCfg, module) ||
        sequencedUnitCalls != 1 || !unitPlaceholderReachedParent)
        return fail("unit sibling did not sequence once before its terminal CFG");

    auto binarySiblingBoundary = std::make_unique<moon::BlockStmt>();
    auto binaryReturn = std::make_unique<moon::ReturnStmt>();
    auto binary = std::make_unique<moon::BinaryExpr>();
    binary->op = moon::Operator::Add;
    binary->type = i32Id;
    auto earlierCall = std::make_unique<moon::CallExpr>();
    auto earlierReducer = std::make_unique<moon::IdentifierExpr>();
    earlierReducer->name = "reducer";
    earlierReducer->type = reducerTypeId;
    earlierCall->callee = std::move(earlierReducer);
    earlierCall->type = i32Id;
    for (int64_t value : {2, 3}) {
        auto argument = std::make_unique<moon::IntLiteralExpr>();
        argument->value = value;
        argument->type = i32Id;
        earlierCall->args.push_back(std::move(argument));
    }
    binary->lhs = std::move(earlierCall);
    binary->rhs = makeDirectRangeTerminal(context, IteratorOp::Count, i32Id, i32Id, false);
    binaryReturn->value = std::move(binary);
    binarySiblingBoundary->stmts.push_back(std::move(binaryReturn));
    auto binarySiblingCfg = cfgBuilder.build(std::move(binarySiblingBoundary), {reducerParameter},
                                             moon::RegionKind::Function, module);
    bool hoistedBinarySibling = false;
    bool returnedNormalizedBinary = false;
    if (binarySiblingCfg)
        for (const auto& block : binarySiblingCfg->blocks) {
            for (const auto& operation : block.operations)
                if (const auto* declaration = dynamic_cast<const moon::LetStmt*>(operation.get());
                    declaration && declaration->name.rfind("$expression.hoist.", 0) == 0)
                    hoistedBinarySibling = dynamic_cast<const moon::CallExpr*>(
                                               declaration->initializer.get()) != nullptr;
            if (block.terminator.kind != moon::TerminatorKind::Return) continue;
            const auto* returned =
                dynamic_cast<const moon::BinaryExpr*>(block.terminator.operand.get());
            const auto* lhs =
                returned ? dynamic_cast<const moon::IdentifierExpr*>(returned->lhs.get()) : nullptr;
            const auto* rhs =
                returned ? dynamic_cast<const moon::IdentifierExpr*>(returned->rhs.get()) : nullptr;
            returnedNormalizedBinary = lhs && rhs &&
                                       lhs->name.rfind("$expression.hoist.", 0) == 0 &&
                                       rhs->name.rfind("$terminal.count.", 0) == 0;
        }
    if (!binarySiblingCfg || !cfgVerifier.verify(*binarySiblingCfg, module) ||
        !hoistedBinarySibling || !returnedNormalizedBinary)
        return fail("Copy binary sibling did not preserve evaluation order across terminal CFG");

    auto affineOrderedBoundary = std::make_unique<moon::BlockStmt>();
    auto affineOrderedUse = std::make_unique<moon::ExprStmt>();
    auto affineOrderedCall = std::make_unique<moon::CallExpr>();
    auto affineOrderedReducer = std::make_unique<moon::IdentifierExpr>();
    affineOrderedReducer->name = "reducer";
    affineOrderedReducer->type = reducerTypeId;
    affineOrderedCall->callee = std::move(affineOrderedReducer);
    affineOrderedCall->type = i32Id;
    auto producedAffine = std::make_unique<moon::CallExpr>();
    auto affineProducer = std::make_unique<moon::IdentifierExpr>();
    affineProducer->name = "affineProducer";
    affineProducer->type = affineValueProducerTypeId;
    producedAffine->callee = std::move(affineProducer);
    producedAffine->type = i32Id;
    producedAffine->returnUsage = luna::ownership::Usage::Affine;
    affineOrderedCall->args.push_back(std::move(producedAffine));
    affineOrderedCall->args.push_back(
        makeDirectRangeTerminal(context, IteratorOp::Count, i32Id, i32Id, false));
    affineOrderedUse->expr = std::move(affineOrderedCall);
    affineOrderedBoundary->stmts.push_back(std::move(affineOrderedUse));
    moon::Param affineProducerParameter;
    affineProducerParameter.name = "affineProducer";
    affineProducerParameter.type = affineValueProducerTypeId;
    auto affineOrderedCfg = cfgBuilder.build(std::move(affineOrderedBoundary),
                                             {reducerParameter, affineProducerParameter},
                                             moon::RegionKind::Function, module);
    moon::LetStmt* affineOrderedState = nullptr;
    moon::CallExpr* affineOrderedConsumer = nullptr;
    moon::MoveExpr* affineOrderedTransfer = nullptr;
    if (affineOrderedCfg)
        for (auto& block : affineOrderedCfg->blocks)
            for (auto& operation : block.operations) {
                if (auto* declaration = dynamic_cast<moon::LetStmt*>(operation.get());
                    declaration && declaration->name.rfind("$expression.hoist.", 0) == 0 &&
                    declaration->usage == luna::ownership::Usage::Affine)
                    affineOrderedState = declaration;
                auto* statement = dynamic_cast<moon::ExprStmt*>(operation.get());
                auto* call =
                    statement ? dynamic_cast<moon::CallExpr*>(statement->expr.get()) : nullptr;
                if (call && call->args.size() == 2) {
                    affineOrderedConsumer = call;
                    affineOrderedTransfer = dynamic_cast<moon::MoveExpr*>(call->args.front().get());
                }
            }
    const auto* affineOrderedIdentifier =
        affineOrderedTransfer
            ? dynamic_cast<const moon::IdentifierExpr*>(affineOrderedTransfer->operand.get())
            : nullptr;
    if (!affineOrderedCfg || !cfgVerifier.verify(*affineOrderedCfg, module) ||
        !affineOrderedState || !affineOrderedIdentifier ||
        affineOrderedIdentifier->local != affineOrderedState->local)
        return fail("cleanup-free affine sibling did not transfer once across terminal CFG");
    auto preservedAffineTransfer = std::move(affineOrderedConsumer->args.front());
    affineOrderedConsumer->args.front() = std::move(affineOrderedTransfer->operand);
    if (cfgVerifier.verify(*affineOrderedCfg, module))
        return fail("CFG verifier accepted a copied affine expression sibling");
    affineOrderedTransfer->operand = std::move(affineOrderedConsumer->args.front());
    affineOrderedConsumer->args.front() = std::move(preservedAffineTransfer);
    if (!cfgVerifier.verify(*affineOrderedCfg, module))
        return fail("restored affine expression sibling CFG did not verify");

    moon::Param affineValueParameter;
    affineValueParameter.name = "affineValue";
    affineValueParameter.type = stringId;
    affineValueParameter.usage = luna::ownership::Usage::Affine;
    auto cleanupAffineBoundary = std::make_unique<moon::BlockStmt>();
    auto cleanupAffineBinding = std::make_unique<moon::LetStmt>();
    cleanupAffineBinding->name = "cleanupCombined";
    cleanupAffineBinding->type = stringId;
    cleanupAffineBinding->usage = luna::ownership::Usage::Affine;
    auto cleanupAffineCall = std::make_unique<moon::CallExpr>();
    auto cleanupAffineReducer = std::make_unique<moon::IdentifierExpr>();
    cleanupAffineReducer->name = "affineReducer";
    cleanupAffineReducer->type = affineReducerTypeId;
    cleanupAffineCall->callee = std::move(cleanupAffineReducer);
    cleanupAffineCall->type = stringId;
    cleanupAffineCall->returnUsage = luna::ownership::Usage::Affine;
    auto movedAffineValue = std::make_unique<moon::MoveExpr>();
    movedAffineValue->type = stringId;
    auto cleanupAffineSource = std::make_unique<moon::IdentifierExpr>();
    cleanupAffineSource->name = "affineValue";
    cleanupAffineSource->type = stringId;
    movedAffineValue->operand = std::move(cleanupAffineSource);
    cleanupAffineCall->args.push_back(std::move(movedAffineValue));
    cleanupAffineCall->args.push_back(
        makeDirectRangeTerminal(context, IteratorOp::Count, i32Id, i32Id, false));
    cleanupAffineBinding->initializer = std::move(cleanupAffineCall);
    cleanupAffineBoundary->stmts.push_back(std::move(cleanupAffineBinding));
    auto cleanupAffineRelease = std::make_unique<moon::FreeStmt>();
    cleanupAffineRelease->isImplicit = true;
    cleanupAffineRelease->action = cleanupActionForType(TyString);
    auto cleanupAffineResult = std::make_unique<moon::IdentifierExpr>();
    cleanupAffineResult->name = "cleanupCombined";
    cleanupAffineResult->type = stringId;
    cleanupAffineRelease->operand = std::move(cleanupAffineResult);
    cleanupAffineBoundary->stmts.push_back(std::move(cleanupAffineRelease));
    auto cleanupAffineCfg = cfgBuilder.build(std::move(cleanupAffineBoundary),
                                             {affineReducerParameter, affineValueParameter},
                                             moon::RegionKind::Function, module);
    const moon::LocalRecord* cleanupAffineState = nullptr;
    bool cleanupAffineTransferred = false;
    if (cleanupAffineCfg) {
        for (const auto& local : cleanupAffineCfg->locals)
            if (local.name.rfind("$expression.hoist.", 0) == 0) cleanupAffineState = &local;
        for (const auto& block : cleanupAffineCfg->blocks)
            for (const auto& operation : block.operations) {
                const auto* declaration = dynamic_cast<const moon::LetStmt*>(operation.get());
                const auto* call =
                    declaration && declaration->name == "cleanupCombined"
                        ? dynamic_cast<const moon::CallExpr*>(declaration->initializer.get())
                        : nullptr;
                const auto* transfer =
                    call && !call->args.empty()
                        ? dynamic_cast<const moon::MoveExpr*>(call->args.front().get())
                        : nullptr;
                const auto* identifier =
                    transfer ? dynamic_cast<const moon::IdentifierExpr*>(transfer->operand.get())
                             : nullptr;
                cleanupAffineTransferred =
                    cleanupAffineTransferred || (cleanupAffineState && identifier &&
                                                 identifier->local == cleanupAffineState->id);
            }
    }
    bool cleanupAffineTracked = false;
    if (cleanupAffineCfg && cleanupAffineState)
        for (const auto& cleanup : cleanupAffineCfg->cleanups)
            cleanupAffineTracked =
                cleanupAffineTracked || cleanup.place.root == cleanupAffineState->id;
    if (!cleanupAffineCfg || !cfgVerifier.verify(*cleanupAffineCfg, module) ||
        !cleanupAffineState || !cleanupAffineTransferred || !cleanupAffineTracked)
        return fail("cleanup-bearing affine sibling did not survive a non-exiting terminal CFG");

    auto cleanupExitBoundary = std::make_unique<moon::BlockStmt>();
    auto cleanupExitBinding = std::make_unique<moon::LetStmt>();
    cleanupExitBinding->name = "cleanupTryCombined";
    cleanupExitBinding->type = stringId;
    cleanupExitBinding->usage = luna::ownership::Usage::Affine;
    auto cleanupExitCall = std::make_unique<moon::CallExpr>();
    auto cleanupExitReducer = std::make_unique<moon::IdentifierExpr>();
    cleanupExitReducer->name = "affineReducer";
    cleanupExitReducer->type = affineReducerTypeId;
    cleanupExitCall->callee = std::move(cleanupExitReducer);
    cleanupExitCall->type = stringId;
    cleanupExitCall->returnUsage = luna::ownership::Usage::Affine;
    auto cleanupExitMove = std::make_unique<moon::MoveExpr>();
    cleanupExitMove->type = stringId;
    auto cleanupExitSource = std::make_unique<moon::IdentifierExpr>();
    cleanupExitSource->name = "affineValue";
    cleanupExitSource->type = stringId;
    cleanupExitMove->operand = std::move(cleanupExitSource);
    cleanupExitCall->args.push_back(std::move(cleanupExitMove));
    auto cleanupExitTry = std::make_unique<moon::TryExpr>();
    cleanupExitTry->type = i32Id;
    cleanupExitTry->resultType = resultI32BoolId;
    cleanupExitTry->propagatedResultType = resultI32BoolId;
    cleanupExitTry->valueType = i32Id;
    cleanupExitTry->errorType = boolId;
    cleanupExitTry->propagatedErrorType = boolId;
    auto cleanupExitInput = std::make_unique<moon::IdentifierExpr>();
    cleanupExitInput->name = "input";
    cleanupExitInput->type = resultI32BoolId;
    cleanupExitTry->operand = std::move(cleanupExitInput);
    cleanupExitCall->args.push_back(std::move(cleanupExitTry));
    cleanupExitBinding->initializer = std::move(cleanupExitCall);
    cleanupExitBoundary->stmts.push_back(std::move(cleanupExitBinding));
    auto cleanupExitRelease = std::make_unique<moon::FreeStmt>();
    cleanupExitRelease->isImplicit = true;
    cleanupExitRelease->action = cleanupActionForType(TyString);
    auto cleanupExitResult = std::make_unique<moon::IdentifierExpr>();
    cleanupExitResult->name = "cleanupTryCombined";
    cleanupExitResult->type = stringId;
    cleanupExitRelease->operand = std::move(cleanupExitResult);
    cleanupExitBoundary->stmts.push_back(std::move(cleanupExitRelease));
    auto cleanupExitCfg =
        cfgBuilder.build(std::move(cleanupExitBoundary),
                         {affineReducerParameter, affineValueParameter, tryParameter},
                         moon::RegionKind::Function, module);
    const moon::LocalRecord* cleanupExitState = nullptr;
    moon::CleanupId cleanupExitId;
    moon::Terminator* cleanupExitFailure = nullptr;
    if (cleanupExitCfg) {
        for (const auto& local : cleanupExitCfg->locals)
            if (local.name.rfind("$expression.hoist.", 0) == 0) cleanupExitState = &local;
        if (cleanupExitState)
            for (const auto& cleanup : cleanupExitCfg->cleanups)
                if (cleanup.place.root == cleanupExitState->id) {
                    cleanupExitId = cleanup.id;
                    break;
                }
        for (auto& block : cleanupExitCfg->blocks) {
            auto* propagated =
                dynamic_cast<moon::ResultConstructExpr*>(block.terminator.operand.get());
            if (block.terminator.kind == moon::TerminatorKind::Return && propagated &&
                !propagated->isOk)
                cleanupExitFailure = &block.terminator;
        }
    }
    if (!cleanupExitCfg || !cfgVerifier.verify(*cleanupExitCfg, module) || !cleanupExitState ||
        cleanupExitId.empty() || !cleanupExitFailure ||
        std::find(cleanupExitFailure->exitCleanups.begin(), cleanupExitFailure->exitCleanups.end(),
                  cleanupExitId) == cleanupExitFailure->exitCleanups.end())
        return fail("Try failure did not clean an active affine expression sibling");
    const auto preservedCleanupExit = cleanupExitFailure->exitCleanups;
    cleanupExitFailure->exitCleanups.erase(std::remove(cleanupExitFailure->exitCleanups.begin(),
                                                       cleanupExitFailure->exitCleanups.end(),
                                                       cleanupExitId),
                                           cleanupExitFailure->exitCleanups.end());
    if (cfgVerifier.verify(*cleanupExitCfg, module))
        return fail("CFG verifier accepted a Try edge without its affine sibling cleanup");
    cleanupExitFailure->exitCleanups = preservedCleanupExit;
    if (!cfgVerifier.verify(*cleanupExitCfg, module))
        return fail("restored affine Try cleanup CFG did not verify");

    auto cleanupReturnBoundary = std::make_unique<moon::BlockStmt>();
    auto cleanupReturnUse = std::make_unique<moon::ExprStmt>();
    auto cleanupReturnCall = std::make_unique<moon::CallExpr>();
    auto cleanupReturnReducer = std::make_unique<moon::IdentifierExpr>();
    cleanupReturnReducer->name = "affineReducer";
    cleanupReturnReducer->type = affineReducerTypeId;
    cleanupReturnCall->callee = std::move(cleanupReturnReducer);
    cleanupReturnCall->type = stringId;
    cleanupReturnCall->returnUsage = luna::ownership::Usage::Affine;
    auto cleanupReturnMove = std::make_unique<moon::MoveExpr>();
    cleanupReturnMove->type = stringId;
    auto cleanupReturnSource = std::make_unique<moon::IdentifierExpr>();
    cleanupReturnSource->name = "affineValue";
    cleanupReturnSource->type = stringId;
    cleanupReturnMove->operand = std::move(cleanupReturnSource);
    cleanupReturnCall->args.push_back(std::move(cleanupReturnMove));
    auto cleanupReturnBlock = std::make_unique<moon::BlockExpr>();
    cleanupReturnBlock->type = unitId;
    cleanupReturnBlock->block = std::make_unique<moon::BlockStmt>();
    auto cleanupReturn = std::make_unique<moon::ReturnStmt>();
    auto cleanupReturnValue = std::make_unique<moon::BoolLiteralExpr>();
    cleanupReturnValue->value = false;
    cleanupReturnValue->type = boolId;
    cleanupReturn->value = std::move(cleanupReturnValue);
    cleanupReturnBlock->block->stmts.push_back(std::move(cleanupReturn));
    cleanupReturnCall->args.push_back(std::move(cleanupReturnBlock));
    cleanupReturnUse->expr = std::move(cleanupReturnCall);
    cleanupReturnBoundary->stmts.push_back(std::move(cleanupReturnUse));
    auto cleanupReturnCfg = cfgBuilder.build(std::move(cleanupReturnBoundary),
                                             {affineReducerParameter, affineValueParameter},
                                             moon::RegionKind::Function, module);
    const moon::LocalRecord* cleanupReturnState = nullptr;
    moon::CleanupId cleanupReturnId;
    const moon::Terminator* cleanupReturnTerminator = nullptr;
    if (cleanupReturnCfg) {
        for (const auto& local : cleanupReturnCfg->locals)
            if (local.name.rfind("$expression.hoist.", 0) == 0) cleanupReturnState = &local;
        if (cleanupReturnState)
            for (const auto& cleanup : cleanupReturnCfg->cleanups)
                if (cleanup.place.root == cleanupReturnState->id) {
                    cleanupReturnId = cleanup.id;
                    break;
                }
        for (const auto& block : cleanupReturnCfg->blocks)
            if (block.terminator.kind == moon::TerminatorKind::Return &&
                dynamic_cast<const moon::BoolLiteralExpr*>(block.terminator.operand.get()))
                cleanupReturnTerminator = &block.terminator;
    }
    if (!cleanupReturnCfg || !cfgVerifier.verify(*cleanupReturnCfg, module) ||
        !cleanupReturnState || cleanupReturnId.empty() || !cleanupReturnTerminator ||
        std::find(cleanupReturnTerminator->exitCleanups.begin(),
                  cleanupReturnTerminator->exitCleanups.end(),
                  cleanupReturnId) == cleanupReturnTerminator->exitCleanups.end())
        return fail("block return did not clean an active affine expression sibling");

    moon::Param linearReducerParameter;
    linearReducerParameter.name = "linearReducer";
    linearReducerParameter.type = linearReducerTypeId;
    moon::Param linearValueParameter;
    linearValueParameter.name = "linearValue";
    linearValueParameter.type = i32Id;
    linearValueParameter.usage = luna::ownership::Usage::Linear;
    auto linearOrderedBoundary = std::make_unique<moon::BlockStmt>();
    auto linearOrderedUse = std::make_unique<moon::ExprStmt>();
    auto linearOrderedCall = std::make_unique<moon::CallExpr>();
    auto linearOrderedReducer = std::make_unique<moon::IdentifierExpr>();
    linearOrderedReducer->name = "linearReducer";
    linearOrderedReducer->type = linearReducerTypeId;
    linearOrderedCall->callee = std::move(linearOrderedReducer);
    linearOrderedCall->type = i32Id;
    auto linearOrderedMove = std::make_unique<moon::MoveExpr>();
    linearOrderedMove->type = i32Id;
    auto linearOrderedSource = std::make_unique<moon::IdentifierExpr>();
    linearOrderedSource->name = "linearValue";
    linearOrderedSource->type = i32Id;
    linearOrderedMove->operand = std::move(linearOrderedSource);
    linearOrderedCall->args.push_back(std::move(linearOrderedMove));
    linearOrderedCall->args.push_back(
        makeDirectRangeTerminal(context, IteratorOp::Count, i32Id, i32Id, false));
    linearOrderedUse->expr = std::move(linearOrderedCall);
    linearOrderedBoundary->stmts.push_back(std::move(linearOrderedUse));
    auto linearOrderedCfg = cfgBuilder.build(std::move(linearOrderedBoundary),
                                             {linearReducerParameter, linearValueParameter},
                                             moon::RegionKind::Function, module);
    const moon::LocalRecord* linearOrderedState = nullptr;
    const moon::MoveExpr* linearOrderedTransfer = nullptr;
    if (linearOrderedCfg) {
        for (const auto& local : linearOrderedCfg->locals)
            if (local.name.rfind("$expression.hoist.", 0) == 0) linearOrderedState = &local;
        for (const auto& block : linearOrderedCfg->blocks)
            for (const auto& operation : block.operations) {
                const auto* statement = dynamic_cast<const moon::ExprStmt*>(operation.get());
                const auto* call = statement
                                       ? dynamic_cast<const moon::CallExpr*>(statement->expr.get())
                                       : nullptr;
                if (call && call->args.size() == 2)
                    linearOrderedTransfer =
                        dynamic_cast<const moon::MoveExpr*>(call->args.front().get());
            }
    }
    const auto* linearOrderedIdentifier =
        linearOrderedTransfer
            ? dynamic_cast<const moon::IdentifierExpr*>(linearOrderedTransfer->operand.get())
            : nullptr;
    if (!linearOrderedCfg || !cfgVerifier.verify(*linearOrderedCfg, module) ||
        !linearOrderedState || linearOrderedState->usage != luna::ownership::Usage::Linear ||
        !linearOrderedIdentifier || linearOrderedIdentifier->local != linearOrderedState->id)
        return fail("linear sibling did not transfer exactly once across a non-exiting CFG");

    return 0;
}

} // namespace canonical_test
