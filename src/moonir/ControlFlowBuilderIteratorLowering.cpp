#include "ControlFlowBuilder.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <type_traits>
#include <unordered_set>

namespace moon {
bool ControlFlowBuilder::materializeIteratorRecipe(
    std::unique_ptr<LetStmt> declaration, OpenBlock current,
    ScopeId scope) {
    if (!declaration || declaration->name.empty()) {
        error(declaration ? declaration->location : SourceLocation{},
              "materialized iterator binding has no canonical name");
        return false;
    }
    if (mBindings.empty() || mMaterializedIterators.empty() ||
        mBindings.back().count(declaration->name) ||
        mMaterializedIterators.back().count(declaration->name)) {
        error(declaration->location,
              "duplicate local '" + declaration->name +
              "' in one canonical scope");
        return false;
    }
    const auto* iteratorType = mModule->findType(declaration->type);
    if (!iteratorType || iteratorType->kind != TypeKind::Iterator ||
        iteratorType->innerTypeId.empty()) {
        error(declaration->location,
              "materialized iterator binding has no frozen iterator contract");
        return false;
    }
    IteratorRecipePlan plan;
    if (!parseIteratorRecipe(
            std::move(declaration->initializer), plan,
            declaration->location))
        return false;
    if (plan.materialized) {
        error(declaration->location,
              "materialized iterator binding cannot wrap existing recipe state");
        return false;
    }
    if (!bindIteratorRecipe(plan) ||
        !validateIteratorRecipe(
            plan, iteratorType->innerTypeId, declaration->location,
            declaration->materializedIteratorOwnsSource))
        return false;

    TypeRef indexType;
    TypeRef sizeType;
    for (const auto& type : mModule->typeTable) {
        if (type.kind == TypeKind::I32) indexType = type.id;
        if (type.kind == TypeKind::USize) sizeType = type.id;
    }
    const auto identifier = [this, &declaration](LocalId local) {
        auto value = std::make_unique<IdentifierExpr>();
        value->location = declaration->location;
        if (!local.empty() && local.value < mGraph->locals.size()) {
            const auto& record = mGraph->locals[local.value];
            value->name = record.name;
            value->local = local;
            value->type = record.type;
        }
        return value;
    };
    const auto integer = [&declaration](int64_t value, const TypeRef& type) {
        auto literal = std::make_unique<IntLiteralExpr>();
        literal->location = declaration->location;
        literal->value = value;
        literal->type = type;
        return literal;
    };
    const auto addStateBinding = [this, &declaration, current, scope](
        const std::string& name, const TypeRef& type,
        luna::ownership::Usage usage, std::unique_ptr<Expr> initializer,
        LocalKind kind = LocalKind::Binding,
        bool inferTypeCleanup = true) {
        const LocalId local = addLocal(
            scope, kind, name, type, usage, std::nullopt,
            inferTypeCleanup);
        auto state = std::make_unique<LetStmt>();
        state->location = declaration->location;
        state->name = name;
        state->local = local;
        state->isLinear = usage == luna::ownership::Usage::Linear;
        state->usage = usage;
        state->relation = mGraph->locals[local.value].relation;
        state->type = type;
        state->initializer = std::move(initializer);
        mGraph->blocks[current.block.value].operations.push_back(
            std::move(state));
        return local;
    };

    MaterializedIteratorRecipe materialized;
    materialized.mode = plan.mode;
    materialized.sourceType = plan.sourceType;
    materialized.itemType = plan.itemType;
    materialized.ownsSource = declaration->materializedIteratorOwnsSource;
    const TypeRecord* sourceType = nullptr;
    if (plan.mode != IteratorMode::Range) {
        sourceType = mModule->findType(plan.sourceType);
        auto* sourceIdentifier = dynamic_cast<IdentifierExpr*>(
            plan.source.get());
        if (plan.mode != IteratorMode::Consuming) {
            if (!sourceIdentifier || sourceIdentifier->local.empty()) {
                error(declaration->location,
                      "borrowed materialized recipe requires a canonical source local");
                return false;
            }
            materialized.source = sourceIdentifier->local;
        } else {
            auto sourceInitializer = std::move(plan.source);
            const auto sourceUsage = declaration->materializedIteratorOwnsSource
                ? mModule->findType(plan.sourceType)->sysmeta.resource.usage
                : luna::ownership::Usage::Copy;
            if (declaration->materializedIteratorOwnsSource &&
                dynamic_cast<IdentifierExpr*>(sourceInitializer.get())) {
                auto transfer = std::make_unique<MoveExpr>();
                transfer->location = declaration->location;
                transfer->type = plan.sourceType;
                transfer->operand = std::move(sourceInitializer);
                sourceInitializer = std::move(transfer);
            }
            materialized.source = addStateBinding(
                "$recipe." + declaration->name + ".source",
                plan.sourceType, sourceUsage, std::move(sourceInitializer),
                LocalKind::Binding,
                !declaration->materializedIteratorOwnsSource);
        }
    }

    const TypeRef loopIndexType = sourceType &&
            sourceType->kind == TypeKind::Slice
        ? sizeType : indexType;
    std::unique_ptr<Expr> initial;
    std::unique_ptr<Expr> limit;
    if (plan.mode == IteratorMode::Range) {
        initial = std::move(plan.rangeStart);
        limit = std::move(plan.rangeEnd);
    } else {
        initial = integer(0, loopIndexType);
        if (sourceType->kind == TypeKind::Slice) {
            auto length = std::make_unique<SliceLengthExpr>();
            length->location = declaration->location;
            length->slice = identifier(materialized.source);
            length->type = sizeType;
            limit = std::move(length);
        } else {
            limit = integer(
                static_cast<int64_t>(sourceType->arrayLength),
                loopIndexType);
        }
    }
    materialized.index = addStateBinding(
        "$recipe." + declaration->name + ".index",
        loopIndexType, luna::ownership::Usage::Affine,
        std::move(initial), LocalKind::Synthetic);
    materialized.limit = addStateBinding(
        "$recipe." + declaration->name + ".limit",
        loopIndexType, luna::ownership::Usage::Copy,
        std::move(limit));

    const auto* materializedElement = sourceType &&
            sourceType->kind == TypeKind::Array
        ? mModule->findType(sourceType->innerTypeId) : nullptr;
    if (materialized.ownsSource && materializedElement &&
        materializedElement->sysmeta.resource.usage ==
            luna::ownership::Usage::Affine &&
        materializedElement->sysmeta.resource.cleanupRequired) {
        materialized.nextUnread = addStateBinding(
            "$recipe." + declaration->name + ".next-unread",
            loopIndexType, luna::ownership::Usage::Copy,
            integer(0, loopIndexType), LocalKind::Synthetic);
        for (uint64_t element = 0;
             element < sourceType->arrayLength; ++element) {
            const CleanupId cleanup{
                static_cast<uint32_t>(mGraph->cleanups.size())};
            CleanupRecord record;
            record.id = cleanup;
            record.scope = scope;
            record.place.root = materialized.source;
            record.place.projections.push_back({
                ProjectionKind::ConstantIndex, element, {}});
            record.type = sourceType->innerTypeId;
            record.kind = CleanupKind::Value;
            record.action =
                materializedElement->sysmeta.resource.cleanup;
            record.guard = CleanupGuard{
                materialized.nextUnread, element};
            mGraph->cleanups.push_back(std::move(record));
            mGraph->scopes[scope.value].cleanups.push_back(cleanup);
            materialized.sourceTailCleanups.push_back(cleanup);
        }
    }

    for (size_t stepIndex = 0; stepIndex < plan.steps.size(); ++stepIndex) {
        auto& step = plan.steps[stepIndex];
        const TypeRef argumentType = step.op == IteratorOp::Take
            ? indexType : step.argument->type;
        const LocalId argument = addStateBinding(
            "$recipe." + declaration->name + ".step." +
                std::to_string(stepIndex),
            argumentType, luna::ownership::Usage::Copy,
            std::move(step.argument));
        materialized.steps.push_back({
            step.op, argument, step.inputType, step.outputType});
    }
    mMaterializedIterators.back().emplace(
        declaration->name, std::move(materialized));
    return true;
}

std::optional<ControlFlowBuilder::OpenBlock>
ControlFlowBuilder::lowerIteratorRecipeFor(
    std::unique_ptr<ForStmt> statement, OpenBlock current,
    RegionId region, ScopeId scope) {
    IteratorRecipePlan plan;
    if (!parseIteratorRecipe(
            std::move(statement->iterable), plan, statement->location))
        return std::nullopt;
    if (!bindIteratorRecipe(plan) ||
        !validateIteratorRecipe(
            plan, statement->elementType, statement->location, true))
        return std::nullopt;

    TypeRef boolType;
    TypeRef indexType;
    TypeRef sizeType;
    for (const auto& type : mModule->typeTable) {
        if (type.kind == TypeKind::Bool) boolType = type.id;
        if (type.kind == TypeKind::I32) indexType = type.id;
        if (type.kind == TypeKind::USize) sizeType = type.id;
    }
    if (boolType.empty() || indexType.empty()) {
        error(statement->location,
              "iterator recipe requires frozen bool and i32 compiler types");
        return std::nullopt;
    }
    const TypeRef sourceItemType = plan.steps.empty()
        ? plan.itemType : plan.steps.front().inputType;

    const RegionId loopRegion = addRegion(
        region, RegionKind::Loop, statement->location);
    const ScopeId loopScope = addScope(
        scope, loopRegion, statement->location);
    const BlockId init = addBlock(
        loopRegion, loopScope, statement->location);
    pushBindings();
    const std::string identity = std::to_string(loopRegion.value);

    const auto addBinding = [&](const std::string& name, const TypeRef& type,
                                std::unique_ptr<Expr> initializer,
                                LocalKind kind = LocalKind::Binding,
                                bool inferTypeCleanup = true) {
        const auto* frozen = mModule->findType(type);
        const auto usage = frozen
            ? frozen->sysmeta.resource.usage
            : luna::ownership::Usage::Copy;
        const LocalId local = addLocal(
            loopScope, kind, name, type, usage,
            std::nullopt, inferTypeCleanup);
        auto declaration = std::make_unique<LetStmt>();
        declaration->location = statement->location;
        declaration->name = name;
        declaration->local = local;
        declaration->isLinear = usage == luna::ownership::Usage::Linear;
        declaration->usage = usage;
        declaration->relation = mGraph->locals[local.value].relation;
        declaration->type = type;
        declaration->initializer = std::move(initializer);
        mGraph->blocks[init.value].operations.push_back(
            std::move(declaration));
        return local;
    };
    const auto identifier = [this, &statement](LocalId local) {
        auto value = std::make_unique<IdentifierExpr>();
        value->location = statement->location;
        if (!local.empty() && local.value < mGraph->locals.size()) {
            const auto& record = mGraph->locals[local.value];
            value->name = record.name;
            value->local = local;
            value->type = record.type;
        }
        return value;
    };
    const auto integer = [&statement](int64_t value, const TypeRef& type) {
        auto literal = std::make_unique<IntLiteralExpr>();
        literal->location = statement->location;
        literal->value = value;
        literal->type = type;
        return literal;
    };

    LocalId sourceLocal;
    LocalId nextUnreadLocal;
    std::vector<CleanupId> sourceTailCleanups;
    bool guardedConsumingSource = false;
    const TypeRecord* sourceType = nullptr;
    if (plan.mode != IteratorMode::Range) {
        sourceType = mModule->findType(plan.sourceType);
        if (plan.materialized) {
            if (plan.mode == IteratorMode::Consuming &&
                plan.materializedOwnsSource) {
                const auto* frozen = mModule->findType(plan.sourceType);
                const auto* element = frozen &&
                        frozen->kind == TypeKind::Array
                    ? mModule->findType(frozen->innerTypeId) : nullptr;
                if (element &&
                    element->sysmeta.resource.usage ==
                        luna::ownership::Usage::Affine &&
                    element->sysmeta.resource.cleanupRequired)
                    guardedConsumingSource = true;
            }
            if (guardedConsumingSource) {
                auto transfer = std::make_unique<MoveExpr>();
                transfer->location = statement->location;
                transfer->type = plan.sourceType;
                transfer->operand = identifier(plan.materializedSource);
                sourceLocal = addBinding(
                    "$for.source." + identity, plan.sourceType,
                    std::move(transfer), LocalKind::Binding, false);
            } else {
                sourceLocal = plan.materializedSource;
            }
        } else {
            auto* sourceIdentifier = dynamic_cast<IdentifierExpr*>(
                plan.source.get());
            if (!sourceIdentifier) {
                if (auto* move = dynamic_cast<MoveExpr*>(plan.source.get()))
                    sourceIdentifier = dynamic_cast<IdentifierExpr*>(
                        move->operand.get());
            }
            const bool snapshot = plan.mode == IteratorMode::Consuming;
            const auto* frozen = mModule->findType(plan.sourceType);
            const auto* element = frozen &&
                    frozen->kind == TypeKind::Array
                ? mModule->findType(frozen->innerTypeId) : nullptr;
            guardedConsumingSource = frozen && element &&
                plan.mode == IteratorMode::Consuming &&
                element->sysmeta.resource.usage ==
                    luna::ownership::Usage::Affine &&
                element->sysmeta.resource.cleanupRequired;
            if (!snapshot && sourceIdentifier &&
                !sourceIdentifier->local.empty()) {
                sourceLocal = sourceIdentifier->local;
                plan.source.reset();
            } else {
                if (!frozen ||
                    (frozen->sysmeta.resource.cleanupRequired &&
                     !guardedConsumingSource)) {
                    error(statement->location,
                          "temporary iterator source requires unsupported synthetic cleanup");
                    popBindings();
                    return std::nullopt;
                }
                auto sourceInitializer = std::move(plan.source);
                if (guardedConsumingSource &&
                    dynamic_cast<IdentifierExpr*>(sourceInitializer.get())) {
                    auto transfer = std::make_unique<MoveExpr>();
                    transfer->location = statement->location;
                    transfer->type = plan.sourceType;
                    transfer->operand = std::move(sourceInitializer);
                    sourceInitializer = std::move(transfer);
                }
                sourceLocal = addBinding(
                    "$for.source." + identity, plan.sourceType,
                    std::move(sourceInitializer), LocalKind::Binding,
                    !guardedConsumingSource);
            }
        }
    }
    if (!statement->recipeStateName.empty() &&
        !guardedConsumingSource) {
        error(statement->location,
              "move-only iterator recipe state requires a direct affine array source");
        popBindings();
        return std::nullopt;
    }

    std::unique_ptr<Expr> initial;
    std::unique_ptr<Expr> limit;
    const TypeRef loopIndexType = sourceType &&
            sourceType->kind == TypeKind::Slice
        ? sizeType : indexType;
    if (plan.materialized) {
        if (guardedConsumingSource) {
            // The materialized recipe owns its source in an outer scope; the
            // for-loop's guarded cursor is a fresh Copy synthetic local in
            // loopScope initialized to zero, so the guarded tail cleanup and
            // cursor share loopScope (matching the verifier's cursor-scope
            // rule) while the source local stays in the recipe's scope. The
            // recipe's own index local is left for the recipe to manage.
            initial = integer(0, loopIndexType);
        } else {
            auto transfer = std::make_unique<MoveExpr>();
            transfer->location = statement->location;
            transfer->operand = identifier(plan.materializedIndex);
            transfer->type = loopIndexType;
            initial = std::move(transfer);
        }
        limit = identifier(plan.materializedLimit);
    } else if (plan.mode == IteratorMode::Range) {
        initial = std::move(plan.rangeStart);
        limit = std::move(plan.rangeEnd);
    } else {
        initial = integer(0, loopIndexType);
        if (sourceType->kind == TypeKind::Slice) {
            auto length = std::make_unique<SliceLengthExpr>();
            length->location = statement->location;
            length->slice = identifier(sourceLocal);
            length->type = sizeType;
            limit = std::move(length);
        } else {
            limit = integer(
                static_cast<int64_t>(sourceType->arrayLength),
                loopIndexType);
        }
    }
    const LocalId indexLocal = addBinding(
        "$for.index." + identity, loopIndexType, std::move(initial),
        guardedConsumingSource
            ? LocalKind::Synthetic : LocalKind::Binding);
    const LocalId limitLocal = addBinding(
        "$for.limit." + identity, loopIndexType, std::move(limit));
    if (guardedConsumingSource) {
        nextUnreadLocal = indexLocal;
        const auto* elementType = mModule->findType(
            sourceType->innerTypeId);
        for (uint64_t element = 0;
             element < sourceType->arrayLength; ++element) {
            const CleanupId cleanup{
                static_cast<uint32_t>(mGraph->cleanups.size())};
            CleanupRecord record;
            record.id = cleanup;
            record.scope = loopScope;
            record.place.root = sourceLocal;
            record.place.projections.push_back({
                ProjectionKind::ConstantIndex, element, {}});
            record.type = sourceType->innerTypeId;
            record.kind = CleanupKind::Value;
            record.action = elementType->sysmeta.resource.cleanup;
            record.guard = CleanupGuard{nextUnreadLocal, element};
            mGraph->cleanups.push_back(std::move(record));
            mGraph->scopes[loopScope.value].cleanups.push_back(cleanup);
            sourceTailCleanups.push_back(cleanup);
        }
    }
    std::vector<LocalId> adapterLocals;
    for (size_t stepIndex = 0; stepIndex < plan.steps.size(); ++stepIndex) {
        auto& step = plan.steps[stepIndex];
        const char* adapterName = step.op == IteratorOp::Map
            ? "map" : (step.op == IteratorOp::Filter ? "filter" : "take");
        if (!step.argumentLocal.empty()) {
            adapterLocals.push_back(step.argumentLocal);
        } else {
            const TypeRef adapterType = step.op == IteratorOp::Take
                ? indexType : step.argument->type;
            adapterLocals.push_back(addBinding(
                "$for." + std::string(adapterName) + "." + identity + "." +
                    std::to_string(stepIndex),
                adapterType, std::move(step.argument)));
        }
    }

    const BlockId condition = addBlock(
        loopRegion, loopScope, statement->location);
    connectJump(current, init);
    connectJump(OpenBlock{init, {}}, condition);

    const RegionId bodyRegion = addRegion(
        loopRegion, RegionKind::Lexical, statement->location);
    const ScopeId bodyScope = addScope(
        loopScope, bodyRegion, statement->location);
    const BlockId bodyEntry = addBlock(
        bodyRegion, bodyScope, statement->location);
    const BlockId exit = addBlock(region, scope, statement->location);
    const bool hasFilter = std::any_of(
        plan.steps.begin(), plan.steps.end(), [](const auto& step) {
            return step.op == IteratorOp::Filter;
        });
    BlockId increment;
    if (hasFilter)
        increment = addBlock(
            loopRegion, loopScope, statement->location);

    Terminator conditionTerminator;
    conditionTerminator.kind = TerminatorKind::Branch;
    conditionTerminator.location = statement->location;
    auto hasItem = std::make_unique<BinaryExpr>();
    hasItem->location = statement->location;
    hasItem->lhs = identifier(indexLocal);
    hasItem->op = Operator::Less;
    hasItem->rhs = identifier(limitLocal);
    hasItem->type = boolType;
    conditionTerminator.operand = std::move(hasItem);
    conditionTerminator.primary.target = bodyEntry;
    conditionTerminator.secondary.target = exit;
    conditionTerminator.secondary.cleanups = canonicalCleanupOrder(
        sourceTailCleanups, loopScope, scope);
    mGraph->blocks[condition.value].terminator =
        std::move(conditionTerminator);

    pushBindings();
    const auto addBodyBinding = [&](BlockId block, const std::string& name,
                                    const TypeRef& type,
                                    luna::ownership::Usage usage,
                                    std::unique_ptr<Expr> initializer,
                                    LocalKind kind = LocalKind::Binding) {
        const LocalId local = addLocal(
            bodyScope, kind, name, type, usage);
        auto declaration = std::make_unique<LetStmt>();
        declaration->location = statement->location;
        declaration->name = name;
        declaration->local = local;
        declaration->isLinear =
            usage == luna::ownership::Usage::Linear;
        declaration->usage = usage;
        declaration->relation = mGraph->locals[local.value].relation;
        declaration->type = type;
        declaration->initializer = std::move(initializer);
        mGraph->blocks[block.value].operations.push_back(
            std::move(declaration));
        return local;
    };
    const auto invokeAdapter = [&](LocalId callable, LocalId argument,
                                   const TypeRef& resultType) {
        auto call = std::make_unique<CallExpr>();
        call->location = statement->location;
        call->callee = identifier(callable);
        std::unique_ptr<Expr> argumentValue = identifier(argument);
        call->type = resultType;
        if (!callable.empty() && callable.value < mGraph->locals.size()) {
            const auto* signature = mModule->findType(
                mGraph->locals[callable.value].type);
            if (signature && signature->kind == TypeKind::Function) {
                call->returnUsage = signature->returnContract.usage;
                if (!signature->parameterContracts.empty()) {
                    const auto relation =
                        signature->parameterContracts.front().relation;
                    if (relation ==
                            luna::ownership::Relation::SharedBorrow ||
                        relation ==
                            luna::ownership::Relation::MutableBorrow) {
                        auto borrowed = std::make_unique<BorrowExpr>();
                        borrowed->location = statement->location;
                        borrowed->isMutable = relation ==
                            luna::ownership::Relation::MutableBorrow;
                        borrowed->type = argumentValue->type;
                        borrowed->operand = std::move(argumentValue);
                        argumentValue = std::move(borrowed);
                    } else if (!argument.empty() &&
                               argument.value < mGraph->locals.size() &&
                               luna::ownership::isMoveOnly(
                                   mGraph->locals[argument.value].usage)) {
                        auto transfer = std::make_unique<MoveExpr>();
                        transfer->location = statement->location;
                        transfer->type = argumentValue->type;
                        transfer->operand = std::move(argumentValue);
                        argumentValue = std::move(transfer);
                    }
                }
            }
        }
        call->args.push_back(std::move(argumentValue));
        call->returnsLinear =
            call->returnUsage == luna::ownership::Usage::Linear;
        return call;
    };

    std::unique_ptr<Expr> itemValue;
    if (plan.mode == IteratorMode::Range) {
        itemValue = identifier(indexLocal);
    } else {
        auto element = std::make_unique<IndexExpr>();
        element->location = statement->location;
        element->object = identifier(sourceLocal);
        element->index = identifier(indexLocal);
        element->type = sourceType->innerTypeId;
        if (plan.mode == IteratorMode::Shared ||
            plan.mode == IteratorMode::Mutable) {
            auto borrowed = std::make_unique<BorrowExpr>();
            borrowed->location = statement->location;
            borrowed->isMutable = plan.mode == IteratorMode::Mutable;
            borrowed->type = sourceItemType;
            borrowed->operand = std::move(element);
            itemValue = std::move(borrowed);
        } else {
            if (guardedConsumingSource) {
                auto transfer = std::make_unique<MoveExpr>();
                transfer->location = statement->location;
                transfer->type = sourceItemType;
                transfer->operand = std::move(element);
                transfer->nextUnread = nextUnreadLocal;
                itemValue = std::move(transfer);
            } else {
                itemValue = std::move(element);
            }
        }
    }
    const bool hasValueAdapter = std::any_of(
        plan.steps.begin(), plan.steps.end(), [](const auto& step) {
            return step.op == IteratorOp::Map ||
                step.op == IteratorOp::Filter;
        });
    LocalId currentItem;
    if (hasValueAdapter) {
        const auto* itemType = mModule->findType(sourceItemType);
        const auto itemUsage = itemType
            ? itemType->sysmeta.resource.usage
            : luna::ownership::Usage::Copy;
        currentItem = addBodyBinding(
            bodyEntry, "$for.item." + identity + ".source",
            sourceItemType, itemUsage,
            std::move(itemValue), LocalKind::Synthetic);
    } else {
        currentItem = addBodyBinding(
            bodyEntry, statement->varName, statement->elementType,
            statement->bindingUsage, std::move(itemValue));
    }

    OpenBlock bodyStart{bodyEntry, {}};
    bool hasBranchingAdapter = false;
    for (size_t stepIndex = 0; stepIndex < plan.steps.size(); ++stepIndex) {
        const auto& step = plan.steps[stepIndex];
        if (step.op == IteratorOp::Map) {
            auto mapped = invokeAdapter(
                adapterLocals[stepIndex], currentItem, step.outputType);
            const auto* mappedType = mModule->findType(step.outputType);
            const auto mappedUsage = mappedType
                ? mappedType->sysmeta.resource.usage
                : luna::ownership::Usage::Copy;
            currentItem = addBodyBinding(
                bodyStart.block,
                "$for.item." + identity + "." +
                    std::to_string(stepIndex),
                step.outputType, mappedUsage, std::move(mapped),
                LocalKind::Synthetic);
            continue;
        }

        hasBranchingAdapter = true;
        const BlockId adapterCondition = addBlock(
            bodyRegion, bodyScope, statement->location);
        const BlockId adapterAccepted = addBlock(
            bodyRegion, bodyScope, statement->location);
        connectJump(bodyStart, adapterCondition);

        Terminator adapterTerminator;
        adapterTerminator.kind = TerminatorKind::Branch;
        adapterTerminator.location = statement->location;
        adapterTerminator.primary.target = adapterAccepted;
        if (step.op == IteratorOp::Filter) {
            adapterTerminator.operand = invokeAdapter(
                adapterLocals[stepIndex], currentItem, boolType);
            adapterTerminator.secondary.target = increment;
        } else {
            auto canTake = std::make_unique<BinaryExpr>();
            canTake->location = statement->location;
            canTake->lhs = identifier(adapterLocals[stepIndex]);
            canTake->op = Operator::Greater;
            canTake->rhs = integer(0, indexType);
            canTake->type = boolType;
            adapterTerminator.operand = std::move(canTake);
            adapterTerminator.secondary.target = exit;
        }
        std::vector<CleanupId> rejectedCleanups;
        if (const auto cleanup = mCleanupByLocal.find(currentItem.value);
            cleanup != mCleanupByLocal.end())
            rejectedCleanups.push_back(cleanup->second);
        if (adapterTerminator.secondary.target == exit)
            rejectedCleanups.insert(
                rejectedCleanups.end(), sourceTailCleanups.begin(),
                sourceTailCleanups.end());
        if (const auto* rejected = mGraph->findBlock(
                adapterTerminator.secondary.target);
            rejected && !rejectedCleanups.empty())
            adapterTerminator.secondary.cleanups =
                canonicalCleanupOrder(
                    rejectedCleanups, bodyScope, rejected->scope);
        mGraph->blocks[adapterCondition.value].terminator =
            std::move(adapterTerminator);

        if (step.op == IteratorOp::Take) {
            auto decrement = std::make_unique<ExprStmt>();
            decrement->location = statement->location;
            auto assignment = std::make_unique<AssignExpr>();
            assignment->location = statement->location;
            assignment->op = Operator::SubtractAssign;
            assignment->lhs = identifier(adapterLocals[stepIndex]);
            assignment->rhs = integer(1, indexType);
            assignment->type = indexType;
            decrement->expr = std::move(assignment);
            mGraph->blocks[adapterAccepted.value].operations.push_back(
                std::move(decrement));
        }
        bodyStart = OpenBlock{adapterAccepted, {}};
    }

    if (hasValueAdapter) {
        std::unique_ptr<Expr> finalItem = identifier(currentItem);
        const auto* itemLocal = mGraph->findLocal(currentItem);
        if (itemLocal && luna::ownership::isMoveOnly(itemLocal->usage)) {
            auto transfer = std::make_unique<MoveExpr>();
            transfer->location = statement->location;
            transfer->type = statement->elementType;
            transfer->operand = std::move(finalItem);
            finalItem = std::move(transfer);
        }
        addBodyBinding(
            bodyStart.block, statement->varName, statement->elementType,
            statement->bindingUsage, std::move(finalItem));
    }

    const size_t activeCleanupDepth = mActiveExpressionCleanups.size();
    mActiveExpressionCleanups.insert(
        mActiveExpressionCleanups.end(), sourceTailCleanups.begin(),
        sourceTailCleanups.end());
    if (guardedConsumingSource && !statement->recipeStateName.empty())
        mGuardedConsumingRecipeNames.insert(statement->recipeStateName);
    if (guardedConsumingSource && !plan.materializedName.empty())
        mGuardedConsumingRecipeNames.insert(plan.materializedName);
    std::optional<OpenBlock> bodyExit;
    if (!statement->body) {
        error(statement->location, "for-loop has no canonical body");
    } else if (!hasBranchingAdapter) {
        bodyExit = lowerSequence(
            statement->body->stmts, bodyStart, bodyRegion, bodyScope);
    } else {
        const BlockId userBody = addBlock(
            bodyRegion, bodyScope, statement->location);
        connectJump(bodyStart, userBody);
        bodyExit = lowerSequence(
            statement->body->stmts, OpenBlock{userBody, {}},
            bodyRegion, bodyScope);
    }
    mActiveExpressionCleanups.resize(activeCleanupDepth);
    if (guardedConsumingSource && !statement->recipeStateName.empty())
        mGuardedConsumingRecipeNames.erase(statement->recipeStateName);
    if (guardedConsumingSource && !plan.materializedName.empty())
        mGuardedConsumingRecipeNames.erase(plan.materializedName);
    popBindings();

    if (bodyExit) {
        if (increment.empty())
            increment = addBlock(
                loopRegion, loopScope, statement->location);
        connectJump(*bodyExit, increment);
    }
    if (!increment.empty()) {
        if (!guardedConsumingSource) {
            auto advance = std::make_unique<ExprStmt>();
            advance->location = statement->location;
            auto assignment = std::make_unique<AssignExpr>();
            assignment->location = statement->location;
            assignment->op = Operator::AddAssign;
            assignment->lhs = identifier(indexLocal);
            assignment->rhs = integer(1, loopIndexType);
            assignment->type = loopIndexType;
            advance->expr = std::move(assignment);
            mGraph->blocks[increment.value].operations.push_back(
                std::move(advance));
        }
        connectJump(OpenBlock{increment, {}}, condition);
        mGraph->regions[bodyRegion.value].exit = increment;
    }
    mGraph->regions[loopRegion.value].exit = exit;
    popBindings();
    return OpenBlock{exit, {}};
}

} // namespace moon
