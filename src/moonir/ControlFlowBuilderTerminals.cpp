#include "ControlFlowBuilder.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <type_traits>
#include <unordered_set>

namespace moon {

std::optional<ControlFlowBuilder::OpenBlock>
ControlFlowBuilder::lowerIteratorTerminal(
    std::unique_ptr<CallExpr> terminal, OpenBlock current,
    RegionId region, ScopeId scope, bool discardUnitResult,
    std::unique_ptr<Expr>& replacement) {
    if (!terminal) return std::nullopt;
    auto* member = dynamic_cast<FieldAccessExpr*>(terminal->callee.get());
    if (!member) {
        error(terminal->location,
              "iterator terminal has no canonical recipe receiver");
        return std::nullopt;
    }
    IteratorRecipePlan plan;
    if (!parseIteratorRecipe(
            std::move(member->object), plan, terminal->location))
        return std::nullopt;
    if (!bindIteratorRecipe(plan) ||
        !validateIteratorRecipe(
            plan, terminal->iteratorInputType, terminal->location, true))
        return std::nullopt;

    TypeRef i32Type;
    TypeRef sizeType;
    TypeRef unitType;
    for (const auto& type : mModule->typeTable) {
        if (type.kind == TypeKind::I32) i32Type = type.id;
        if (type.kind == TypeKind::USize) sizeType = type.id;
        if (type.kind == TypeKind::Unit) unitType = type.id;
    }
    const uint64_t terminalIndex = mTerminalCounter++;
    const std::string identity = std::to_string(terminalIndex);
    const auto identifier = [this, &terminal](LocalId local) {
        auto value = std::make_unique<IdentifierExpr>();
        value->location = terminal->location;
        if (!local.empty() && local.value < mGraph->locals.size()) {
            const auto& record = mGraph->locals[local.value];
            value->name = record.name;
            value->local = local;
            value->type = record.type;
        }
        return value;
    };
    const auto integer = [&terminal](int64_t value, const TypeRef& type) {
        auto literal = std::make_unique<IntLiteralExpr>();
        literal->location = terminal->location;
        literal->value = value;
        literal->type = type;
        return literal;
    };
    const auto addBindingAt = [this, &terminal, scope](
        BlockId block, const std::string& name, const TypeRef& type,
        luna::ownership::Usage usage, std::unique_ptr<Expr> initializer,
        LocalKind kind = LocalKind::Binding,
        bool inferTypeCleanup = true) {
        const LocalId local = addLocal(
            scope, kind, name, type, usage, std::nullopt,
            inferTypeCleanup);
        auto declaration = std::make_unique<LetStmt>();
        declaration->location = terminal->location;
        declaration->name = name;
        declaration->local = local;
        declaration->isLinear = usage == luna::ownership::Usage::Linear;
        declaration->usage = usage;
        declaration->relation = mGraph->locals[local.value].relation;
        declaration->type = type;
        declaration->initializer = std::move(initializer);
        mGraph->blocks[block.value].operations.push_back(
            std::move(declaration));
        return local;
    };
    const auto addBinding = [&addBindingAt, &current](
        const std::string& name, const TypeRef& type,
        std::unique_ptr<Expr> initializer) {
        return addBindingAt(
            current.block, name, type, luna::ownership::Usage::Copy,
            std::move(initializer));
    };

    MaterializedIteratorRecipe recipe;
    recipe.mode = plan.mode;
    recipe.sourceType = plan.sourceType;
    recipe.itemType = plan.itemType;
    if (plan.materialized) {
        recipe.source = plan.materializedSource;
        recipe.index = plan.materializedIndex;
        recipe.limit = plan.materializedLimit;
        recipe.ownsSource = plan.materializedOwnsSource;
    } else {
        const TypeRecord* sourceType = nullptr;
        if (plan.mode != IteratorMode::Range) {
            sourceType = mModule->findType(plan.sourceType);
            auto* sourceIdentifier = dynamic_cast<IdentifierExpr*>(
                plan.source.get());
            if (plan.mode != IteratorMode::Consuming &&
                sourceIdentifier && !sourceIdentifier->local.empty()) {
                recipe.source = sourceIdentifier->local;
                plan.source.reset();
            } else {
                const auto* sourceElement = sourceType
                    ? mModule->findType(sourceType->innerTypeId) : nullptr;
                const bool guardedConsuming = sourceType && sourceElement &&
                    plan.mode == IteratorMode::Consuming &&
                    sourceElement->sysmeta.resource.usage ==
                        luna::ownership::Usage::Affine &&
                    sourceElement->sysmeta.resource.cleanupRequired;
                if (!guardedConsuming &&
                    (!sourceType ||
                     sourceType->sysmeta.resource.cleanupRequired ||
                     sourceType->sysmeta.resource.usage !=
                         luna::ownership::Usage::Copy)) {
                    error(terminal->location,
                          "temporary terminal source requires unsupported "
                          "canonical ownership state");
                    return std::nullopt;
                }
                auto sourceInitializer = std::move(plan.source);
                if (guardedConsuming &&
                    dynamic_cast<IdentifierExpr*>(sourceInitializer.get())) {
                    auto transfer = std::make_unique<MoveExpr>();
                    transfer->location = terminal->location;
                    transfer->type = plan.sourceType;
                    transfer->operand = std::move(sourceInitializer);
                    sourceInitializer = std::move(transfer);
                }
                recipe.source = addBindingAt(
                    current.block, "$terminal.source." + identity,
                    plan.sourceType,
                    guardedConsuming
                        ? luna::ownership::Usage::Affine
                        : luna::ownership::Usage::Copy,
                    std::move(sourceInitializer),
                    LocalKind::Binding,
                    !guardedConsuming);
                if (guardedConsuming)
                    recipe.ownsSource = true;
            }
        }

        const TypeRef loopIndexType = sourceType &&
                sourceType->kind == TypeKind::Slice
            ? sizeType : i32Type;
        std::unique_ptr<Expr> initial;
        std::unique_ptr<Expr> limit;
        if (plan.mode == IteratorMode::Range) {
            initial = std::move(plan.rangeStart);
            limit = std::move(plan.rangeEnd);
        } else {
            initial = integer(0, loopIndexType);
            if (sourceType->kind == TypeKind::Slice) {
                auto length = std::make_unique<SliceLengthExpr>();
                length->location = terminal->location;
                length->slice = identifier(recipe.source);
                length->type = sizeType;
                limit = std::move(length);
            } else {
                limit = integer(
                    static_cast<int64_t>(sourceType->arrayLength),
                    loopIndexType);
            }
        }
        recipe.index = addBindingAt(
            current.block, "$terminal.cursor." + identity,
            loopIndexType, luna::ownership::Usage::Affine,
            std::move(initial), LocalKind::Synthetic);
        recipe.limit = addBindingAt(
            current.block, "$terminal.limit." + identity,
            loopIndexType, luna::ownership::Usage::Copy,
            std::move(limit));
    }

    // A direct receiver is evaluated before its adapters. Adapters appended
    // to materialized state evaluate here as well. Both forms therefore
    // become ordinary locals before any terminal argument is evaluated.
    for (size_t index = 0; index < plan.steps.size(); ++index) {
        auto& step = plan.steps[index];
        if (step.argument) {
            const TypeRef argumentType = step.argument->type;
            step.argumentLocal = addBinding(
                "$terminal.adapter." + identity + "." +
                    std::to_string(index),
                argumentType, std::move(step.argument));
        }
        if (step.argumentLocal.empty()) {
            error(terminal->location,
                  "iterator terminal adapter has no canonical local state");
            return std::nullopt;
        }
        recipe.steps.push_back({
            step.op, step.argumentLocal, step.inputType, step.outputType});
    }
    const std::string recipeName = "$terminal.recipe." + identity;
    mMaterializedIterators.back().emplace(recipeName, std::move(recipe));

    const auto* terminalItemType = mModule->findType(
        terminal->iteratorInputType);
    const bool moveOnlyItem = terminalItemType &&
        terminalItemType->sysmeta.resource.usage ==
            luna::ownership::Usage::Affine &&
        terminalItemType->sysmeta.resource.cleanupRequired;

    auto loop = std::make_unique<ForStmt>();
    loop->location = terminal->location;
    loop->varName = "$terminal.item." + identity;
    {
        const auto* terminalItemType = mModule->findType(
            terminal->iteratorInputType);
        loop->bindingUsage = (terminalItemType &&
            terminalItemType->sysmeta.resource.usage ==
                luna::ownership::Usage::Affine &&
            terminalItemType->sysmeta.resource.cleanupRequired)
            ? luna::ownership::Usage::Affine
            : luna::ownership::Usage::Copy;
    }
    loop->elementType = terminal->iteratorInputType;
    auto iterable = std::make_unique<IdentifierExpr>();
    iterable->location = terminal->location;
    iterable->name = recipeName;
    loop->iterable = std::move(iterable);
    loop->body = std::make_unique<BlockStmt>();
    loop->body->location = terminal->location;

    LocalId resultLocal;
    if (terminal->iteratorOp == IteratorOp::Count) {
        if (!terminal->args.empty() || i32Type.empty() ||
            terminal->type != i32Type ||
            terminal->iteratorOutputType != i32Type ||
            terminal->returnUsage != luna::ownership::Usage::Copy ||
            terminal->returnsLinear) {
            error(terminal->location,
                  "iterator count has no canonical i32 contract");
            return std::nullopt;
        }
        resultLocal = addBinding(
            "$terminal.count." + identity, i32Type,
            integer(0, i32Type));
        auto increment = std::make_unique<ExprStmt>();
        increment->location = terminal->location;
        auto assignment = std::make_unique<AssignExpr>();
        assignment->location = terminal->location;
        assignment->op = Operator::AddAssign;
        assignment->lhs = identifier(resultLocal);
        assignment->rhs = integer(1, i32Type);
        assignment->type = i32Type;
        increment->expr = std::move(assignment);
        loop->body->stmts.push_back(std::move(increment));
        // A count terminal does not consume its item. A move-only item
        // moved out of the source by the for-loop transfer must be dropped
        // each iteration so it does not leak.
        if (moveOnlyItem) {
            auto drop = std::make_unique<FreeStmt>();
            drop->isImplicit = true;
            drop->location = terminal->location;
            auto item = std::make_unique<IdentifierExpr>();
            item->location = terminal->location;
            item->name = loop->varName;
            item->type = terminal->iteratorInputType;
            drop->operand = std::move(item);
            const auto* itemType = mModule->findType(
                terminal->iteratorInputType);
            if (itemType)
                drop->action = itemType->sysmeta.resource.cleanup;
            loop->body->stmts.push_back(std::move(drop));
        }
    } else if (terminal->iteratorOp == IteratorOp::Fold) {
        const auto* accumulatorType = mModule->findType(terminal->type);
        const auto accumulatorUsage = accumulatorType
            ? accumulatorType->sysmeta.resource.usage
            : luna::ownership::Usage::Copy;
        if (terminal->args.size() != 2 || !accumulatorType ||
            accumulatorUsage == luna::ownership::Usage::Linear ||
            terminal->iteratorOutputType != terminal->type ||
            terminal->returnUsage != accumulatorUsage ||
            terminal->returnsLinear) {
            error(terminal->location,
                  "canonical fold requires a Copy or affine accumulator");
            return std::nullopt;
        }
        if (!bindExpr(terminal->args[0].get()) ||
            !bindExpr(terminal->args[1].get()))
            return std::nullopt;
        const auto* reducerType = mModule->findType(
            terminal->args[1]->type);
        if (terminal->args[0]->type != terminal->type ||
            !reducerType ||
            (reducerType->kind != TypeKind::Function &&
             reducerType->kind != TypeKind::Closure) ||
            reducerType->parameterTypeIds != TypeRefVec{
                terminal->type, terminal->iteratorInputType} ||
            reducerType->returnTypeId != terminal->type ||
            reducerType->parameterContracts.size() != 2 ||
            reducerType->parameterContracts[0].relation !=
                luna::ownership::Relation::Owned ||
            reducerType->parameterContracts[0].usage !=
                accumulatorUsage ||
            reducerType->parameterContracts[1].relation !=
                luna::ownership::Relation::Owned ||
            (reducerType->parameterContracts[1].usage !=
                luna::ownership::Usage::Copy &&
             !(moveOnlyItem &&
               reducerType->parameterContracts[1].usage ==
                   luna::ownership::Usage::Affine)) ||
            reducerType->returnContract.relation !=
                luna::ownership::Relation::Owned ||
            reducerType->returnContract.usage !=
                accumulatorUsage) {
            error(terminal->location,
                  "fold reducer disagrees with its accumulator "
                  "ownership contract");
            return std::nullopt;
        }
        resultLocal = addBindingAt(
            current.block, "$terminal.fold." + identity,
            terminal->type, accumulatorUsage,
            std::move(terminal->args[0]),
            accumulatorUsage == luna::ownership::Usage::Affine
                ? LocalKind::Synthetic : LocalKind::Binding);
        const TypeRef reducerTypeId = terminal->args[1]->type;
        const LocalId reducer = addBinding(
            "$terminal.reducer." + identity,
            reducerTypeId, std::move(terminal->args[1]));
        auto reduce = std::make_unique<ExprStmt>();
        reduce->location = terminal->location;
        auto assignment = std::make_unique<AssignExpr>();
        assignment->location = terminal->location;
        assignment->op = Operator::Assign;
        assignment->lhs = identifier(resultLocal);
        auto call = std::make_unique<CallExpr>();
        call->location = terminal->location;
        call->callee = identifier(reducer);
        if (accumulatorUsage == luna::ownership::Usage::Affine) {
            auto transfer = std::make_unique<MoveExpr>();
            transfer->location = terminal->location;
            transfer->type = terminal->type;
            transfer->operand = identifier(resultLocal);
            call->args.push_back(std::move(transfer));
        } else {
            call->args.push_back(identifier(resultLocal));
        }
        auto item = std::make_unique<IdentifierExpr>();
        item->location = terminal->location;
        item->name = loop->varName;
        item->type = terminal->iteratorInputType;
        if (moveOnlyItem) {
            auto transfer = std::make_unique<MoveExpr>();
            transfer->location = terminal->location;
            transfer->type = terminal->iteratorInputType;
            transfer->operand = std::move(item);
            call->args.push_back(std::move(transfer));
        } else {
            call->args.push_back(std::move(item));
        }
        call->type = terminal->type;
        call->returnUsage = accumulatorUsage;
        assignment->rhs = std::move(call);
        assignment->type = terminal->type;
        reduce->expr = std::move(assignment);
        loop->body->stmts.push_back(std::move(reduce));
    } else if (terminal->iteratorOp == IteratorOp::ForEach) {
        if (!discardUnitResult) {
            error(terminal->location,
                  "for_each is canonical only as an expression statement");
            return std::nullopt;
        }
        if (terminal->args.size() != 1 || unitType.empty() ||
            terminal->type != unitType ||
            terminal->iteratorOutputType != unitType ||
            terminal->returnUsage != luna::ownership::Usage::Copy ||
            terminal->returnsLinear ||
            !bindExpr(terminal->args[0].get())) {
            error(terminal->location,
                  "for_each has no canonical action contract");
            return std::nullopt;
        }
        const auto* actionType = mModule->findType(
            terminal->args[0]->type);
        if (!actionType ||
            (actionType->kind != TypeKind::Function &&
             actionType->kind != TypeKind::Closure) ||
            actionType->parameterTypeIds !=
                TypeRefVec{terminal->iteratorInputType} ||
            actionType->returnTypeId != unitType ||
            actionType->parameterContracts.size() != 1 ||
            (actionType->parameterContracts[0].usage !=
                luna::ownership::Usage::Copy &&
             !(moveOnlyItem &&
               actionType->parameterContracts[0].usage ==
                   luna::ownership::Usage::Affine)) ||
            actionType->returnContract.usage !=
                luna::ownership::Usage::Copy) {
            error(terminal->location,
                  "for_each action disagrees with its Copy terminal contract");
            return std::nullopt;
        }
        const TypeRef actionTypeId = terminal->args[0]->type;
        const LocalId action = addBinding(
            "$terminal.action." + identity,
            actionTypeId, std::move(terminal->args[0]));
        auto invoke = std::make_unique<ExprStmt>();
        invoke->location = terminal->location;
        auto call = std::make_unique<CallExpr>();
        call->location = terminal->location;
        call->callee = identifier(action);
        auto item = std::make_unique<IdentifierExpr>();
        item->location = terminal->location;
        item->name = loop->varName;
        item->type = terminal->iteratorInputType;
        if (moveOnlyItem) {
            auto transfer = std::make_unique<MoveExpr>();
            transfer->location = terminal->location;
            transfer->type = terminal->iteratorInputType;
            transfer->operand = std::move(item);
            call->args.push_back(std::move(transfer));
        } else {
            call->args.push_back(std::move(item));
        }
        call->type = unitType;
        call->returnUsage = luna::ownership::Usage::Copy;
        invoke->expr = std::move(call);
        loop->body->stmts.push_back(std::move(invoke));
    } else if (terminal->iteratorOp == IteratorOp::Collect) {
        const auto* builderType = mModule->findType(
            terminal->iteratorCollectBuilderType);
        const auto* targetType = mModule->findType(
            terminal->iteratorCollectTargetType);
        const auto* beginDeclaration = mModule->findDeclaration(
            terminal->iteratorCollectBegin);
        const auto* pushDeclaration = mModule->findDeclaration(
            terminal->iteratorCollectPush);
        const auto* finishDeclaration = mModule->findDeclaration(
            terminal->iteratorCollectFinish);
        const auto* beginType = beginDeclaration
            ? mModule->findType(beginDeclaration->type) : nullptr;
        const auto* pushType = pushDeclaration
            ? mModule->findType(pushDeclaration->type) : nullptr;
        const auto* finishType = finishDeclaration
            ? mModule->findType(finishDeclaration->type) : nullptr;
        const auto* builderBorrow = pushType &&
                !pushType->parameterTypeIds.empty()
            ? mModule->findType(pushType->parameterTypeIds.front())
            : nullptr;
        const luna::ownership::Contract ownedAffine{
            luna::ownership::Relation::Owned,
            luna::ownership::Usage::Affine};
        const luna::ownership::Contract mutableBorrow{
            luna::ownership::Relation::MutableBorrow,
            luna::ownership::Usage::Copy};
        const luna::ownership::Contract ownedCopy{
            luna::ownership::Relation::Owned,
            luna::ownership::Usage::Copy};
        const bool declarationsAreFunctions =
            beginDeclaration && pushDeclaration && finishDeclaration &&
            beginDeclaration->kind == DeclarationKind::Function &&
            pushDeclaration->kind == DeclarationKind::Function &&
            finishDeclaration->kind == DeclarationKind::Function;
        const bool canonicalBegin = beginType &&
            beginType->kind == TypeKind::Function &&
            beginType->parameterTypeIds.empty() &&
            beginType->parameterContracts.empty() &&
            beginType->returnTypeId == terminal->iteratorCollectBuilderType &&
            beginType->returnContract == ownedAffine;
        const bool canonicalPush = pushType &&
            pushType->kind == TypeKind::Function &&
            pushType->parameterTypeIds.size() == 2 &&
            pushType->parameterTypeIds[1] == terminal->iteratorInputType &&
            pushType->parameterContracts ==
                std::vector<luna::ownership::Contract>{
                    mutableBorrow, ownedAffine} &&
            pushType->returnTypeId == unitType &&
            pushType->returnContract == ownedCopy &&
            builderBorrow && builderBorrow->kind == TypeKind::Reference &&
            builderBorrow->isMutable &&
            builderBorrow->innerTypeId ==
                terminal->iteratorCollectBuilderType;
        const bool canonicalFinish = finishType &&
            finishType->kind == TypeKind::Function &&
            finishType->parameterTypeIds == TypeRefVec{
                terminal->iteratorCollectBuilderType} &&
            finishType->parameterContracts ==
                std::vector<luna::ownership::Contract>{ownedAffine} &&
            finishType->returnTypeId == terminal->iteratorCollectTargetType &&
            finishType->returnContract == ownedAffine;
        if (discardUnitResult || !terminal->args.empty() || unitType.empty() ||
            !builderType || !targetType || !declarationsAreFunctions ||
            !canonicalBegin || !canonicalPush || !canonicalFinish ||
            terminal->type != terminal->iteratorCollectTargetType ||
            terminal->iteratorOutputType !=
                terminal->iteratorCollectTargetType ||
            terminal->returnUsage != luna::ownership::Usage::Affine ||
            terminal->returnsLinear ||
            builderType->domain != luna::types::TypeDomain::Value ||
            targetType->domain != luna::types::TypeDomain::Value ||
            builderType->sysmeta.resource.usage ==
                luna::ownership::Usage::Linear ||
            targetType->sysmeta.resource.usage ==
                luna::ownership::Usage::Linear) {
            error(terminal->location,
                  "collect has no canonical affine "
                  "FromIterator builder contract");
            return std::nullopt;
        }

        const auto directCall = [&terminal](
            const DeclarationRef& reference,
            const DeclarationRecord& declaration,
            const TypeRecord& signature) {
            auto call = std::make_unique<CallExpr>();
            call->location = terminal->location;
            call->calleeRef = reference;
            call->type = signature.returnTypeId;
            call->returnUsage = signature.returnContract.usage;
            call->returnsLinear =
                call->returnUsage == luna::ownership::Usage::Linear;
            auto callee = std::make_unique<IdentifierExpr>();
            callee->location = terminal->location;
            callee->name = declaration.sourceName;
            callee->declaration = reference;
            callee->type = declaration.type;
            call->callee = std::move(callee);
            return call;
        };

        auto begin = directCall(
            terminal->iteratorCollectBegin,
            *beginDeclaration, *beginType);
        const LocalId builder = addBindingAt(
            current.block, "$terminal.collect.builder." + identity,
            terminal->iteratorCollectBuilderType,
            luna::ownership::Usage::Affine, std::move(begin),
            LocalKind::Synthetic);

        auto push = directCall(
            terminal->iteratorCollectPush,
            *pushDeclaration, *pushType);
        auto borrowedBuilder = std::make_unique<BorrowExpr>();
        borrowedBuilder->location = terminal->location;
        borrowedBuilder->isMutable = true;
        borrowedBuilder->type = pushType->parameterTypeIds.front();
        borrowedBuilder->operand = identifier(builder);
        push->args.push_back(std::move(borrowedBuilder));
        auto item = std::make_unique<IdentifierExpr>();
        item->location = terminal->location;
        item->name = loop->varName;
        item->type = terminal->iteratorInputType;
        if (moveOnlyItem) {
            auto transfer = std::make_unique<MoveExpr>();
            transfer->location = terminal->location;
            transfer->type = terminal->iteratorInputType;
            transfer->operand = std::move(item);
            push->args.push_back(std::move(transfer));
        } else {
            push->args.push_back(std::move(item));
        }
        auto pushStatement = std::make_unique<ExprStmt>();
        pushStatement->location = terminal->location;
        pushStatement->expr = std::move(push);
        loop->body->stmts.push_back(std::move(pushStatement));

        auto lowered = lowerIteratorRecipeFor(
            std::move(loop), std::move(current), region, scope);
        if (!lowered) return std::nullopt;

        auto finish = directCall(
            terminal->iteratorCollectFinish,
            *finishDeclaration, *finishType);
        auto movedBuilder = std::make_unique<MoveExpr>();
        movedBuilder->location = terminal->location;
        movedBuilder->type = terminal->iteratorCollectBuilderType;
        movedBuilder->operand = identifier(builder);
        finish->args.push_back(std::move(movedBuilder));
        resultLocal = addBindingAt(
            lowered->block, "$terminal.collect.result." + identity,
            terminal->iteratorCollectTargetType,
            luna::ownership::Usage::Affine, std::move(finish),
            LocalKind::Synthetic);

        auto transfer = std::make_unique<MoveExpr>();
        transfer->location = terminal->location;
        transfer->type = terminal->iteratorCollectTargetType;
        transfer->operand = identifier(resultLocal);
        replacement = std::move(transfer);
        return lowered;
    } else {
        error(terminal->location,
              "unsupported iterator terminal operation");
        return std::nullopt;
    }

    auto lowered = lowerIteratorRecipeFor(
        std::move(loop), std::move(current), region, scope);
    if (!lowered) return std::nullopt;
    if (!resultLocal.empty()) {
        const auto* result = mGraph->findLocal(resultLocal);
        if (result && luna::ownership::isMoveOnly(result->usage)) {
            auto transfer = std::make_unique<MoveExpr>();
            transfer->location = terminal->location;
            transfer->type = result->type;
            transfer->operand = identifier(resultLocal);
            replacement = std::move(transfer);
        } else {
            replacement = identifier(resultLocal);
        }
    }
    return lowered;
}

} // namespace moon
