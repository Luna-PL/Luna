#include "core/TypeLayout.h"
#include "moonir/ControlFlowBuilder.h"
#include "moonir/MoonIR.h"
#include "moonir/Verifier.h"
#include "moonir_canonical_test_support.h"

#include <algorithm>
#include <iostream>

namespace canonical_test {

int runIteratorTerminalTests(ControlFlowTestContext& context) {
    auto& module = context.module;
    auto& cfgVerifier = context.verifier;
    auto& cfgBuilder = context.builder;
    const auto rangeId = context.rangeId;
    const auto i32Id = context.i32Id;
    const auto stringId = context.stringId;
    const auto boolId = context.boolId;
    const auto unitId = context.unitId;
    const auto resultI32BoolId = context.resultI32BoolId;
    const auto lambdaTypeId = context.lambdaTypeId;
    const auto reducerTypeId = context.reducerTypeId;
    const auto affineReducerTypeId = context.affineReducerTypeId;
    const auto linearReducerTypeId = context.linearReducerTypeId;
    const auto affineValueProducerTypeId = context.affineValueProducerTypeId;
    const auto actionTypeId = context.actionTypeId;
    const auto unitOrderedConsumerTypeId = context.unitOrderedConsumerTypeId;
    const auto choiceId = context.choiceId;

    moon::Param tryParameter;
    tryParameter.name = "input";
    tryParameter.type = context.resultI32BoolId;

    auto statementConditionRoot = std::make_unique<moon::BlockStmt>();
    auto statementCondition = std::make_unique<moon::IfStmt>();
    auto terminalCondition = std::make_unique<moon::BinaryExpr>();
    terminalCondition->op = moon::Operator::Greater;
    terminalCondition->type = boolId;
    terminalCondition->lhs =
        makeDirectRangeTerminal(context, IteratorOp::Count, i32Id, i32Id, false);
    auto conditionZero = std::make_unique<moon::IntLiteralExpr>();
    conditionZero->value = 0;
    conditionZero->type = i32Id;
    terminalCondition->rhs = std::move(conditionZero);
    statementCondition->cond = std::move(terminalCondition);
    statementCondition->thenBlock = std::make_unique<moon::BlockStmt>();
    statementCondition->elseBranch = std::make_unique<moon::BlockStmt>();
    statementConditionRoot->stmts.push_back(std::move(statementCondition));
    auto statementConditionCfg =
        cfgBuilder.build(std::move(statementConditionRoot), {}, moon::RegionKind::Function, module);
    if (!statementConditionCfg || !cfgVerifier.verify(*statementConditionCfg, module))
        return fail("if statement condition did not normalize before branching");

    auto statementScrutineeRoot = std::make_unique<moon::BlockStmt>();
    auto statementScrutinee = std::make_unique<moon::MatchStmt>();
    statementScrutinee->matchedType = choiceId;
    auto terminalVariant = std::make_unique<moon::VariantConstructExpr>();
    terminalVariant->typeName = "Choice";
    terminalVariant->variantName = "Some";
    terminalVariant->constructedType = choiceId;
    terminalVariant->type = choiceId;
    terminalVariant->args.push_back(
        makeDirectRangeTerminal(context, IteratorOp::Count, i32Id, i32Id, false));
    statementScrutinee->scrutinee = std::move(terminalVariant);
    moon::MatchArm statementNoneArm;
    statementNoneArm.variantName = "None";
    statementNoneArm.variantIndex = 0;
    statementNoneArm.body = std::make_unique<moon::BlockStmt>();
    statementScrutinee->arms.push_back(std::move(statementNoneArm));
    moon::MatchArm statementSomeArm;
    statementSomeArm.variantName = "Some";
    statementSomeArm.variantIndex = 1;
    statementSomeArm.bindings = {"item"};
    statementSomeArm.bindingTypes = {i32Id};
    statementSomeArm.bindingUsages = {luna::ownership::Usage::Copy};
    statementSomeArm.body = std::make_unique<moon::BlockStmt>();
    statementScrutinee->arms.push_back(std::move(statementSomeArm));
    statementScrutineeRoot->stmts.push_back(std::move(statementScrutinee));
    auto statementScrutineeCfg =
        cfgBuilder.build(std::move(statementScrutineeRoot), {}, moon::RegionKind::Function, module);
    if (!statementScrutineeCfg || !cfgVerifier.verify(*statementScrutineeCfg, module))
        return fail("match statement scrutinee did not normalize before switching");

    auto repeatedConditionRoot = std::make_unique<moon::BlockStmt>();
    auto repeatedConditionLoop = std::make_unique<moon::WhileStmt>();
    auto repeatedCondition = std::make_unique<moon::BinaryExpr>();
    repeatedCondition->op = moon::Operator::Greater;
    repeatedCondition->type = boolId;
    repeatedCondition->lhs =
        makeDirectRangeTerminal(context, IteratorOp::Count, i32Id, i32Id, false);
    auto repeatedZero = std::make_unique<moon::IntLiteralExpr>();
    repeatedZero->value = 0;
    repeatedZero->type = i32Id;
    repeatedCondition->rhs = std::move(repeatedZero);
    repeatedConditionLoop->cond = std::move(repeatedCondition);
    repeatedConditionLoop->body = std::make_unique<moon::BlockStmt>();
    repeatedConditionRoot->stmts.push_back(std::move(repeatedConditionLoop));
    auto repeatedConditionCfg =
        cfgBuilder.build(std::move(repeatedConditionRoot), {}, moon::RegionKind::Function, module);
    const moon::RegionRecord* repeatedOuterLoop = nullptr;
    const moon::LocalRecord* repeatedCursor = nullptr;
    size_t repeatedHeaderPredecessors = 0;
    if (repeatedConditionCfg) {
        for (const auto& candidate : repeatedConditionCfg->regions)
            if (!repeatedOuterLoop && candidate.kind == moon::RegionKind::Loop)
                repeatedOuterLoop = &candidate;
        for (const auto& local : repeatedConditionCfg->locals)
            if (local.name.rfind("$terminal.cursor.", 0) == 0) repeatedCursor = &local;
        if (repeatedOuterLoop)
            for (const auto& block : repeatedConditionCfg->blocks)
                if (block.terminator.kind == moon::TerminatorKind::Jump &&
                    block.terminator.primary.target == repeatedOuterLoop->entry)
                    ++repeatedHeaderPredecessors;
    }
    const auto* repeatedCursorScope = repeatedConditionCfg && repeatedCursor
                                          ? repeatedConditionCfg->findScope(repeatedCursor->scope)
                                          : nullptr;
    const auto* repeatedEvaluationRegion =
        repeatedConditionCfg && repeatedCursorScope
            ? repeatedConditionCfg->findRegion(repeatedCursorScope->region)
            : nullptr;
    if (!repeatedConditionCfg || !cfgVerifier.verify(*repeatedConditionCfg, module) ||
        !repeatedOuterLoop || !repeatedCursorScope || !repeatedEvaluationRegion ||
        repeatedEvaluationRegion->kind != moon::RegionKind::Lexical ||
        repeatedEvaluationRegion->parent != repeatedOuterLoop->id ||
        repeatedCursorScope->parent != repeatedOuterLoop->scope || repeatedHeaderPredecessors != 2)
        return fail(
            "repeated while condition did not rebuild synthetic state through its loop header");

    auto countStructured = std::make_unique<moon::BlockStmt>();
    countStructured->stmts.push_back(makeMaterializedRangeBinding(context, "countPending"));
    auto countUse = std::make_unique<moon::ExprStmt>();
    auto sinkCall = std::make_unique<moon::CallExpr>();
    auto sink = std::make_unique<moon::IdentifierExpr>();
    sink->name = "sink";
    sink->type = actionTypeId;
    sinkCall->callee = std::move(sink);
    sinkCall->type = unitId;
    sinkCall->args.push_back(
        makeIteratorTerminal(context, "countPending", IteratorOp::Count, i32Id, i32Id));
    countUse->expr = std::move(sinkCall);
    countStructured->stmts.push_back(std::move(countUse));
    moon::Param sinkParameter;
    sinkParameter.name = "sink";
    sinkParameter.type = actionTypeId;
    auto countCfg = cfgBuilder.build(std::move(countStructured), {sinkParameter},
                                     moon::RegionKind::Function, module);
    bool countResultReachedSink = false;
    if (countCfg)
        for (const auto& block : countCfg->blocks)
            for (const auto& operation : block.operations)
                if (const auto* expression = dynamic_cast<const moon::ExprStmt*>(operation.get()))
                    if (const auto* call =
                            dynamic_cast<const moon::CallExpr*>(expression->expr.get());
                        call && call->iteratorOp == IteratorOp::None && call->args.size() == 1)
                        if (const auto* argument =
                                dynamic_cast<const moon::IdentifierExpr*>(call->args.front().get()))
                            countResultReachedSink =
                                argument->name.rfind("$terminal.count.", 0) == 0;
    if (!countCfg || !cfgVerifier.verify(*countCfg, module) || !countResultReachedSink)
        return fail("materialized count did not normalize before its outer call");

    auto foldStructured = std::make_unique<moon::BlockStmt>();
    foldStructured->stmts.push_back(makeMaterializedRangeBinding(context, "foldPending"));
    auto foldBinding = std::make_unique<moon::LetStmt>();
    foldBinding->name = "folded";
    foldBinding->type = i32Id;
    foldBinding->usage = luna::ownership::Usage::Copy;
    auto transform = std::make_unique<moon::CallExpr>();
    transform->iteratorOp = IteratorOp::Map;
    transform->iteratorInputType = i32Id;
    transform->iteratorOutputType = i32Id;
    transform->type = rangeId;
    auto transformMember = std::make_unique<moon::FieldAccessExpr>();
    transformMember->field = "map";
    auto foldRecipe = std::make_unique<moon::IdentifierExpr>();
    foldRecipe->name = "foldPending";
    foldRecipe->type = rangeId;
    transformMember->object = std::move(foldRecipe);
    transform->callee = std::move(transformMember);
    auto transformFunction = std::make_unique<moon::IdentifierExpr>();
    transformFunction->name = "transform";
    transformFunction->type = lambdaTypeId;
    transform->args.push_back(std::move(transformFunction));
    auto foldTerminal = std::make_unique<moon::CallExpr>();
    foldTerminal->iteratorOp = IteratorOp::Fold;
    foldTerminal->iteratorInputType = i32Id;
    foldTerminal->iteratorOutputType = i32Id;
    foldTerminal->type = i32Id;
    auto foldMember = std::make_unique<moon::FieldAccessExpr>();
    foldMember->field = "fold";
    foldMember->object = std::move(transform);
    foldTerminal->callee = std::move(foldMember);
    auto initial = std::make_unique<moon::IntLiteralExpr>();
    initial->value = 0;
    initial->type = i32Id;
    foldTerminal->args.push_back(std::move(initial));
    auto reducer = std::make_unique<moon::IdentifierExpr>();
    reducer->name = "reducer";
    reducer->type = reducerTypeId;
    foldTerminal->args.push_back(std::move(reducer));
    foldBinding->initializer = std::move(foldTerminal);
    foldStructured->stmts.push_back(std::move(foldBinding));
    moon::Param transformParameter;
    transformParameter.name = "transform";
    transformParameter.type = lambdaTypeId;
    moon::Param reducerParameter;
    reducerParameter.name = "reducer";
    reducerParameter.type = reducerTypeId;
    auto foldCfg =
        cfgBuilder.build(std::move(foldStructured), {transformParameter, reducerParameter},
                         moon::RegionKind::Function, module);
    bool foldedReadsAccumulator = false;
    size_t adapterOrder = static_cast<size_t>(-1);
    size_t accumulatorOrder = static_cast<size_t>(-1);
    size_t reducerOrder = static_cast<size_t>(-1);
    if (foldCfg)
        for (const auto& block : foldCfg->blocks)
            for (size_t operationIndex = 0; operationIndex < block.operations.size();
                 ++operationIndex) {
                const auto& operation = block.operations[operationIndex];
                if (const auto* declaration = dynamic_cast<const moon::LetStmt*>(operation.get());
                    declaration && declaration->name.rfind("$terminal.adapter.", 0) == 0)
                    adapterOrder = operationIndex;
                else if (declaration && declaration->name.rfind("$terminal.fold.", 0) == 0)
                    accumulatorOrder = operationIndex;
                else if (declaration && declaration->name.rfind("$terminal.reducer.", 0) == 0)
                    reducerOrder = operationIndex;
                else if (declaration && declaration->name == "folded")
                    if (const auto* value = dynamic_cast<const moon::IdentifierExpr*>(
                            declaration->initializer.get()))
                        foldedReadsAccumulator = value->name.rfind("$terminal.fold.", 0) == 0;
            }
    if (!foldCfg || !cfgVerifier.verify(*foldCfg, module) || !foldedReadsAccumulator ||
        !(adapterOrder < accumulatorOrder && accumulatorOrder < reducerOrder))
        return fail("materialized Copy fold did not normalize to accumulator CFG");

    auto directFoldStructured = std::make_unique<moon::BlockStmt>();
    auto directFoldUse = std::make_unique<moon::ExprStmt>();
    auto directFoldSink = std::make_unique<moon::CallExpr>();
    auto directSink = std::make_unique<moon::IdentifierExpr>();
    directSink->name = "sink";
    directSink->type = actionTypeId;
    directFoldSink->callee = std::move(directSink);
    directFoldSink->type = unitId;
    auto directFold = makeDirectRangeTerminal(context, IteratorOp::Fold, i32Id, i32Id, true);
    auto directInitial = std::make_unique<moon::IntLiteralExpr>();
    directInitial->value = 0;
    directInitial->type = i32Id;
    directFold->args.push_back(std::move(directInitial));
    auto directReducer = std::make_unique<moon::IdentifierExpr>();
    directReducer->name = "reducer";
    directReducer->type = reducerTypeId;
    directFold->args.push_back(std::move(directReducer));
    directFoldSink->args.push_back(std::move(directFold));
    directFoldUse->expr = std::move(directFoldSink);
    directFoldStructured->stmts.push_back(std::move(directFoldUse));
    auto directFoldCfg =
        cfgBuilder.build(std::move(directFoldStructured), {sinkParameter, reducerParameter},
                         moon::RegionKind::Function, module);
    size_t directCursorOrder = static_cast<size_t>(-1);
    size_t directLimitOrder = static_cast<size_t>(-1);
    size_t directAdapterOrder = static_cast<size_t>(-1);
    size_t directAccumulatorOrder = static_cast<size_t>(-1);
    size_t directReducerOrder = static_cast<size_t>(-1);
    bool directFoldReachedSink = false;
    if (directFoldCfg) {
        const auto& entry = directFoldCfg->blocks.front();
        for (size_t index = 0; index < entry.operations.size(); ++index)
            if (const auto* declaration =
                    dynamic_cast<const moon::LetStmt*>(entry.operations[index].get())) {
                if (declaration->name.rfind("$terminal.cursor.", 0) == 0)
                    directCursorOrder = index;
                else if (declaration->name.rfind("$terminal.limit.", 0) == 0)
                    directLimitOrder = index;
                else if (declaration->name.rfind("$terminal.adapter.", 0) == 0)
                    directAdapterOrder = index;
                else if (declaration->name.rfind("$terminal.fold.", 0) == 0)
                    directAccumulatorOrder = index;
                else if (declaration->name.rfind("$terminal.reducer.", 0) == 0)
                    directReducerOrder = index;
            }
        for (const auto& block : directFoldCfg->blocks)
            for (const auto& operation : block.operations)
                if (const auto* expression = dynamic_cast<const moon::ExprStmt*>(operation.get()))
                    if (const auto* call =
                            dynamic_cast<const moon::CallExpr*>(expression->expr.get());
                        call && call->args.size() == 1)
                        if (const auto* argument =
                                dynamic_cast<const moon::IdentifierExpr*>(call->args.front().get()))
                            directFoldReachedSink = argument->name.rfind("$terminal.fold.", 0) == 0;
    }
    if (!directFoldCfg || !cfgVerifier.verify(*directFoldCfg, module) || !directFoldReachedSink ||
        !(directCursorOrder < directLimitOrder && directLimitOrder < directAdapterOrder &&
          directAdapterOrder < directAccumulatorOrder &&
          directAccumulatorOrder < directReducerOrder))
        return fail("direct Copy fold did not preserve receiver/terminal evaluation order");

    auto affineFoldStructured = std::make_unique<moon::BlockStmt>();
    auto affineFoldBinding = std::make_unique<moon::LetStmt>();
    affineFoldBinding->name = "affineFolded";
    affineFoldBinding->type = stringId;
    affineFoldBinding->usage = luna::ownership::Usage::Affine;
    auto affineFold = makeDirectRangeTerminal(context, IteratorOp::Fold, stringId, stringId, true);
    affineFold->returnUsage = luna::ownership::Usage::Affine;
    auto affineInitial = std::make_unique<moon::StringLiteralExpr>();
    affineInitial->value = "seed";
    affineInitial->type = stringId;
    affineFold->args.push_back(std::move(affineInitial));
    auto affineReducer = std::make_unique<moon::IdentifierExpr>();
    affineReducer->name = "affineReducer";
    affineReducer->type = affineReducerTypeId;
    affineFold->args.push_back(std::move(affineReducer));
    affineFoldBinding->initializer = std::move(affineFold);
    affineFoldStructured->stmts.push_back(std::move(affineFoldBinding));
    auto affineFoldCleanup = std::make_unique<moon::FreeStmt>();
    affineFoldCleanup->isImplicit = true;
    affineFoldCleanup->action = cleanupActionForType(TyString);
    auto affineFoldedIdentifier = std::make_unique<moon::IdentifierExpr>();
    affineFoldedIdentifier->name = "affineFolded";
    affineFoldedIdentifier->type = stringId;
    affineFoldCleanup->operand = std::move(affineFoldedIdentifier);
    affineFoldStructured->stmts.push_back(std::move(affineFoldCleanup));
    moon::Param affineReducerParameter;
    affineReducerParameter.name = "affineReducer";
    affineReducerParameter.type = affineReducerTypeId;
    auto affineFoldCfg = cfgBuilder.build(std::move(affineFoldStructured), {affineReducerParameter},
                                          moon::RegionKind::Function, module);
    moon::LocalRecord* affineAccumulator = nullptr;
    moon::LetStmt* affineDestination = nullptr;
    moon::MoveExpr* affineFinalTransfer = nullptr;
    moon::AssignExpr* affineReplacement = nullptr;
    moon::CallExpr* affineReducerCall = nullptr;
    if (affineFoldCfg) {
        for (auto& local : affineFoldCfg->locals)
            if (local.name.rfind("$terminal.fold.", 0) == 0) affineAccumulator = &local;
        for (auto& block : affineFoldCfg->blocks)
            for (auto& operation : block.operations) {
                if (auto* declaration = dynamic_cast<moon::LetStmt*>(operation.get());
                    declaration && declaration->name == "affineFolded")
                    affineDestination = declaration;
                if (auto* expression = dynamic_cast<moon::ExprStmt*>(operation.get()))
                    if (auto* assignment = dynamic_cast<moon::AssignExpr*>(expression->expr.get());
                        assignment && assignment->op == moon::Operator::Assign) {
                        auto* call = dynamic_cast<moon::CallExpr*>(assignment->rhs.get());
                        if (call && call->type == stringId) {
                            affineReplacement = assignment;
                            affineReducerCall = call;
                        }
                    }
            }
    }
    affineFinalTransfer = affineDestination
                              ? dynamic_cast<moon::MoveExpr*>(affineDestination->initializer.get())
                              : nullptr;
    auto* affineAccumulatorMove =
        affineReducerCall && !affineReducerCall->args.empty()
            ? dynamic_cast<moon::MoveExpr*>(affineReducerCall->args.front().get())
            : nullptr;
    auto* movedAffineAccumulator =
        affineAccumulatorMove
            ? dynamic_cast<moon::IdentifierExpr*>(affineAccumulatorMove->operand.get())
            : nullptr;
    auto* finalAffineAccumulator =
        affineFinalTransfer
            ? dynamic_cast<moon::IdentifierExpr*>(affineFinalTransfer->operand.get())
            : nullptr;
    if (!affineFoldCfg || !cfgVerifier.verify(*affineFoldCfg, module) || !affineAccumulator ||
        affineAccumulator->kind != moon::LocalKind::Synthetic ||
        affineAccumulator->usage != luna::ownership::Usage::Affine || !affineReplacement ||
        !affineReducerCall || affineReducerCall->returnUsage != luna::ownership::Usage::Affine ||
        !movedAffineAccumulator || !finalAffineAccumulator ||
        movedAffineAccumulator->local != affineAccumulator->id ||
        finalAffineAccumulator->local != affineAccumulator->id ||
        affineFoldCfg->cleanups.size() != 2)
        return fail("affine fold did not normalize to a transfer/reinitialize CFG cycle");

    auto savedAffineArgument = std::move(affineReducerCall->args.front());
    auto copiedAffineAccumulator = std::make_unique<moon::IdentifierExpr>();
    copiedAffineAccumulator->name = affineAccumulator->name;
    copiedAffineAccumulator->local = affineAccumulator->id;
    copiedAffineAccumulator->type = affineAccumulator->type;
    affineReducerCall->args.front() = std::move(copiedAffineAccumulator);
    if (cfgVerifier.verify(*affineFoldCfg, module))
        return fail("CFG verifier accepted a copied affine fold accumulator");
    auto activeOverwrite = std::make_unique<moon::StringLiteralExpr>();
    activeOverwrite->value = "replacement";
    activeOverwrite->type = stringId;
    affineReducerCall->args.front() = std::move(activeOverwrite);
    if (cfgVerifier.verify(*affineFoldCfg, module))
        return fail("CFG verifier accepted overwrite of an active affine fold accumulator");
    affineReducerCall->args.front() = std::move(savedAffineArgument);
    if (!cfgVerifier.verify(*affineFoldCfg, module))
        return fail("restored affine fold accumulator transfer did not verify");

    if (!affineDestination) return fail("affine fold destination disappeared from canonical CFG");
    auto savedDestinationInitializer = std::move(affineDestination->initializer);
    auto copiedFinalResult = std::make_unique<moon::IdentifierExpr>();
    copiedFinalResult->name = affineAccumulator->name;
    copiedFinalResult->local = affineAccumulator->id;
    copiedFinalResult->type = affineAccumulator->type;
    affineDestination->initializer = std::move(copiedFinalResult);
    if (cfgVerifier.verify(*affineFoldCfg, module))
        return fail("CFG verifier accepted a copied affine fold result");
    affineDestination->initializer = std::move(savedDestinationInitializer);
    affineFinalTransfer = dynamic_cast<moon::MoveExpr*>(affineDestination->initializer.get());
    if (!affineFinalTransfer) return fail("affine fold final transfer was not restored");
    if (!cfgVerifier.verify(*affineFoldCfg, module))
        return fail("restored affine fold result transfer did not verify");

    if (const int result = runIteratorOrderingTests(context)) return result;

    return 0;
}

} // namespace canonical_test
