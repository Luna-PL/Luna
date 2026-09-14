#include "core/TypeLayout.h"
#include "moonir/ControlFlowBuilder.h"
#include "moonir/MoonIR.h"
#include "moonir/Verifier.h"
#include "moonir_canonical_test_support.h"

#include <algorithm>
#include <iostream>

namespace canonical_test {

int runIteratorCleanupTests(ControlFlowTestContext& context) {
    auto& module = context.module;
    auto& cfgVerifier = context.verifier;
    auto& cfgBuilder = context.builder;
    const auto& ownedProduct = context.ownedProduct;
    const auto productId = context.productId;
    const auto ownedProductId = context.ownedProductId;
    const auto inlineProductId = context.inlineProductId;
    const auto i32Id = context.i32Id;
    const auto stringId = context.stringId;
    const auto boolId = context.boolId;
    const auto unitId = context.unitId;
    const auto resultI32BoolId = context.resultI32BoolId;
    const auto affineReducerTypeId = context.affineReducerTypeId;
    const auto linearReducerTypeId = context.linearReducerTypeId;
    const auto actionTypeId = context.actionTypeId;

    moon::Param tryParameter;
    tryParameter.name = "input";
    tryParameter.type = context.resultI32BoolId;

    moon::Param affineReducerParameter;
    affineReducerParameter.name = "affineReducer";
    affineReducerParameter.type = affineReducerTypeId;
    moon::Param affineValueParameter;
    affineValueParameter.name = "affineValue";
    affineValueParameter.type = stringId;
    affineValueParameter.usage = luna::ownership::Usage::Affine;
    moon::Param linearReducerParameter;
    linearReducerParameter.name = "linearReducer";
    linearReducerParameter.type = linearReducerTypeId;
    moon::Param linearValueParameter;
    linearValueParameter.name = "linearValue";
    linearValueParameter.type = i32Id;
    linearValueParameter.usage = luna::ownership::Usage::Linear;

    auto linearExitBoundary = std::make_unique<moon::BlockStmt>();
    auto linearExitUse = std::make_unique<moon::ExprStmt>();
    auto linearExitCall = std::make_unique<moon::CallExpr>();
    auto linearExitReducer = std::make_unique<moon::IdentifierExpr>();
    linearExitReducer->name = "linearReducer";
    linearExitReducer->type = linearReducerTypeId;
    linearExitCall->callee = std::move(linearExitReducer);
    linearExitCall->type = i32Id;
    auto linearExitMove = std::make_unique<moon::MoveExpr>();
    linearExitMove->type = i32Id;
    auto linearExitSource = std::make_unique<moon::IdentifierExpr>();
    linearExitSource->name = "linearValue";
    linearExitSource->type = i32Id;
    linearExitMove->operand = std::move(linearExitSource);
    linearExitCall->args.push_back(std::move(linearExitMove));
    auto linearExitTry = std::make_unique<moon::TryExpr>();
    linearExitTry->type = i32Id;
    linearExitTry->resultType = resultI32BoolId;
    linearExitTry->propagatedResultType = resultI32BoolId;
    linearExitTry->valueType = i32Id;
    linearExitTry->errorType = boolId;
    linearExitTry->propagatedErrorType = boolId;
    auto linearExitInput = std::make_unique<moon::IdentifierExpr>();
    linearExitInput->name = "input";
    linearExitInput->type = resultI32BoolId;
    linearExitTry->operand = std::move(linearExitInput);
    linearExitCall->args.push_back(std::move(linearExitTry));
    linearExitUse->expr = std::move(linearExitCall);
    linearExitBoundary->stmts.push_back(std::move(linearExitUse));
    if (cfgBuilder.build(std::move(linearExitBoundary),
                         {linearReducerParameter, linearValueParameter, tryParameter},
                         moon::RegionKind::Function, module))
        return fail("linear sibling crossed an early-exit CFG path");
    bool diagnosedLinearExit = false;
    for (const auto& message : cfgBuilder.errors())
        diagnosedLinearExit =
            diagnosedLinearExit ||
            message.find("linear expression sibling may cross an early-exit") != std::string::npos;
    if (!diagnosedLinearExit) return fail("linear sibling lost its early-exit diagnostic");

    auto affineSiblingBoundary = std::make_unique<moon::BlockStmt>();
    auto affineSiblingUse = std::make_unique<moon::ExprStmt>();
    auto affineSiblingCall = std::make_unique<moon::CallExpr>();
    auto affineSiblingReducer = std::make_unique<moon::IdentifierExpr>();
    affineSiblingReducer->name = "affineReducer";
    affineSiblingReducer->type = affineReducerTypeId;
    affineSiblingCall->callee = std::move(affineSiblingReducer);
    affineSiblingCall->type = stringId;
    affineSiblingCall->returnUsage = luna::ownership::Usage::Affine;
    auto affineEarlierSibling = std::make_unique<moon::IdentifierExpr>();
    affineEarlierSibling->name = "affineValue";
    affineEarlierSibling->type = stringId;
    affineSiblingCall->args.push_back(std::move(affineEarlierSibling));
    affineSiblingCall->args.push_back(
        makeDirectRangeTerminal(context, IteratorOp::Count, i32Id, i32Id, false));
    affineSiblingUse->expr = std::move(affineSiblingCall);
    affineSiblingBoundary->stmts.push_back(std::move(affineSiblingUse));
    if (cfgBuilder.build(std::move(affineSiblingBoundary),
                         {affineReducerParameter, affineValueParameter}, moon::RegionKind::Function,
                         module))
        return fail("move-only expression sibling entered Copy-only CFG hoisting");
    bool diagnosedAffineSibling = false;
    for (const auto& message : cfgBuilder.errors())
        diagnosedAffineSibling = diagnosedAffineSibling ||
                                 message.find("requires an explicit transfer") != std::string::npos;
    if (!diagnosedAffineSibling)
        return fail("move-only expression sibling lost its explicit hoisting boundary");

    auto shortCircuitBoundary = std::make_unique<moon::BlockStmt>();
    auto shortCircuitUse = std::make_unique<moon::ExprStmt>();
    auto shortCircuit = std::make_unique<moon::BinaryExpr>();
    shortCircuit->op = moon::Operator::LogicalAnd;
    shortCircuit->type = boolId;
    auto shortCircuitLeft = std::make_unique<moon::BoolLiteralExpr>();
    shortCircuitLeft->value = true;
    shortCircuitLeft->type = boolId;
    shortCircuit->lhs = std::move(shortCircuitLeft);
    auto shortCircuitRight = std::make_unique<moon::BinaryExpr>();
    shortCircuitRight->op = moon::Operator::Greater;
    shortCircuitRight->type = boolId;
    shortCircuitRight->lhs =
        makeDirectRangeTerminal(context, IteratorOp::Count, i32Id, i32Id, false);
    auto zero = std::make_unique<moon::IntLiteralExpr>();
    zero->value = 0;
    zero->type = i32Id;
    shortCircuitRight->rhs = std::move(zero);
    shortCircuit->rhs = std::move(shortCircuitRight);
    shortCircuitUse->expr = std::move(shortCircuit);
    shortCircuitBoundary->stmts.push_back(std::move(shortCircuitUse));
    auto shortCircuitCfg =
        cfgBuilder.build(std::move(shortCircuitBoundary), {}, moon::RegionKind::Function, module);
    moon::LetStmt* shortCircuitState = nullptr;
    moon::AssignExpr* shortCircuitAssignment = nullptr;
    if (shortCircuitCfg) {
        for (auto& block : shortCircuitCfg->blocks) {
            for (auto& operation : block.operations) {
                if (auto* declaration = dynamic_cast<moon::LetStmt*>(operation.get());
                    declaration && declaration->name.rfind("$short-circuit.", 0) == 0)
                    shortCircuitState = declaration;
                if (auto* statement = dynamic_cast<moon::ExprStmt*>(operation.get()))
                    if (auto* assignment = dynamic_cast<moon::AssignExpr*>(statement->expr.get());
                        assignment && assignment->lhs)
                        if (auto* destination =
                                dynamic_cast<moon::IdentifierExpr*>(assignment->lhs.get());
                            destination && destination->name.rfind("$short-circuit.", 0) == 0)
                            shortCircuitAssignment = assignment;
            }
        }
    }
    if (!shortCircuitCfg || !cfgVerifier.verify(*shortCircuitCfg, module) || !shortCircuitState ||
        !shortCircuitAssignment)
        return fail("short-circuit terminal did not become conditional CFG");
    auto preservedShortCircuitRhs = std::move(shortCircuitAssignment->rhs);
    auto forgedShortCircuit = std::make_unique<moon::BinaryExpr>();
    forgedShortCircuit->op = moon::Operator::LogicalOr;
    forgedShortCircuit->type = boolId;
    auto forgedLeft = std::make_unique<moon::BoolLiteralExpr>();
    forgedLeft->value = false;
    forgedLeft->type = boolId;
    forgedShortCircuit->lhs = std::move(forgedLeft);
    auto forgedRight = std::make_unique<moon::BoolLiteralExpr>();
    forgedRight->value = true;
    forgedRight->type = boolId;
    forgedShortCircuit->rhs = std::move(forgedRight);
    shortCircuitAssignment->rhs = std::move(forgedShortCircuit);
    if (cfgVerifier.verify(*shortCircuitCfg, module))
        return fail("CFG verifier accepted a residual short-circuit expression");
    shortCircuitAssignment->rhs = std::move(preservedShortCircuitRhs);
    if (!cfgVerifier.verify(*shortCircuitCfg, module))
        return fail("restored short-circuit CFG did not verify");

    auto inlineRecordBoundary = std::make_unique<moon::BlockStmt>();
    auto inlineRecordBinding = std::make_unique<moon::LetStmt>();
    inlineRecordBinding->name = "inlineSnapshot";
    inlineRecordBinding->type = inlineProductId;
    auto inlineRecord = std::make_unique<moon::RecordLiteralExpr>();
    inlineRecord->type = inlineProductId;
    moon::RecordLiteralExpr::Field inlineValueField;
    inlineValueField.name = "value";
    inlineValueField.value =
        makeDirectRangeTerminal(context, IteratorOp::Count, i32Id, i32Id, false);
    inlineRecord->fields.push_back(std::move(inlineValueField));
    inlineRecordBinding->initializer = std::move(inlineRecord);
    inlineRecordBoundary->stmts.push_back(std::move(inlineRecordBinding));
    auto inlineRecordCfg =
        cfgBuilder.build(std::move(inlineRecordBoundary), {}, moon::RegionKind::Function, module);
    bool normalizedInlineField = false;
    if (inlineRecordCfg)
        for (const auto& block : inlineRecordCfg->blocks)
            for (const auto& operation : block.operations) {
                const auto* declaration = dynamic_cast<const moon::LetStmt*>(operation.get());
                const auto* value = declaration ? dynamic_cast<const moon::RecordLiteralExpr*>(
                                                      declaration->initializer.get())
                                                : nullptr;
                const auto* field = value && !value->fields.empty()
                                        ? dynamic_cast<const moon::IdentifierExpr*>(
                                              value->fields.front().value.get())
                                        : nullptr;
                normalizedInlineField = normalizedInlineField ||
                                        (field && field->name.rfind("$terminal.count.", 0) == 0);
            }
    if (!inlineRecordCfg || !cfgVerifier.verify(*inlineRecordCfg, module) || !normalizedInlineField)
        return fail("inline record field did not preserve ordered CFG evaluation");

    auto recordTerminalBoundary = std::make_unique<moon::BlockStmt>();
    auto recordTerminalBinding = std::make_unique<moon::LetStmt>();
    recordTerminalBinding->name = "snapshot";
    recordTerminalBinding->type = productId;
    recordTerminalBinding->usage = module.findType(productId)->sysmeta.resource.usage;
    auto recordTerminal = std::make_unique<moon::RecordLiteralExpr>();
    recordTerminal->type = productId;
    moon::RecordLiteralExpr::Field valueField;
    valueField.name = "value";
    valueField.value = makeDirectRangeTerminal(context, IteratorOp::Count, i32Id, i32Id, false);
    recordTerminal->fields.push_back(std::move(valueField));
    recordTerminalBinding->initializer = std::move(recordTerminal);
    recordTerminalBoundary->stmts.push_back(std::move(recordTerminalBinding));
    auto recordTerminalRelease = std::make_unique<moon::FreeStmt>();
    recordTerminalRelease->isImplicit = true;
    recordTerminalRelease->action = luna::ownership::CleanupAction::Deallocate;
    auto recordTerminalResult = std::make_unique<moon::IdentifierExpr>();
    recordTerminalResult->name = "snapshot";
    recordTerminalResult->type = productId;
    recordTerminalRelease->operand = std::move(recordTerminalResult);
    recordTerminalBoundary->stmts.push_back(std::move(recordTerminalRelease));
    auto recordTerminalCfg =
        cfgBuilder.build(std::move(recordTerminalBoundary), {}, moon::RegionKind::Function, module);
    const moon::AllocateStmt* recordAllocation = nullptr;
    const moon::InitAllocationExpr* recordInitialization = nullptr;
    bool retainedAllocatingRecord = false;
    if (recordTerminalCfg)
        for (const auto& block : recordTerminalCfg->blocks)
            for (const auto& operation : block.operations) {
                if (const auto* allocation =
                        dynamic_cast<const moon::AllocateStmt*>(operation.get()))
                    recordAllocation = allocation;
                const auto* declaration = dynamic_cast<const moon::LetStmt*>(operation.get());
                if (!declaration) continue;
                retainedAllocatingRecord =
                    retainedAllocatingRecord ||
                    dynamic_cast<const moon::RecordLiteralExpr*>(declaration->initializer.get());
                if (declaration->name == "snapshot")
                    recordInitialization = dynamic_cast<const moon::InitAllocationExpr*>(
                        declaration->initializer.get());
            }
    const auto* recordInitializedField =
        recordInitialization && !recordInitialization->elements.empty()
            ? dynamic_cast<const moon::IdentifierExpr*>(
                  recordInitialization->elements.front().value.get())
            : nullptr;
    if (!recordTerminalCfg || !cfgVerifier.verify(*recordTerminalCfg, module) ||
        !recordAllocation || !recordInitialization ||
        recordInitialization->allocation != recordAllocation->local || !recordInitializedField ||
        recordInitializedField->name.rfind("$terminal.count.", 0) != 0 || retainedAllocatingRecord)
        return fail("allocating record did not preserve allocation-before-terminal order");

    auto ownedRecordBoundary = std::make_unique<moon::BlockStmt>();
    auto ownedRecordBinding = std::make_unique<moon::LetStmt>();
    ownedRecordBinding->name = "ownedSnapshot";
    ownedRecordBinding->type = ownedProductId;
    ownedRecordBinding->usage = module.findType(ownedProductId)->sysmeta.resource.usage;
    auto ownedRecord = std::make_unique<moon::RecordLiteralExpr>();
    ownedRecord->type = ownedProductId;
    moon::RecordLiteralExpr::Field ownedField;
    ownedField.name = "owned";
    auto ownedFieldMove = std::make_unique<moon::MoveExpr>();
    ownedFieldMove->type = stringId;
    auto ownedFieldSource = std::make_unique<moon::IdentifierExpr>();
    ownedFieldSource->name = "affineValue";
    ownedFieldSource->type = stringId;
    ownedFieldMove->operand = std::move(ownedFieldSource);
    ownedField.value = std::move(ownedFieldMove);
    ownedRecord->fields.push_back(std::move(ownedField));
    moon::RecordLiteralExpr::Field fallibleField;
    fallibleField.name = "value";
    auto ownedRecordTry = std::make_unique<moon::TryExpr>();
    ownedRecordTry->type = i32Id;
    ownedRecordTry->resultType = resultI32BoolId;
    ownedRecordTry->propagatedResultType = resultI32BoolId;
    ownedRecordTry->valueType = i32Id;
    ownedRecordTry->errorType = boolId;
    ownedRecordTry->propagatedErrorType = boolId;
    auto ownedRecordInput = std::make_unique<moon::IdentifierExpr>();
    ownedRecordInput->name = "input";
    ownedRecordInput->type = resultI32BoolId;
    ownedRecordTry->operand = std::move(ownedRecordInput);
    fallibleField.value = std::move(ownedRecordTry);
    ownedRecord->fields.push_back(std::move(fallibleField));
    ownedRecordBinding->initializer = std::move(ownedRecord);
    ownedRecordBoundary->stmts.push_back(std::move(ownedRecordBinding));
    auto ownedRecordRelease = std::make_unique<moon::FreeStmt>();
    ownedRecordRelease->isImplicit = true;
    ownedRecordRelease->action = cleanupActionForType(ownedProduct);
    auto ownedRecordResult = std::make_unique<moon::IdentifierExpr>();
    ownedRecordResult->name = "ownedSnapshot";
    ownedRecordResult->type = ownedProductId;
    ownedRecordRelease->operand = std::move(ownedRecordResult);
    ownedRecordBoundary->stmts.push_back(std::move(ownedRecordRelease));
    auto ownedRecordCfg =
        cfgBuilder.build(std::move(ownedRecordBoundary), {affineValueParameter, tryParameter},
                         moon::RegionKind::Function, module);
    moon::CleanupId ownedRecordRawCleanup;
    moon::CleanupId ownedRecordValueCleanup;
    const moon::Terminator* ownedRecordFailure = nullptr;
    if (ownedRecordCfg) {
        for (const auto& cleanup : ownedRecordCfg->cleanups) {
            const auto* local = ownedRecordCfg->findLocal(cleanup.place.root);
            if (local && local->kind == moon::LocalKind::Allocation)
                ownedRecordRawCleanup = cleanup.id;
            if (local && local->name.rfind("$expression.hoist.", 0) == 0)
                ownedRecordValueCleanup = cleanup.id;
        }
        for (const auto& block : ownedRecordCfg->blocks) {
            const auto* propagated =
                dynamic_cast<const moon::ResultConstructExpr*>(block.terminator.operand.get());
            if (block.terminator.kind == moon::TerminatorKind::Return && propagated &&
                !propagated->isOk)
                ownedRecordFailure = &block.terminator;
        }
    }
    const auto rawCleanupPosition =
        ownedRecordFailure
            ? std::find(ownedRecordFailure->exitCleanups.begin(),
                        ownedRecordFailure->exitCleanups.end(), ownedRecordRawCleanup)
            : std::vector<moon::CleanupId>::const_iterator{};
    const auto valueCleanupPosition =
        ownedRecordFailure
            ? std::find(ownedRecordFailure->exitCleanups.begin(),
                        ownedRecordFailure->exitCleanups.end(), ownedRecordValueCleanup)
            : std::vector<moon::CleanupId>::const_iterator{};
    if (!ownedRecordCfg || !cfgVerifier.verify(*ownedRecordCfg, module) ||
        ownedRecordRawCleanup.empty() || ownedRecordValueCleanup.empty() || !ownedRecordFailure ||
        rawCleanupPosition == ownedRecordFailure->exitCleanups.end() ||
        valueCleanupPosition == ownedRecordFailure->exitCleanups.end() ||
        valueCleanupPosition > rawCleanupPosition)
        return fail("partial struct initialization did not clean value before raw storage");

    auto heapTryBoundary = std::make_unique<moon::BlockStmt>();
    auto heapTryBinding = std::make_unique<moon::LetStmt>();
    heapTryBinding->name = "heapTryValue";
    heapTryBinding->type = i32Id;
    auto heapTry = std::make_unique<moon::HeapAllocExpr>();
    heapTry->type = i32Id;
    heapTry->allocatedType = i32Id;
    auto heapTryConstructor = std::make_unique<moon::CallExpr>();
    auto heapTryCallee = std::make_unique<moon::IdentifierExpr>();
    heapTryCallee->name = "i32";
    heapTryConstructor->callee = std::move(heapTryCallee);
    auto heapTryPropagation = std::make_unique<moon::TryExpr>();
    heapTryPropagation->type = i32Id;
    heapTryPropagation->resultType = resultI32BoolId;
    heapTryPropagation->propagatedResultType = resultI32BoolId;
    heapTryPropagation->valueType = i32Id;
    heapTryPropagation->errorType = boolId;
    heapTryPropagation->propagatedErrorType = boolId;
    auto heapTryInput = std::make_unique<moon::IdentifierExpr>();
    heapTryInput->name = "input";
    heapTryInput->type = resultI32BoolId;
    heapTryPropagation->operand = std::move(heapTryInput);
    heapTryConstructor->args.push_back(std::move(heapTryPropagation));
    heapTry->initializer = std::move(heapTryConstructor);
    heapTryBinding->initializer = std::move(heapTry);
    heapTryBoundary->stmts.push_back(std::move(heapTryBinding));
    auto heapTryRelease = std::make_unique<moon::FreeStmt>();
    heapTryRelease->isImplicit = true;
    heapTryRelease->action = luna::ownership::CleanupAction::Deallocate;
    auto heapTryResult = std::make_unique<moon::IdentifierExpr>();
    heapTryResult->name = "heapTryValue";
    heapTryResult->type = i32Id;
    heapTryRelease->operand = std::move(heapTryResult);
    heapTryBoundary->stmts.push_back(std::move(heapTryRelease));
    auto heapTryCfg = cfgBuilder.build(std::move(heapTryBoundary), {tryParameter},
                                       moon::RegionKind::Function, module);
    const moon::LocalRecord* heapRawLocal = nullptr;
    moon::CleanupId heapRawCleanup;
    moon::Terminator* heapFailure = nullptr;
    moon::InitAllocationExpr* heapInitialization = nullptr;
    if (heapTryCfg) {
        for (const auto& local : heapTryCfg->locals)
            if (local.kind == moon::LocalKind::Allocation) heapRawLocal = &local;
        if (heapRawLocal)
            for (const auto& cleanup : heapTryCfg->cleanups)
                if (cleanup.place.root == heapRawLocal->id &&
                    cleanup.kind == moon::CleanupKind::Allocation) {
                    heapRawCleanup = cleanup.id;
                    break;
                }
        for (auto& block : heapTryCfg->blocks) {
            for (auto& operation : block.operations) {
                auto* declaration = dynamic_cast<moon::LetStmt*>(operation.get());
                if (declaration && declaration->name == "heapTryValue")
                    heapInitialization =
                        dynamic_cast<moon::InitAllocationExpr*>(declaration->initializer.get());
            }
            const auto* propagated =
                dynamic_cast<const moon::ResultConstructExpr*>(block.terminator.operand.get());
            if (block.terminator.kind == moon::TerminatorKind::Return && propagated &&
                !propagated->isOk)
                heapFailure = &block.terminator;
        }
    }
    if (!heapTryCfg || !cfgVerifier.verify(*heapTryCfg, module) || !heapRawLocal ||
        heapRawCleanup.empty() || !heapFailure || !heapInitialization ||
        std::find(heapFailure->exitCleanups.begin(), heapFailure->exitCleanups.end(),
                  heapRawCleanup) == heapFailure->exitCleanups.end())
        return fail("heap initializer early exit did not release raw allocation");
    const auto preservedHeapFailureCleanups = heapFailure->exitCleanups;
    heapFailure->exitCleanups.erase(std::remove(heapFailure->exitCleanups.begin(),
                                                heapFailure->exitCleanups.end(), heapRawCleanup),
                                    heapFailure->exitCleanups.end());
    if (cfgVerifier.verify(*heapTryCfg, module))
        return fail("CFG verifier accepted a leaking heap initializer failure edge");
    heapFailure->exitCleanups = preservedHeapFailureCleanups;
    if (!cfgVerifier.verify(*heapTryCfg, module))
        return fail("restored heap initializer cleanup CFG did not verify");
    const auto preservedHeapAllocation = heapInitialization->allocation;
    heapInitialization->allocation = moon::LocalId{999};
    if (cfgVerifier.verify(*heapTryCfg, module))
        return fail("CFG verifier accepted an initialization with no allocation identity");
    heapInitialization->allocation = preservedHeapAllocation;
    auto& heapRawCleanupRecord = heapTryCfg->cleanups[heapRawCleanup.value];
    heapRawCleanupRecord.kind = moon::CleanupKind::Value;
    if (cfgVerifier.verify(*heapTryCfg, module))
        return fail("CFG verifier accepted raw storage with a value cleanup");
    heapRawCleanupRecord.kind = moon::CleanupKind::Allocation;
    if (!cfgVerifier.verify(*heapTryCfg, module))
        return fail("restored heap allocation identity did not verify");

    auto discardedAllocationBoundary = std::make_unique<moon::BlockStmt>();
    auto discardedAllocationUse = std::make_unique<moon::ExprStmt>();
    auto discardedAllocation = std::make_unique<moon::HeapAllocExpr>();
    discardedAllocation->type = i32Id;
    discardedAllocation->allocatedType = i32Id;
    auto discardedConstructor = std::make_unique<moon::CallExpr>();
    auto discardedCallee = std::make_unique<moon::IdentifierExpr>();
    discardedCallee->name = "i32";
    discardedConstructor->callee = std::move(discardedCallee);
    auto discardedValue = std::make_unique<moon::IntLiteralExpr>();
    discardedValue->value = 1;
    discardedValue->type = i32Id;
    discardedConstructor->args.push_back(std::move(discardedValue));
    discardedAllocation->initializer = std::move(discardedConstructor);
    discardedAllocationUse->expr = std::move(discardedAllocation);
    discardedAllocationBoundary->stmts.push_back(std::move(discardedAllocationUse));
    if (cfgBuilder.build(std::move(discardedAllocationBoundary), {}, moon::RegionKind::Function,
                         module))
        return fail("CFG builder accepted a discarded owning allocation");
    bool diagnosedDiscardedAllocation = false;
    for (const auto& message : cfgBuilder.errors())
        diagnosedDiscardedAllocation =
            diagnosedDiscardedAllocation ||
            message.find("allocation result cannot be discarded") != std::string::npos;
    if (!diagnosedDiscardedAllocation)
        return fail("discarded allocation lost its ownership diagnostic");

    auto forEachStructured = std::make_unique<moon::BlockStmt>();
    forEachStructured->stmts.push_back(
        makeMaterializedRangeBinding(context, "forEachPending", false));
    auto forEachUse = std::make_unique<moon::ExprStmt>();
    auto forEachTerminal =
        makeIteratorTerminal(context, "forEachPending", IteratorOp::ForEach, unitId, unitId);
    auto action = std::make_unique<moon::IdentifierExpr>();
    action->name = "action";
    action->type = actionTypeId;
    forEachTerminal->args.push_back(std::move(action));
    forEachUse->expr = std::move(forEachTerminal);
    forEachStructured->stmts.push_back(std::move(forEachUse));
    moon::Param actionParameter;
    actionParameter.name = "action";
    actionParameter.type = actionTypeId;
    auto forEachCfg = cfgBuilder.build(std::move(forEachStructured), {actionParameter},
                                       moon::RegionKind::Function, module);
    bool retainedForEachTerminal = false;
    if (forEachCfg)
        for (const auto& block : forEachCfg->blocks)
            for (const auto& operation : block.operations)
                if (const auto* expression = dynamic_cast<const moon::ExprStmt*>(operation.get()))
                    if (const auto* call =
                            dynamic_cast<const moon::CallExpr*>(expression->expr.get()))
                        retainedForEachTerminal =
                            retainedForEachTerminal || call->iteratorOp == IteratorOp::ForEach;
    if (!forEachCfg || !cfgVerifier.verify(*forEachCfg, module) || retainedForEachTerminal)
        return fail("materialized for_each did not normalize to loop body calls");

    auto directForEachStructured = std::make_unique<moon::BlockStmt>();
    auto directForEachUse = std::make_unique<moon::ExprStmt>();
    auto directForEach =
        makeDirectRangeTerminal(context, IteratorOp::ForEach, unitId, unitId, true);
    auto directAction = std::make_unique<moon::IdentifierExpr>();
    directAction->name = "action";
    directAction->type = actionTypeId;
    directForEach->args.push_back(std::move(directAction));
    directForEachUse->expr = std::move(directForEach);
    directForEachStructured->stmts.push_back(std::move(directForEachUse));
    auto directForEachCfg = cfgBuilder.build(std::move(directForEachStructured), {actionParameter},
                                             moon::RegionKind::Function, module);
    bool retainedDirectForEachTerminal = false;
    bool directForEachActionLocal = false;
    if (directForEachCfg)
        for (const auto& block : directForEachCfg->blocks)
            for (const auto& operation : block.operations) {
                if (const auto* declaration = dynamic_cast<const moon::LetStmt*>(operation.get()))
                    directForEachActionLocal = directForEachActionLocal ||
                                               declaration->name.rfind("$terminal.action.", 0) == 0;
                if (const auto* expression = dynamic_cast<const moon::ExprStmt*>(operation.get()))
                    if (const auto* call =
                            dynamic_cast<const moon::CallExpr*>(expression->expr.get()))
                        retainedDirectForEachTerminal = retainedDirectForEachTerminal ||
                                                        call->iteratorOp == IteratorOp::ForEach;
            }
    if (!directForEachCfg || !cfgVerifier.verify(*directForEachCfg, module) ||
        !directForEachActionLocal || retainedDirectForEachTerminal)
        return fail("direct for_each did not normalize to body calls");

    return 0;
}

} // namespace canonical_test
