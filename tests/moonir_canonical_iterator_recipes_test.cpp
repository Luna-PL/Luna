#include "moonir/MoonIR.h"
#include "moonir/ControlFlowBuilder.h"
#include "moonir/Verifier.h"
#include "core/TypeLayout.h"
#include "moonir_canonical_test_support.h"

#include <algorithm>
#include <iostream>

namespace canonical_test {

int runIteratorRecipeTests(ControlFlowTestContext& context) {
    auto& module = context.module;
    auto& cfgVerifier = context.verifier;
    auto& cfgBuilder = context.builder;
    const auto rangeId = context.rangeId;
    const auto i32Id = context.i32Id;

    auto recipeStructured = std::make_unique<moon::BlockStmt>();
    auto recipeLoop = std::make_unique<moon::ForStmt>();
    recipeLoop->varName = "value";
    recipeLoop->bindingUsage = luna::ownership::Usage::Copy;
    recipeLoop->elementType = i32Id;
    auto rangeCall = std::make_unique<moon::CallExpr>();
    rangeCall->iteratorOp = IteratorOp::Range;
    rangeCall->iteratorInputType = i32Id;
    rangeCall->iteratorOutputType = i32Id;
    rangeCall->type = rangeId;
    auto rangeCallee = std::make_unique<moon::IdentifierExpr>();
    rangeCallee->name = "range";
    rangeCall->callee = std::move(rangeCallee);
    auto rangeStart = std::make_unique<moon::IntLiteralExpr>();
    rangeStart->value = 0;
    rangeStart->type = i32Id;
    auto rangeEnd = std::make_unique<moon::IntLiteralExpr>();
    rangeEnd->value = 4;
    rangeEnd->type = i32Id;
    rangeCall->args.push_back(std::move(rangeStart));
    rangeCall->args.push_back(std::move(rangeEnd));
    auto takeCall = std::make_unique<moon::CallExpr>();
    takeCall->iteratorOp = IteratorOp::Take;
    takeCall->iteratorInputType = i32Id;
    takeCall->iteratorOutputType = i32Id;
    takeCall->type = rangeId;
    auto takeMember = std::make_unique<moon::FieldAccessExpr>();
    takeMember->field = "take";
    takeMember->object = std::move(rangeCall);
    takeCall->callee = std::move(takeMember);
    auto takeCount = std::make_unique<moon::IntLiteralExpr>();
    takeCount->value = 2;
    takeCount->type = i32Id;
    takeCall->args.push_back(std::move(takeCount));
    recipeLoop->iterable = std::move(takeCall);
    recipeLoop->body = std::make_unique<moon::BlockStmt>();
    auto recipeUse = std::make_unique<moon::ExprStmt>();
    auto recipeIdentifier = std::make_unique<moon::IdentifierExpr>();
    recipeIdentifier->name = "value";
    recipeIdentifier->type = i32Id;
    recipeUse->expr = std::move(recipeIdentifier);
    recipeLoop->body->stmts.push_back(std::move(recipeUse));
    recipeStructured->stmts.push_back(std::move(recipeLoop));
    auto recipeCfg = cfgBuilder.build(
        std::move(recipeStructured), {},
        moon::RegionKind::Function, module);
    if (!recipeCfg || !cfgVerifier.verify(*recipeCfg, module) ||
        recipeCfg->blocks.size() != 9 || recipeCfg->locals.size() != 4 ||
        recipeCfg->blocks[1].operations.size() != 3 ||
        recipeCfg->blocks[2].terminator.kind !=
            moon::TerminatorKind::Branch ||
        recipeCfg->blocks[5].terminator.kind !=
            moon::TerminatorKind::Branch ||
        recipeCfg->blocks[8].terminator.primary.target != moon::BlockId{2})
        return fail("range/take recipe did not expand to canonical CFG operations");

    const auto makeMaterializedRangeProgram = [&](size_t loopCount) {
        auto structured = std::make_unique<moon::BlockStmt>();
        structured->stmts.push_back(
            makeMaterializedRangeBinding(context, "pending"));
        for (size_t loopIndex = 0; loopIndex < loopCount; ++loopIndex) {
            auto loop = std::make_unique<moon::ForStmt>();
            loop->varName = "materializedValue" +
                std::to_string(loopIndex);
            loop->bindingUsage = luna::ownership::Usage::Copy;
            loop->elementType = i32Id;
            auto pending = std::make_unique<moon::IdentifierExpr>();
            pending->name = "pending";
            pending->type = rangeId;
            loop->iterable = std::move(pending);
            loop->body = std::make_unique<moon::BlockStmt>();
            structured->stmts.push_back(std::move(loop));
        }
        return structured;
    };
    auto materializedCfg = cfgBuilder.build(
        makeMaterializedRangeProgram(1), {},
        moon::RegionKind::Function, module);
    auto* materializedCursor = materializedCfg &&
            !materializedCfg->blocks.empty() &&
            !materializedCfg->blocks[0].operations.empty()
        ? dynamic_cast<moon::LetStmt*>(
              materializedCfg->blocks[0].operations[0].get())
        : nullptr;
    auto* transferredCursor = materializedCfg &&
            materializedCfg->blocks.size() > 1 &&
            !materializedCfg->blocks[1].operations.empty()
        ? dynamic_cast<moon::LetStmt*>(
              materializedCfg->blocks[1].operations[0].get())
        : nullptr;
    auto* cursorMove = transferredCursor
        ? dynamic_cast<moon::MoveExpr*>(
              transferredCursor->initializer.get())
        : nullptr;
    auto* movedCursor = cursorMove
        ? dynamic_cast<moon::IdentifierExpr*>(cursorMove->operand.get())
        : nullptr;
    bool retainedIteratorLocal = false;
    if (materializedCfg)
        for (const auto& local : materializedCfg->locals) {
            const auto* type = module.findType(local.type);
            retainedIteratorLocal = retainedIteratorLocal ||
                (type && type->kind == TypeKind::Iterator);
        }
    if (!materializedCfg || !cfgVerifier.verify(*materializedCfg, module) ||
        materializedCfg->blocks.size() != 9 ||
        materializedCfg->locals.size() != 6 ||
        materializedCfg->blocks[0].operations.size() != 3 ||
        materializedCfg->blocks[1].operations.size() != 2 ||
        !materializedCursor ||
        materializedCursor->usage != luna::ownership::Usage::Affine ||
        materializedCfg->locals[materializedCursor->local.value].kind !=
            moon::LocalKind::Synthetic ||
        !cursorMove || !movedCursor ||
        movedCursor->local != materializedCursor->local ||
        retainedIteratorLocal)
        return fail("materialized range did not erase to affine ordinary CFG state");

    auto savedCursorTransfer = std::move(transferredCursor->initializer);
    auto invalidCursorRead = std::make_unique<moon::IdentifierExpr>();
    invalidCursorRead->name = movedCursor->name;
    invalidCursorRead->local = movedCursor->local;
    invalidCursorRead->type = movedCursor->type;
    transferredCursor->initializer = std::move(invalidCursorRead);
    if (cfgVerifier.verify(*materializedCfg, module))
        return fail("CFG verifier accepted a copied materialized cursor");
    transferredCursor->initializer = std::move(savedCursorTransfer);
    if (!cfgVerifier.verify(*materializedCfg, module))
        return fail("restored materialized range CFG did not verify");

    auto reusedMaterializedCfg = cfgBuilder.build(
        makeMaterializedRangeProgram(2), {},
        moon::RegionKind::Function, module);
    if (!reusedMaterializedCfg ||
        cfgVerifier.verify(*reusedMaterializedCfg, module))
        return fail("CFG verifier accepted repeated materialized recipe consumption");


    return 0;
}

} // namespace canonical_test
