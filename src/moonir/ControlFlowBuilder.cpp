#include "ControlFlowBuilder.h"

#include <algorithm>
#include <unordered_set>

namespace moon {

std::unique_ptr<ControlFlowGraph> ControlFlowBuilder::build(
    std::unique_ptr<BlockStmt> root,
    const std::vector<Param>& parameters,
    RegionKind rootKind,
    const Module& module) {
    mErrors.clear();
    mBindings.clear();
    mMaterializedIterators.clear();
    mCleanupByLocal.clear();
    mActiveExpressionCleanups.clear();
    mBindingIteratorRecipe = false;
    mTerminalCounter = 0;
    mExpressionCounter = 0;
    mModule = &module;
    if (!root) {
        error({}, "cannot build a canonical CFG from a missing body");
        mModule = nullptr;
        return nullptr;
    }
    if (!module.typeTableSealed) {
        error(root->location,
              "canonical CFG construction requires a sealed type table");
        mModule = nullptr;
        return nullptr;
    }

    auto graph = std::make_unique<ControlFlowGraph>();
    mGraph = graph.get();
    const RegionId rootRegion = addRegion({}, rootKind, root->location);
    const ScopeId rootScope = addScope({}, rootRegion, root->location);
    const BlockId entry = addBlock(rootRegion, rootScope, root->location);
    graph->rootRegion = rootRegion;
    graph->rootScope = rootScope;
    graph->entry = entry;

    pushBindings();
    for (const auto& parameter : parameters)
        addLocal(rootScope, LocalKind::Parameter, parameter.name,
                 parameter.type, parameter.usage, parameter.relation);

    auto open = lowerSequence(
        root->stmts, OpenBlock{entry, {}}, rootRegion, rootScope);
    if (open) {
        auto& terminator = graph->blocks[open->block.value].terminator;
        terminator.kind = TerminatorKind::Return;
        terminator.location = root->location;
        terminator.exitCleanups = canonicalCleanupOrder(
            open->cleanups, rootScope, std::nullopt);
    }
    popBindings();

    canonicalizeCleanupTable();
    graph->sealed = mErrors.empty();
    mGraph = nullptr;
    mModule = nullptr;
    if (!mErrors.empty()) return nullptr;
    return graph;
}

RegionId ControlFlowBuilder::addRegion(
    RegionId parent, RegionKind kind, const SourceLocation& location) {
    const RegionId id{static_cast<uint32_t>(mGraph->regions.size())};
    RegionRecord record;
    record.id = id;
    record.parent = parent;
    record.kind = kind;
    record.location = location;
    mGraph->regions.push_back(std::move(record));
    return id;
}

ScopeId ControlFlowBuilder::addScope(
    ScopeId parent, RegionId region, const SourceLocation& location) {
    const ScopeId id{static_cast<uint32_t>(mGraph->scopes.size())};
    ScopeRecord record;
    record.id = id;
    record.parent = parent;
    record.region = region;
    record.location = location;
    mGraph->scopes.push_back(std::move(record));
    mGraph->regions[region.value].scope = id;
    return id;
}

BlockId ControlFlowBuilder::addBlock(
    RegionId region, ScopeId scope, const SourceLocation& location) {
    const BlockId id{static_cast<uint32_t>(mGraph->blocks.size())};
    BasicBlock block;
    block.id = id;
    block.region = region;
    block.scope = scope;
    block.location = location;
    mGraph->blocks.push_back(std::move(block));
    auto& owner = mGraph->regions[region.value];
    if (owner.entry.empty()) owner.entry = id;
    owner.blocks.push_back(id);
    return id;
}

LocalId ControlFlowBuilder::addLocal(
    ScopeId scope, LocalKind kind, const std::string& name,
    const TypeRef& type, luna::ownership::Usage usage,
    std::optional<luna::ownership::Relation> relation) {
    if (name.empty()) {
        error(mGraph->scopes[scope.value].location,
              "canonical local has no diagnostic name");
        return {};
    }
    if (!mModule->findType(type)) {
        error(mGraph->scopes[scope.value].location,
              "local '" + name + "' references a missing frozen type");
        return {};
    }
    if (mBindings.back().count(name) ||
        (!mMaterializedIterators.empty() &&
         mMaterializedIterators.back().count(name))) {
        error(mGraph->scopes[scope.value].location,
              "duplicate local '" + name + "' in one canonical scope");
        return {};
    }
    const LocalId id{static_cast<uint32_t>(mGraph->locals.size())};
    LocalRecord record;
    record.id = id;
    record.scope = scope;
    record.kind = kind;
    record.name = name;
    record.type = type;
    record.usage = usage;
    if (relation) {
        record.relation = *relation;
    } else if (const auto* frozen = mModule->findType(type)) {
        record.relation = frozen->sysmeta.resource.relation;
    }
    mGraph->locals.push_back(std::move(record));
    mGraph->scopes[scope.value].locals.push_back(id);
    mBindings.back()[name] = id;
    if (const auto* frozen = mModule->findType(type);
        frozen && frozen->sysmeta.resource.cleanupRequired)
        addCleanup(id, type, frozen->sysmeta.resource.cleanup);
    return id;
}

CleanupId ControlFlowBuilder::addCleanup(
    LocalId local, const TypeRef& type,
    luna::ownership::CleanupAction action) {
    if (local.empty() || local.value >= mGraph->locals.size()) {
        error({}, "cleanup references an unresolved canonical local");
        return {};
    }
    if (auto found = mCleanupByLocal.find(local.value);
        found != mCleanupByLocal.end()) {
        const auto& existing = mGraph->cleanups[found->second.value];
        if (existing.type != type || existing.action != action)
            error({}, "local '" + mGraph->locals[local.value].name +
                      "' has inconsistent cleanup obligations");
        return found->second;
    }
    const auto& localRecord = mGraph->locals[local.value];
    const CleanupId id{static_cast<uint32_t>(mGraph->cleanups.size())};
    CleanupRecord cleanup;
    cleanup.id = id;
    cleanup.scope = localRecord.scope;
    cleanup.place.root = local;
    cleanup.type = type;
    cleanup.action = action;
    mGraph->cleanups.push_back(cleanup);
    mGraph->scopes[cleanup.scope.value].cleanups.push_back(id);
    mCleanupByLocal[local.value] = id;
    return id;
}

ControlFlowBuilder::BuiltBlock ControlFlowBuilder::lowerNestedBlock(
    std::unique_ptr<BlockStmt> block, RegionId parentRegion,
    ScopeId parentScope, RegionKind kind) {
    BuiltBlock result;
    if (!block) {
        error({}, "structured control node has no body");
        return result;
    }
    result.region = addRegion(parentRegion, kind, block->location);
    result.scope = addScope(parentScope, result.region, block->location);
    result.entry = addBlock(result.region, result.scope, block->location);
    pushBindings();
    result.exit = lowerSequence(
        block->stmts, OpenBlock{result.entry, {}},
        result.region, result.scope);
    popBindings();
    return result;
}

std::optional<ControlFlowBuilder::OpenBlock>
ControlFlowBuilder::lowerSequence(
    std::vector<std::unique_ptr<Stmt>>& statements,
    OpenBlock current, RegionId region, ScopeId scope) {
    std::optional<OpenBlock> open = std::move(current);
    for (auto& statement : statements) {
        if (!statement) {
            error({}, "structured body contains a null statement");
            continue;
        }
        if (!open) {
            error(statement->location,
                  "structured body contains a statement after a terminating path");
            break;
        }
        open = lowerStatement(std::move(statement), std::move(*open),
                              region, scope);
    }
    return open;
}

std::optional<ControlFlowBuilder::OpenBlock>
ControlFlowBuilder::lowerStatement(
    std::unique_ptr<Stmt> statement, OpenBlock current,
    RegionId region, ScopeId scope) {
    if (!current.cleanups.empty() &&
        !(dynamic_cast<FreeStmt*>(statement.get()) &&
          static_cast<FreeStmt*>(statement.get())->isImplicit)) {
        error(statement->location,
              "implicit lexical cleanup is not the final operation on its path");
        return std::nullopt;
    }
    if (auto* declaration = dynamic_cast<LetStmt*>(statement.get())) {
        if (declaration->materializesIteratorRecipe) {
            std::unique_ptr<LetStmt> owned(
                static_cast<LetStmt*>(statement.release()));
            if (!materializeIteratorRecipe(
                    std::move(owned), current, scope))
                return std::nullopt;
            return current;
        }
        auto normalized = normalizeControlFlowExpression(
            declaration->initializer, current, region, scope, false);
        if (!normalized) return std::nullopt;
        current = std::move(*normalized);
        if (!bindExpr(declaration->initializer.get())) return std::nullopt;
        declaration->local = addLocal(
            scope, LocalKind::Binding, declaration->name,
            declaration->type, declaration->usage);
        mGraph->blocks[current.block.value].operations.push_back(
            std::move(statement));
        return current;
    }
    if (auto* expression = dynamic_cast<ExprStmt*>(statement.get())) {
        auto normalized = normalizeControlFlowExpression(
            expression->expr, current, region, scope, true);
        if (!normalized) return std::nullopt;
        current = std::move(*normalized);
        if (!expression->expr) return current;
        if (!bindExpr(expression->expr.get())) return std::nullopt;
        mGraph->blocks[current.block.value].operations.push_back(
            std::move(statement));
        return current;
    }
    if (auto* release = dynamic_cast<FreeStmt*>(statement.get())) {
        if (!bindExpr(release->operand.get())) return std::nullopt;
        if (!release->isImplicit) {
            mGraph->blocks[current.block.value].operations.push_back(
                std::move(statement));
            return current;
        }
        auto* identifier = dynamic_cast<IdentifierExpr*>(
            release->operand.get());
        if (!identifier || identifier->local.empty()) {
            error(release->location,
                  "implicit cleanup does not reference a canonical local");
            return std::nullopt;
        }
        const auto& local = mGraph->locals[identifier->local.value];
        const CleanupId cleanup = addCleanup(
            identifier->local, local.type, release->action);
        if (!cleanup.empty()) current.cleanups.push_back(cleanup);
        return current;
    }
    if (auto* await = dynamic_cast<AwaitStmt*>(statement.get())) {
        if (!bindExpr(await->event.get())) return std::nullopt;
        mGraph->blocks[current.block.value].operations.push_back(
            std::move(statement));
        return current;
    }
    if (auto* returned = dynamic_cast<ReturnStmt*>(statement.get())) {
        auto normalized = normalizeControlFlowExpression(
            returned->value, current, region, scope, false);
        if (!normalized) return std::nullopt;
        current = std::move(*normalized);
        if (!bindExpr(returned->value.get())) return std::nullopt;
        auto cleanups = lowerCleanupObligations(returned->cleanups, scope);
        cleanups.insert(cleanups.end(), current.cleanups.begin(),
                        current.cleanups.end());
        cleanups.insert(cleanups.end(),
                        mActiveExpressionCleanups.begin(),
                        mActiveExpressionCleanups.end());
        auto& terminator = mGraph->blocks[current.block.value].terminator;
        terminator.kind = TerminatorKind::Return;
        terminator.location = returned->location;
        terminator.operand = std::move(returned->value);
        terminator.exitCleanups = canonicalCleanupOrder(
            cleanups, scope, std::nullopt);
        return std::nullopt;
    }
    if (dynamic_cast<IfStmt*>(statement.get())) {
        std::unique_ptr<IfStmt> owned(
            static_cast<IfStmt*>(statement.release()));
        return lowerIf(std::move(owned), std::move(current), region, scope);
    }
    if (dynamic_cast<WhileStmt*>(statement.get())) {
        std::unique_ptr<WhileStmt> owned(
            static_cast<WhileStmt*>(statement.release()));
        return lowerWhile(std::move(owned), std::move(current), region, scope);
    }
    if (dynamic_cast<ForStmt*>(statement.get())) {
        std::unique_ptr<ForStmt> owned(
            static_cast<ForStmt*>(statement.release()));
        return lowerFor(std::move(owned), std::move(current), region, scope);
    }
    if (dynamic_cast<MatchStmt*>(statement.get())) {
        std::unique_ptr<MatchStmt> owned(
            static_cast<MatchStmt*>(statement.release()));
        return lowerMatch(std::move(owned), std::move(current), region, scope);
    }
    if (dynamic_cast<BlockStmt*>(statement.get())) {
        std::unique_ptr<BlockStmt> owned(
            static_cast<BlockStmt*>(statement.release()));
        auto body = lowerNestedBlock(
            std::move(owned), region, scope, RegionKind::Lexical);
        connectJump(current, body.entry);
        if (!body.exit) return std::nullopt;
        const BlockId continuation = addBlock(
            region, scope, body.exit
                ? mGraph->blocks[body.exit->block.value].location
                : SourceLocation{});
        connectJump(*body.exit, continuation);
        mGraph->regions[body.region.value].exit = continuation;
        return OpenBlock{continuation, {}};
    }

    error(statement->location,
          "control-flow builder does not yet support this structured statement");
    return std::nullopt;
}

std::optional<ControlFlowBuilder::OpenBlock> ControlFlowBuilder::lowerIf(
    std::unique_ptr<IfStmt> statement, OpenBlock current,
    RegionId region, ScopeId scope) {
    auto condition = normalizeControlFlowExpression(
        statement->cond, std::move(current), region, scope, false);
    if (!condition) return std::nullopt;
    current = std::move(*condition);
    if (!bindExpr(statement->cond.get())) return std::nullopt;
    auto thenBody = lowerNestedBlock(
        std::move(statement->thenBlock), region, scope,
        RegionKind::Lexical);

    const bool hasElse = statement->elseBranch != nullptr;
    std::optional<BuiltBlock> elseBody;
    std::optional<OpenBlock> elseExit;
    BlockId elseEntry;
    if (statement->elseBranch) {
        if (dynamic_cast<BlockStmt*>(statement->elseBranch.get())) {
            std::unique_ptr<BlockStmt> owned(
                static_cast<BlockStmt*>(statement->elseBranch.release()));
            elseBody = lowerNestedBlock(
                std::move(owned), region, scope, RegionKind::Lexical);
            elseEntry = elseBody->entry;
            elseExit = std::move(elseBody->exit);
        } else {
            elseEntry = addBlock(
                region, scope, statement->elseBranch->location);
            elseExit = lowerStatement(
                std::move(statement->elseBranch),
                OpenBlock{elseEntry, {}}, region, scope);
        }
    }

    const bool needsMerge = !hasElse ||
        thenBody.exit.has_value() || elseExit.has_value();
    BlockId falseTarget;
    if (!needsMerge) {
        falseTarget = elseEntry;
    } else {
        const BlockId merge = addBlock(region, scope, statement->location);
        if (thenBody.exit) connectJump(*thenBody.exit, merge);
        mGraph->regions[thenBody.region.value].exit = merge;
        if (hasElse) {
            falseTarget = elseEntry;
            if (elseExit) connectJump(*elseExit, merge);
            if (elseBody)
                mGraph->regions[elseBody->region.value].exit = merge;
        } else {
            falseTarget = merge;
        }
        auto& terminator = mGraph->blocks[current.block.value].terminator;
        terminator.kind = TerminatorKind::Branch;
        terminator.location = statement->location;
        terminator.operand = std::move(statement->cond);
        terminator.primary.target = thenBody.entry;
        terminator.secondary.target = falseTarget;
        return OpenBlock{merge, {}};
    }
    auto& terminator = mGraph->blocks[current.block.value].terminator;
    terminator.kind = TerminatorKind::Branch;
    terminator.location = statement->location;
    terminator.operand = std::move(statement->cond);
    terminator.primary.target = thenBody.entry;
    terminator.secondary.target = falseTarget;
    return std::nullopt;
}

std::optional<ControlFlowBuilder::OpenBlock> ControlFlowBuilder::lowerWhile(
    std::unique_ptr<WhileStmt> statement, OpenBlock current,
    RegionId region, ScopeId scope) {
    const RegionId loopRegion = addRegion(
        region, RegionKind::Loop, statement->location);
    const ScopeId loopScope = addScope(
        scope, loopRegion, statement->location);
    pushBindings();

    BlockId conditionEntry;
    OpenBlock condition;
    const bool expandsCondition = containsPendingControlFlow(
        statement->cond.get());
    if (expandsCondition) {
        // The loop header is deliberately outside the evaluation scope. A
        // backedge first leaves the prior condition/body state, then enters a
        // fresh execution of the condition and reactivates its synthetic
        // locals. No runtime initialized bit is needed.
        const BlockId header = addBlock(
            loopRegion, loopScope, statement->location);
        connectJump(current, header);
        const RegionId evaluationRegion = addRegion(
            loopRegion, RegionKind::Lexical, statement->location);
        const ScopeId evaluationScope = addScope(
            loopScope, evaluationRegion, statement->location);
        conditionEntry = addBlock(
            evaluationRegion, evaluationScope, statement->location);
        connectJump(OpenBlock{header, {}}, conditionEntry);

        pushBindings();
        auto normalized = normalizeControlFlowExpression(
            statement->cond, OpenBlock{conditionEntry, {}},
            evaluationRegion, evaluationScope, false);
        if (!normalized) {
            popBindings();
            popBindings();
            return std::nullopt;
        }
        condition = std::move(*normalized);
        if (!bindExpr(statement->cond.get())) {
            popBindings();
            popBindings();
            return std::nullopt;
        }
        popBindings();
    } else {
        conditionEntry = addBlock(
            loopRegion, loopScope, statement->location);
        connectJump(current, conditionEntry);
        condition = OpenBlock{conditionEntry, {}};
        if (!bindExpr(statement->cond.get())) {
            popBindings();
            return std::nullopt;
        }
    }

    auto body = lowerNestedBlock(
        std::move(statement->body), loopRegion, loopScope,
        RegionKind::Lexical);
    popBindings();

    const BlockId exit = addBlock(region, scope, statement->location);
    auto& terminator = mGraph->blocks[condition.block.value].terminator;
    terminator.kind = TerminatorKind::Branch;
    terminator.location = statement->location;
    terminator.operand = std::move(statement->cond);
    terminator.primary.target = body.entry;
    terminator.secondary.target = exit;
    if (expandsCondition) {
        terminator.primary.cleanups = canonicalCleanupOrder(
            condition.cleanups,
            mGraph->blocks[condition.block.value].scope,
            mGraph->blocks[body.entry.value].scope);
        terminator.secondary.cleanups = canonicalCleanupOrder(
            condition.cleanups,
            mGraph->blocks[condition.block.value].scope,
            mGraph->blocks[exit.value].scope);
    }
    const BlockId backedge = expandsCondition
        ? mGraph->regions[loopRegion.value].entry
        : conditionEntry;
    if (body.exit) connectJump(*body.exit, backedge);
    mGraph->regions[body.region.value].exit = backedge;
    mGraph->regions[loopRegion.value].exit = exit;
    return OpenBlock{exit, {}};
}

std::optional<ControlFlowBuilder::OpenBlock> ControlFlowBuilder::lowerFor(
    std::unique_ptr<ForStmt> statement, OpenBlock current,
    RegionId region, ScopeId scope) {
    if (statement->protocolNext.empty())
        return lowerIteratorRecipeFor(
            std::move(statement), std::move(current), region, scope);

    const bool previousRecipeAllowance = mBindingIteratorRecipe;
    mBindingIteratorRecipe = false;
    const bool bound = bindExpr(statement->iterable.get());
    mBindingIteratorRecipe = previousRecipeAllowance;
    if (!bound) return std::nullopt;

    const auto* nextDeclaration = mModule->findDeclaration(
        statement->protocolNext);
    const auto* nextType = nextDeclaration
        ? mModule->findType(nextDeclaration->type) : nullptr;
    const auto* optionType = mModule->findType(
        statement->protocolOptionType);
    const auto* iteratorType = mModule->findType(
        statement->protocolIteratorType);
    if (!nextDeclaration || nextDeclaration->kind != DeclarationKind::Function ||
        !nextType || nextType->kind != TypeKind::Function ||
        nextType->parameterTypeIds.size() != 1 ||
        nextType->returnTypeId != statement->protocolOptionType) {
        error(statement->location,
              "for-loop Iterator::next witness has no canonical unary function contract");
        return std::nullopt;
    }
    const auto* receiverType = mModule->findType(
        nextType->parameterTypeIds.front());
    if (!receiverType || receiverType->kind != TypeKind::Reference ||
        !receiverType->isMutable ||
        receiverType->innerTypeId != statement->protocolIteratorType ||
        !iteratorType) {
        error(statement->location,
              "for-loop Iterator::next witness does not take &mut iterator state");
        return std::nullopt;
    }
    if (!optionType || optionType->kind != TypeKind::Enum ||
        optionType->variants.size() != 2 ||
        statement->protocolNoneVariant >= optionType->variants.size() ||
        statement->protocolSomeVariant >= optionType->variants.size() ||
        statement->protocolNoneVariant == statement->protocolSomeVariant ||
        !optionType->variants[statement->protocolNoneVariant].fields.empty() ||
        optionType->variants[statement->protocolSomeVariant].fields !=
            TypeRefVec{statement->elementType}) {
        error(statement->location,
              "for-loop protocol Option witness has no canonical None/Some<T> shape");
        return std::nullopt;
    }

    const RegionId loopRegion = addRegion(
        region, RegionKind::Loop, statement->location);
    const ScopeId loopScope = addScope(
        scope, loopRegion, statement->location);
    pushBindings();

    BlockId start;
    LocalId stateLocal;
    std::string stateName;
    if (!statement->protocolInto.empty()) {
        const auto* intoDeclaration = mModule->findDeclaration(
            statement->protocolInto);
        const auto* intoType = intoDeclaration
            ? mModule->findType(intoDeclaration->type) : nullptr;
        if (!intoDeclaration ||
            intoDeclaration->kind != DeclarationKind::Function ||
            !intoType || intoType->kind != TypeKind::Function ||
            intoType->parameterTypeIds !=
                TypeRefVec{statement->protocolInputType} ||
            intoType->returnTypeId != statement->protocolIteratorType ||
            statement->protocolStateName.empty() ||
            !statement->iterable ||
            statement->iterable->type != statement->protocolInputType) {
            error(statement->location,
                  "for-loop IntoIterator witness has no canonical state conversion contract");
            popBindings();
            return std::nullopt;
        }
        const bool cleanupRequired =
            iteratorType->sysmeta.resource.cleanupRequired;
        if (statement->protocolStateNeedsCleanup != cleanupRequired ||
            (cleanupRequired && statement->protocolStateCleanup !=
                iteratorType->sysmeta.resource.cleanup)) {
            error(statement->location,
                  "for-loop hidden iterator state disagrees with frozen cleanup facts");
            popBindings();
            return std::nullopt;
        }

        start = addBlock(loopRegion, loopScope, statement->location);
        stateName = statement->protocolStateName;
        const auto stateUsage = iteratorType->sysmeta.resource.usage;
        stateLocal = addLocal(
            loopScope, LocalKind::Binding, stateName,
            statement->protocolIteratorType, stateUsage);

        auto conversion = std::make_unique<CallExpr>();
        conversion->location = statement->location;
        conversion->calleeRef = statement->protocolInto;
        conversion->type = statement->protocolIteratorType;
        conversion->returnUsage = intoType->returnContract.usage;
        conversion->returnsLinear =
            conversion->returnUsage == luna::ownership::Usage::Linear;
        auto conversionCallee = std::make_unique<IdentifierExpr>();
        conversionCallee->location = statement->location;
        conversionCallee->name = intoDeclaration->sourceName;
        conversionCallee->declaration = statement->protocolInto;
        conversionCallee->type = intoDeclaration->type;
        conversion->callee = std::move(conversionCallee);
        if (dynamic_cast<MoveExpr*>(statement->iterable.get())) {
            conversion->args.push_back(std::move(statement->iterable));
        } else {
            auto moved = std::make_unique<MoveExpr>();
            moved->location = statement->location;
            moved->type = statement->protocolInputType;
            moved->operand = std::move(statement->iterable);
            conversion->args.push_back(std::move(moved));
        }

        auto state = std::make_unique<LetStmt>();
        state->location = statement->location;
        state->name = stateName;
        state->local = stateLocal;
        state->isLinear = stateUsage == luna::ownership::Usage::Linear;
        state->usage = stateUsage;
        state->type = statement->protocolIteratorType;
        state->initializer = std::move(conversion);
        mGraph->blocks[start.value].operations.push_back(std::move(state));
    } else {
        auto* state = dynamic_cast<IdentifierExpr*>(statement->iterable.get());
        if (!state || state->local.empty() ||
            state->type != statement->protocolIteratorType ||
            !statement->protocolStateName.empty() ||
            statement->protocolStateNeedsCleanup) {
            error(statement->location,
                  "direct Iterator for-loop requires one borrowed canonical local state");
            popBindings();
            return std::nullopt;
        }
        stateLocal = state->local;
        stateName = state->name;
    }

    const BlockId condition = addBlock(
        loopRegion, loopScope, statement->location);
    if (!start.empty()) {
        connectJump(OpenBlock{start, {}}, condition);
        connectJump(current, start);
    } else {
        connectJump(current, condition);
    }

    const RegionId bodyRegion = addRegion(
        loopRegion, RegionKind::MatchArm, statement->location);
    const ScopeId bodyScope = addScope(
        loopScope, bodyRegion, statement->location);
    const BlockId bodyEntry = addBlock(
        bodyRegion, bodyScope, statement->location);
    pushBindings();
    const LocalId itemLocal = addLocal(
        bodyScope, LocalKind::Pattern, statement->varName,
        statement->elementType, statement->bindingUsage);
    auto bodyExit = statement->body
        ? lowerSequence(statement->body->stmts, OpenBlock{bodyEntry, {}},
                        bodyRegion, bodyScope)
        : std::optional<OpenBlock>{};
    if (!statement->body)
        error(statement->location, "for-loop has no canonical body");
    popBindings();

    const BlockId exit = addBlock(region, scope, statement->location);
    const BlockId invalid = addBlock(
        loopRegion, loopScope, statement->location);
    auto& invalidTerminator = mGraph->blocks[invalid.value].terminator;
    invalidTerminator.kind = TerminatorKind::Unreachable;
    invalidTerminator.location = statement->location;

    auto state = std::make_unique<IdentifierExpr>();
    state->location = statement->location;
    state->name = stateName;
    state->local = stateLocal;
    state->type = statement->protocolIteratorType;
    auto receiver = std::make_unique<BorrowExpr>();
    receiver->location = statement->location;
    receiver->isMutable = true;
    receiver->type = nextType->parameterTypeIds.front();
    receiver->operand = std::move(state);
    auto next = std::make_unique<CallExpr>();
    next->location = statement->location;
    next->calleeRef = statement->protocolNext;
    next->type = statement->protocolOptionType;
    next->returnUsage = nextType->returnContract.usage;
    next->returnsLinear =
        next->returnUsage == luna::ownership::Usage::Linear;
    auto nextCallee = std::make_unique<IdentifierExpr>();
    nextCallee->location = statement->location;
    nextCallee->name = nextDeclaration->sourceName;
    nextCallee->declaration = statement->protocolNext;
    nextCallee->type = nextDeclaration->type;
    next->callee = std::move(nextCallee);
    next->args.push_back(std::move(receiver));

    Terminator terminator;
    terminator.kind = TerminatorKind::Switch;
    terminator.location = statement->location;
    terminator.operand = std::move(next);
    terminator.switchType = statement->protocolOptionType;
    terminator.primary.target = invalid;
    SwitchEdge none;
    none.tag = statement->protocolNoneVariant;
    none.edge.target = exit;
    std::vector<CleanupId> loopCleanups;
    if (auto cleanup = mCleanupByLocal.find(stateLocal.value);
        cleanup != mCleanupByLocal.end() &&
        mGraph->locals[stateLocal.value].scope == loopScope)
        loopCleanups.push_back(cleanup->second);
    none.edge.cleanups = canonicalCleanupOrder(
        loopCleanups, loopScope, scope);
    SwitchEdge some;
    some.tag = statement->protocolSomeVariant;
    some.edge.target = bodyEntry;
    some.bindings.push_back(itemLocal);
    terminator.cases.push_back(std::move(none));
    terminator.cases.push_back(std::move(some));
    std::sort(terminator.cases.begin(), terminator.cases.end(),
              [](const SwitchEdge& lhs, const SwitchEdge& rhs) {
        return lhs.tag < rhs.tag;
    });
    mGraph->blocks[condition.value].terminator = std::move(terminator);

    if (bodyExit) connectJump(*bodyExit, condition);
    mGraph->regions[bodyRegion.value].exit = condition;
    mGraph->regions[loopRegion.value].exit = exit;
    popBindings();
    return OpenBlock{exit, {}};
}

bool ControlFlowBuilder::parseIteratorRecipe(
    std::unique_ptr<Expr> expression, IteratorRecipePlan& plan,
    const SourceLocation& location) {
    if (!expression) {
        error(location, "iterator recipe has no canonical expression");
        return false;
    }
    auto* call = dynamic_cast<CallExpr*>(expression.get());
    if (!call) {
        if (const auto* identifier =
                dynamic_cast<const IdentifierExpr*>(expression.get())) {
            if (const auto* materialized =
                    lookupMaterializedIterator(identifier->name)) {
                plan.mode = materialized->mode;
                plan.sourceType = materialized->sourceType;
                plan.itemType = materialized->itemType;
                plan.materialized = true;
                plan.materializedSource = materialized->source;
                plan.materializedIndex = materialized->index;
                plan.materializedLimit = materialized->limit;
                for (const auto& step : materialized->steps) {
                    IteratorRecipeStep copied;
                    copied.op = step.op;
                    copied.argumentLocal = step.argument;
                    copied.inputType = step.inputType;
                    copied.outputType = step.outputType;
                    plan.steps.push_back(std::move(copied));
                }
                return true;
            }
        }
        const auto* sourceType = mModule->findType(expression->type);
        if (!sourceType ||
            (sourceType->kind != TypeKind::Array &&
             sourceType->kind != TypeKind::Slice)) {
            error(location,
                  "materialized iterator recipes require the later canonical subphase");
            return false;
        }
        plan.sourceType = expression->type;
        plan.source = std::move(expression);
        plan.mode = sourceType->kind == TypeKind::Slice
            ? IteratorMode::Shared : IteratorMode::Consuming;
        plan.itemType = sourceType->kind == TypeKind::Slice
            ? TypeRef{} : sourceType->innerTypeId;
        return true;
    }

    if (call->iteratorOp == IteratorOp::Range) {
        if (call->args.size() != 2 ||
            call->iteratorInputType.empty() ||
            call->iteratorOutputType.empty()) {
            error(location, "range recipe has no canonical start/end contract");
            return false;
        }
        plan.mode = IteratorMode::Range;
        plan.rangeStart = std::move(call->args[0]);
        plan.rangeEnd = std::move(call->args[1]);
        plan.itemType = call->iteratorOutputType;
        return true;
    }

    auto* member = dynamic_cast<FieldAccessExpr*>(call->callee.get());
    if (!member) {
        error(location, "iterator recipe call has no canonical receiver");
        return false;
    }
    if (call->iteratorOp == IteratorOp::Iter ||
        call->iteratorOp == IteratorOp::IterMut ||
        call->iteratorOp == IteratorOp::IntoIter) {
        if (!call->args.empty() || !member->object) {
            error(location, "iterator source adapter has an invalid canonical shape");
            return false;
        }
        plan.sourceType = member->object->type;
        plan.source = std::move(member->object);
        plan.mode = call->iteratorOp == IteratorOp::Iter
            ? IteratorMode::Shared
            : (call->iteratorOp == IteratorOp::IterMut
                ? IteratorMode::Mutable : IteratorMode::Consuming);
        plan.itemType = call->iteratorOutputType;
        return true;
    }
    if (call->iteratorOp == IteratorOp::Take) {
        if (call->args.size() != 1 || !member->object) {
            error(location, "iterator take adapter has an invalid canonical shape");
            return false;
        }
        auto receiver = std::move(member->object);
        auto argument = std::move(call->args.front());
        const TypeRef inputType = call->iteratorInputType;
        const TypeRef outputType = call->iteratorOutputType;
        if (!parseIteratorRecipe(std::move(receiver), plan, location))
            return false;
        if (inputType != plan.itemType || outputType != inputType) {
            error(location,
                  "iterator take adapter disagrees with its canonical item type");
            return false;
        }
        plan.steps.push_back({
            IteratorOp::Take, std::move(argument), {},
            inputType, outputType});
        plan.itemType = outputType;
        return true;
    }
    if (call->iteratorOp == IteratorOp::Map ||
        call->iteratorOp == IteratorOp::Filter) {
        if (call->args.size() != 1 || !member->object) {
            error(location,
                  "iterator map/filter adapter has an invalid canonical shape");
            return false;
        }
        auto receiver = std::move(member->object);
        auto callable = std::move(call->args.front());
        const TypeRef inputType = call->iteratorInputType;
        const TypeRef outputType = call->iteratorOutputType;
        if (!parseIteratorRecipe(std::move(receiver), plan, location))
            return false;
        if (inputType != plan.itemType || outputType.empty()) {
            error(location,
                  "iterator map/filter adapter disagrees with its canonical item type");
            return false;
        }
        const auto* closure = mModule->findType(callable->type);
        TypeRef expectedResult = outputType;
        if (call->iteratorOp == IteratorOp::Filter)
            for (const auto& type : mModule->typeTable)
                if (type.kind == TypeKind::Bool) expectedResult = type.id;
        if (!closure || closure->kind != TypeKind::Function ||
            closure->parameterTypeIds != TypeRefVec{inputType} ||
            closure->returnTypeId != expectedResult ||
            (call->iteratorOp == IteratorOp::Filter &&
             outputType != inputType)) {
            error(location,
                  "iterator map/filter requires one canonical capture-free callable");
            return false;
        }
        plan.steps.push_back({call->iteratorOp, std::move(callable), {},
                              inputType, outputType});
        plan.itemType = outputType;
        return true;
    }
    error(location, "unsupported compiler iterator recipe operation");
    return false;
}

bool ControlFlowBuilder::bindIteratorRecipe(IteratorRecipePlan& plan) {
    const bool previousRecipeAllowance = mBindingIteratorRecipe;
    mBindingIteratorRecipe = true;
    bool valid = true;
    if (!plan.materialized) {
        if (plan.mode == IteratorMode::Range) {
            valid = bindExpr(plan.rangeStart.get()) &&
                bindExpr(plan.rangeEnd.get());
        } else {
            valid = bindExpr(plan.source.get());
        }
    }
    for (auto& step : plan.steps)
        if (step.argument && !bindExpr(step.argument.get())) valid = false;
    mBindingIteratorRecipe = previousRecipeAllowance;
    return valid;
}

bool ControlFlowBuilder::validateIteratorRecipe(
    IteratorRecipePlan& plan, const TypeRef& expectedItem,
    const SourceLocation& location) {
    TypeRef indexType;
    TypeRef sizeType;
    for (const auto& type : mModule->typeTable) {
        if (type.kind == TypeKind::I32) indexType = type.id;
        if (type.kind == TypeKind::USize) sizeType = type.id;
    }
    if (indexType.empty()) {
        error(location, "iterator recipe requires a frozen i32 compiler type");
        return false;
    }
    if (plan.mode == IteratorMode::Range) {
        if (plan.materialized) {
            const auto* initial = mGraph->findLocal(plan.materializedIndex);
            const auto* limit = mGraph->findLocal(plan.materializedLimit);
            if (!initial || !limit || initial->type != indexType ||
                limit->type != indexType) {
                error(location,
                      "materialized range has no canonical i32 cursor state");
                return false;
            }
        } else if (!plan.rangeStart || !plan.rangeEnd ||
                   plan.rangeStart->type != indexType ||
                   plan.rangeEnd->type != indexType) {
            error(location,
                  "range recipe must be normalized to i32 start/end/item values");
            return false;
        }
    } else {
        const auto* sourceType = mModule->findType(plan.sourceType);
        if (!sourceType ||
            (sourceType->kind != TypeKind::Array &&
             sourceType->kind != TypeKind::Slice) ||
            sourceType->innerTypeId.empty()) {
            error(location,
                  "iterator recipe source is not a frozen array or slice");
            return false;
        }
        if (sourceType->kind == TypeKind::Slice &&
            plan.mode != IteratorMode::Shared) {
            error(location, "read-only slice recipes require shared iteration");
            return false;
        }
        if (sourceType->kind == TypeKind::Slice && sizeType.empty()) {
            error(location, "slice recipe requires a frozen usize compiler type");
            return false;
        }
        const auto* elementType = mModule->findType(sourceType->innerTypeId);
        if (sourceType->kind == TypeKind::Array &&
            plan.mode == IteratorMode::Consuming &&
            (!elementType || elementType->sysmeta.resource.usage !=
                luna::ownership::Usage::Copy)) {
            error(location,
                  "move-only consuming arrays require projected canonical cleanup state");
            return false;
        }
        if (sourceType->kind == TypeKind::Slice && plan.itemType.empty())
            plan.itemType = expectedItem;
        if (plan.materialized) {
            const auto* source = mGraph->findLocal(plan.materializedSource);
            if (!source || source->type != plan.sourceType) {
                error(location,
                      "materialized iterator has no canonical source local");
                return false;
            }
        }
    }

    const TypeRef sourceItemType = plan.steps.empty()
        ? plan.itemType : plan.steps.front().inputType;
    if (plan.mode == IteratorMode::Range) {
        if (sourceItemType != indexType) {
            error(location, "range recipe adapter input is not canonical i32");
            return false;
        }
    } else {
        const auto* frozenSource = mModule->findType(plan.sourceType);
        const auto* frozenItem = mModule->findType(sourceItemType);
        const bool consumingItem = frozenSource &&
            sourceItemType == frozenSource->innerTypeId;
        const bool borrowedItem = frozenSource && frozenItem &&
            frozenItem->kind == TypeKind::Reference &&
            frozenItem->innerTypeId == frozenSource->innerTypeId &&
            frozenItem->isMutable == (plan.mode == IteratorMode::Mutable);
        if ((plan.mode == IteratorMode::Consuming && !consumingItem) ||
            ((plan.mode == IteratorMode::Shared ||
              plan.mode == IteratorMode::Mutable) && !borrowedItem)) {
            error(location,
                  "iterator recipe source mode disagrees with its first item type");
            return false;
        }
    }
    if (plan.itemType != expectedItem) {
        error(location,
              "iterator recipe item type disagrees with its consumer binding");
        return false;
    }

    for (const auto& step : plan.steps) {
        const auto* argumentLocal = mGraph->findLocal(step.argumentLocal);
        const TypeRef argumentType = step.argument
            ? step.argument->type
            : (argumentLocal ? argumentLocal->type : TypeRef{});
        if ((step.argument != nullptr) == !step.argumentLocal.empty()) {
            error(location,
                  "iterator adapter has ambiguous materialized argument state");
            return false;
        }
        if (step.op == IteratorOp::Take) {
            if (argumentType != indexType) {
                error(location,
                      "iterator take count must be normalized to i32");
                return false;
            }
            continue;
        }
        if (step.op != IteratorOp::Map && step.op != IteratorOp::Filter) {
            error(location,
                  "iterator recipe contains an unsupported canonical adapter");
            return false;
        }
        const auto* input = mModule->findType(step.inputType);
        const auto* output = mModule->findType(step.outputType);
        const auto* callable = mModule->findType(argumentType);
        const auto* lambda = step.argument
            ? dynamic_cast<const LambdaExpr*>(step.argument.get()) : nullptr;
        if (!input || !output || !callable ||
            callable->kind != TypeKind::Function ||
            (lambda &&
             (lambda->body || !lambda->controlFlow ||
              !lambda->captures.empty() ||
              argumentType != lambda->closureType)) ||
            callable->parameterContracts.size() != 1 ||
            callable->parameterContracts.front().usage !=
                luna::ownership::Usage::Copy ||
            callable->returnContract.usage != luna::ownership::Usage::Copy ||
            input->sysmeta.resource.usage != luna::ownership::Usage::Copy ||
            output->sysmeta.resource.usage != luna::ownership::Usage::Copy) {
            error(location,
                  "non-Copy map/filter item or callable contract requires "
                  "canonical per-item ownership state");
            return false;
        }
    }
    return true;
}

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
    if (declaration->materializedIteratorOwnsSource ||
        !declaration->materializedIteratorSourceType.empty()) {
        error(declaration->location,
              "move-only materialized recipe requires projected source cleanup state");
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
            plan, iteratorType->innerTypeId, declaration->location))
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
        LocalKind kind = LocalKind::Binding) {
        const LocalId local = addLocal(
            scope, kind, name, type, usage);
        auto state = std::make_unique<LetStmt>();
        state->location = declaration->location;
        state->name = name;
        state->local = local;
        state->isLinear = usage == luna::ownership::Usage::Linear;
        state->usage = usage;
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
            materialized.source = addStateBinding(
                "$recipe." + declaration->name + ".source",
                plan.sourceType, luna::ownership::Usage::Copy,
                std::move(plan.source));
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
    if (!statement->recipeStateName.empty()) {
        error(statement->location,
              "move-only iterator recipe state requires projected array cleanup");
        return std::nullopt;
    }

    if (!bindIteratorRecipe(plan) ||
        !validateIteratorRecipe(
            plan, statement->elementType, statement->location))
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
                                std::unique_ptr<Expr> initializer) {
        const auto* frozen = mModule->findType(type);
        const auto usage = frozen
            ? frozen->sysmeta.resource.usage
            : luna::ownership::Usage::Copy;
        const LocalId local = addLocal(
            loopScope, LocalKind::Binding, name, type, usage);
        auto declaration = std::make_unique<LetStmt>();
        declaration->location = statement->location;
        declaration->name = name;
        declaration->local = local;
        declaration->isLinear = usage == luna::ownership::Usage::Linear;
        declaration->usage = usage;
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
    const TypeRecord* sourceType = nullptr;
    if (plan.mode != IteratorMode::Range) {
        sourceType = mModule->findType(plan.sourceType);
        if (plan.materialized) {
            sourceLocal = plan.materializedSource;
        } else {
            auto* sourceIdentifier = dynamic_cast<IdentifierExpr*>(
                plan.source.get());
            const bool snapshot = plan.mode == IteratorMode::Consuming;
            if (!snapshot && sourceIdentifier &&
                !sourceIdentifier->local.empty()) {
                sourceLocal = sourceIdentifier->local;
                plan.source.reset();
            } else {
                const auto* frozen = mModule->findType(plan.sourceType);
                if (!frozen || frozen->sysmeta.resource.cleanupRequired) {
                    error(statement->location,
                          "temporary iterator source requires unsupported synthetic cleanup");
                    popBindings();
                    return std::nullopt;
                }
                sourceLocal = addBinding(
                    "$for.source." + identity, plan.sourceType,
                    std::move(plan.source));
            }
        }
    }

    std::unique_ptr<Expr> initial;
    std::unique_ptr<Expr> limit;
    const TypeRef loopIndexType = sourceType &&
            sourceType->kind == TypeKind::Slice
        ? sizeType : indexType;
    if (plan.materialized) {
        auto transfer = std::make_unique<MoveExpr>();
        transfer->location = statement->location;
        transfer->operand = identifier(plan.materializedIndex);
        transfer->type = loopIndexType;
        initial = std::move(transfer);
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
        "$for.index." + identity, loopIndexType, std::move(initial));
    const LocalId limitLocal = addBinding(
        "$for.limit." + identity, loopIndexType, std::move(limit));
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
    mGraph->blocks[condition.value].terminator =
        std::move(conditionTerminator);

    pushBindings();
    const auto addBodyBinding = [&](BlockId block, const std::string& name,
                                    const TypeRef& type,
                                    luna::ownership::Usage usage,
                                    std::unique_ptr<Expr> initializer) {
        const LocalId local = addLocal(
            bodyScope, LocalKind::Binding, name, type, usage);
        auto declaration = std::make_unique<LetStmt>();
        declaration->location = statement->location;
        declaration->name = name;
        declaration->local = local;
        declaration->isLinear =
            usage == luna::ownership::Usage::Linear;
        declaration->usage = usage;
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
        call->args.push_back(identifier(argument));
        call->type = resultType;
        if (!callable.empty() && callable.value < mGraph->locals.size()) {
            const auto* signature = mModule->findType(
                mGraph->locals[callable.value].type);
            if (signature && signature->kind == TypeKind::Function)
                call->returnUsage = signature->returnContract.usage;
        }
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
            itemValue = std::move(element);
        }
    }
    const bool hasValueAdapter = std::any_of(
        plan.steps.begin(), plan.steps.end(), [](const auto& step) {
            return step.op == IteratorOp::Map ||
                step.op == IteratorOp::Filter;
        });
    LocalId currentItem;
    if (hasValueAdapter) {
        currentItem = addBodyBinding(
            bodyEntry, "$for.item." + identity + ".source",
            sourceItemType, luna::ownership::Usage::Copy,
            std::move(itemValue));
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
            currentItem = addBodyBinding(
                bodyStart.block,
                "$for.item." + identity + "." +
                    std::to_string(stepIndex),
                step.outputType, luna::ownership::Usage::Copy,
                std::move(mapped));
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

    if (hasValueAdapter)
        addBodyBinding(
            bodyStart.block, statement->varName, statement->elementType,
            statement->bindingUsage, identifier(currentItem));

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
    popBindings();

    if (bodyExit) {
        if (increment.empty())
            increment = addBlock(
                loopRegion, loopScope, statement->location);
        connectJump(*bodyExit, increment);
    }
    if (!increment.empty()) {
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
        connectJump(OpenBlock{increment, {}}, condition);
        mGraph->regions[bodyRegion.value].exit = increment;
    }
    mGraph->regions[loopRegion.value].exit = exit;
    popBindings();
    return OpenBlock{exit, {}};
}

std::optional<ControlFlowBuilder::OpenBlock>
ControlFlowBuilder::lowerShortCircuitExpression(
    std::unique_ptr<BinaryExpr> expression, OpenBlock current,
    RegionId region, ScopeId scope, std::unique_ptr<Expr>& replacement) {
    if (!expression || !expression->lhs || !expression->rhs ||
        (expression->op != Operator::LogicalAnd &&
         expression->op != Operator::LogicalOr)) {
        error(expression ? expression->location : SourceLocation{},
              "short-circuit expression has an invalid canonical shape");
        return std::nullopt;
    }
    const auto* resultType = mModule->findType(expression->type);
    if (!resultType || resultType->kind != TypeKind::Bool ||
        expression->lhs->type != expression->type ||
        expression->rhs->type != expression->type) {
        error(expression->location,
              "short-circuit expression must have frozen bool operands");
        return std::nullopt;
    }

    auto lhs = normalizeControlFlowExpression(
        expression->lhs, std::move(current), region, scope, false);
    if (!lhs) return std::nullopt;
    current = std::move(*lhs);
    if (!bindExpr(expression->lhs.get())) return std::nullopt;

    const std::string name =
        "$short-circuit." + std::to_string(mExpressionCounter++);
    const LocalId result = addLocal(
        scope, LocalKind::Synthetic, name, expression->type,
        luna::ownership::Usage::Copy);
    if (result.empty()) return std::nullopt;

    auto initial = std::make_unique<BoolLiteralExpr>();
    initial->location = expression->location;
    initial->value = expression->op == Operator::LogicalOr;
    initial->type = expression->type;
    auto declaration = std::make_unique<LetStmt>();
    declaration->location = expression->location;
    declaration->name = name;
    declaration->local = result;
    declaration->usage = luna::ownership::Usage::Copy;
    declaration->type = expression->type;
    declaration->initializer = std::move(initial);
    mGraph->blocks[current.block.value].operations.push_back(
        std::move(declaration));

    const BlockId rhsEntry = addBlock(
        region, scope, expression->rhs->location);
    const BlockId merge = addBlock(
        region, scope, expression->location);
    auto& branch = mGraph->blocks[current.block.value].terminator;
    branch.kind = TerminatorKind::Branch;
    branch.location = expression->location;
    branch.operand = std::move(expression->lhs);
    if (expression->op == Operator::LogicalAnd) {
        branch.primary.target = rhsEntry;
        branch.secondary.target = merge;
    } else {
        branch.primary.target = merge;
        branch.secondary.target = rhsEntry;
    }

    auto rhs = normalizeControlFlowExpression(
        expression->rhs, OpenBlock{rhsEntry, {}}, region, scope, false);
    if (rhs) {
        if (!bindExpr(expression->rhs.get())) return std::nullopt;
        auto destination = std::make_unique<IdentifierExpr>();
        destination->location = expression->location;
        destination->name = name;
        destination->local = result;
        destination->type = expression->type;
        auto assignment = std::make_unique<AssignExpr>();
        assignment->location = expression->location;
        assignment->op = Operator::Assign;
        assignment->lhs = std::move(destination);
        assignment->rhs = std::move(expression->rhs);
        assignment->type = expression->type;
        auto statement = std::make_unique<ExprStmt>();
        statement->location = expression->location;
        statement->expr = std::move(assignment);
        mGraph->blocks[rhs->block.value].operations.push_back(
            std::move(statement));
        connectJump(*rhs, merge);
    }

    auto value = std::make_unique<IdentifierExpr>();
    value->location = expression->location;
    value->name = name;
    value->local = result;
    value->type = expression->type;
    replacement = std::move(value);
    return OpenBlock{merge, {}};
}

std::optional<ControlFlowBuilder::OpenBlock>
ControlFlowBuilder::lowerTryExpression(
    std::unique_ptr<TryExpr> expression, OpenBlock current,
    RegionId region, ScopeId scope, std::unique_ptr<Expr>& replacement) {
    if (!expression || !expression->operand) {
        error(expression ? expression->location : SourceLocation{},
              "error propagation has no operand");
        return std::nullopt;
    }

    auto operand = normalizeControlFlowExpression(
        expression->operand, std::move(current), region, scope, false);
    if (!operand) return std::nullopt;
    current = std::move(*operand);
    if (!bindExpr(expression->operand.get())) return std::nullopt;

    const auto* sourceResult = mModule->findType(expression->resultType);
    const auto* targetResult = mModule->findType(
        expression->propagatedResultType);
    const auto* valueType = mModule->findType(expression->valueType);
    const auto* errorType = mModule->findType(expression->errorType);
    const auto* targetErrorType = mModule->findType(
        expression->propagatedErrorType);
    if (!sourceResult || sourceResult->kind != TypeKind::Result ||
        sourceResult->typeArgumentIds.size() != 2 ||
        sourceResult->typeArgumentIds[0] != expression->valueType ||
        sourceResult->typeArgumentIds[1] != expression->errorType ||
        !targetResult || targetResult->kind != TypeKind::Result ||
        targetResult->typeArgumentIds.size() != 2 ||
        targetResult->typeArgumentIds[1] !=
            expression->propagatedErrorType ||
        !valueType || !errorType || !targetErrorType ||
        expression->operand->type != expression->resultType ||
        expression->type != expression->valueType) {
        error(expression->location,
              "error propagation disagrees with its frozen Result types");
        return std::nullopt;
    }

    const DeclarationRecord* conversion = nullptr;
    const TypeRecord* conversionType = nullptr;
    if (expression->errorType == expression->propagatedErrorType) {
        if (!expression->errorConversion.empty()) {
            error(expression->location,
                  "error propagation has an unnecessary From conversion");
            return std::nullopt;
        }
    } else {
        conversion = mModule->findDeclaration(expression->errorConversion);
        conversionType = conversion
            ? mModule->findType(conversion->type) : nullptr;
        if (!conversion || conversion->kind != DeclarationKind::Function ||
            !conversionType || conversionType->kind != TypeKind::Function ||
            conversionType->parameterTypeIds !=
                TypeRefVec{expression->errorType} ||
            conversionType->returnTypeId !=
                expression->propagatedErrorType ||
            conversionType->parameterContracts.size() != 1 ||
            conversionType->parameterContracts.front().relation !=
                luna::ownership::Relation::Owned ||
            conversionType->returnContract.relation !=
                luna::ownership::Relation::Owned) {
            error(expression->location,
                  "error propagation From witness has no canonical owned conversion contract");
            return std::nullopt;
        }
    }

    const std::string identity =
        std::to_string(mExpressionCounter++);
    const auto usageOf = [](const TypeRecord* type) {
        return type
            ? type->sysmeta.resource.usage
            : luna::ownership::Usage::Copy;
    };
    const LocalId successLocal = addLocal(
        scope, LocalKind::Pattern, "$try.value." + identity,
        expression->valueType, usageOf(valueType));
    const LocalId errorLocal = addLocal(
        scope, LocalKind::Pattern, "$try.error." + identity,
        expression->errorType, usageOf(errorType));
    if (successLocal.empty() || errorLocal.empty())
        return std::nullopt;

    const BlockId success = addBlock(region, scope, expression->location);
    const BlockId failure = addBlock(region, scope, expression->location);
    const BlockId invalid = addBlock(region, scope, expression->location);
    auto& invalidTerminator = mGraph->blocks[invalid.value].terminator;
    invalidTerminator.kind = TerminatorKind::Unreachable;
    invalidTerminator.location = expression->location;

    const auto identifier = [this, &expression](LocalId local) {
        auto value = std::make_unique<IdentifierExpr>();
        value->location = expression->location;
        if (!local.empty() && local.value < mGraph->locals.size()) {
            const auto& record = mGraph->locals[local.value];
            value->name = record.name;
            value->local = local;
            value->type = record.type;
        }
        return value;
    };
    const auto transferIfNeeded = [&expression, &usageOf](
        std::unique_ptr<Expr> value, const TypeRecord* type) {
        if (!type || !luna::ownership::isMoveOnly(usageOf(type)))
            return value;
        auto transfer = std::make_unique<MoveExpr>();
        transfer->location = expression->location;
        transfer->type = type->id;
        transfer->operand = std::move(value);
        return std::unique_ptr<Expr>(std::move(transfer));
    };

    std::unique_ptr<Expr> propagatedError = transferIfNeeded(
        identifier(errorLocal), errorType);
    if (conversion && conversionType) {
        auto call = std::make_unique<CallExpr>();
        call->location = expression->location;
        call->calleeRef = expression->errorConversion;
        call->type = expression->propagatedErrorType;
        call->returnUsage = conversionType->returnContract.usage;
        call->returnsLinear =
            call->returnUsage == luna::ownership::Usage::Linear;
        auto callee = std::make_unique<IdentifierExpr>();
        callee->location = expression->location;
        callee->name = conversion->sourceName;
        callee->declaration = expression->errorConversion;
        callee->type = conversion->type;
        call->callee = std::move(callee);
        call->args.push_back(std::move(propagatedError));
        propagatedError = std::move(call);
    }

    auto propagatedResult = std::make_unique<ResultConstructExpr>();
    propagatedResult->location = expression->location;
    propagatedResult->isOk = false;
    propagatedResult->payload = std::move(propagatedError);
    propagatedResult->type = expression->propagatedResultType;
    if (!bindExpr(propagatedResult.get())) return std::nullopt;

    auto failureCleanups = lowerCleanupObligations(
        expression->cleanups, scope);
    failureCleanups.insert(
        failureCleanups.end(), current.cleanups.begin(),
        current.cleanups.end());
    failureCleanups.insert(
        failureCleanups.end(), mActiveExpressionCleanups.begin(),
        mActiveExpressionCleanups.end());
    auto& failureTerminator = mGraph->blocks[failure.value].terminator;
    failureTerminator.kind = TerminatorKind::Return;
    failureTerminator.location = expression->location;
    failureTerminator.operand = std::move(propagatedResult);
    failureTerminator.exitCleanups = canonicalCleanupOrder(
        failureCleanups, scope, std::nullopt);

    Terminator switchTerminator;
    switchTerminator.kind = TerminatorKind::Switch;
    switchTerminator.location = expression->location;
    switchTerminator.operand = transferIfNeeded(
        std::move(expression->operand), sourceResult);
    switchTerminator.switchType = expression->resultType;
    switchTerminator.primary.target = invalid;
    SwitchEdge errorEdge;
    errorEdge.tag = 0;
    errorEdge.edge.target = failure;
    errorEdge.bindings.push_back(errorLocal);
    switchTerminator.cases.push_back(std::move(errorEdge));
    SwitchEdge successEdge;
    successEdge.tag = 1;
    successEdge.edge.target = success;
    successEdge.bindings.push_back(successLocal);
    switchTerminator.cases.push_back(std::move(successEdge));
    mGraph->blocks[current.block.value].terminator =
        std::move(switchTerminator);

    replacement = transferIfNeeded(
        identifier(successLocal), valueType);
    return OpenBlock{success, {}};
}

std::optional<ControlFlowBuilder::OpenBlock>
ControlFlowBuilder::lowerBlockExpression(
    std::unique_ptr<BlockExpr> expression, OpenBlock current,
    RegionId region, ScopeId scope, std::unique_ptr<Expr>& replacement) {
    if (!expression || !expression->block) {
        error(expression ? expression->location : SourceLocation{},
              "block expression has no structured body");
        return std::nullopt;
    }
    const auto* resultType = mModule->findType(expression->type);
    if (!resultType || resultType->kind != TypeKind::Unit) {
        error(expression->location,
              "block expression must have the frozen unit type");
        return std::nullopt;
    }

    const SourceLocation location = expression->location;
    auto body = lowerNestedBlock(
        std::move(expression->block), region, scope,
        RegionKind::Lexical);
    connectJump(current, body.entry);
    if (!body.exit) return std::nullopt;

    const BlockId continuation = addBlock(region, scope, location);
    connectJump(*body.exit, continuation);
    mGraph->regions[body.region.value].exit = continuation;

    auto unit = std::make_unique<UnitExpr>();
    unit->location = location;
    unit->type = expression->type;
    replacement = std::move(unit);
    return OpenBlock{continuation, {}};
}

std::optional<ControlFlowBuilder::OpenBlock>
ControlFlowBuilder::lowerIfExpression(
    std::unique_ptr<IfExpr> expression, OpenBlock current,
    RegionId region, ScopeId scope, std::unique_ptr<Expr>& replacement) {
    if (!expression || !expression->cond || !expression->thenExpr ||
        !expression->elseExpr) {
        error(expression ? expression->location : SourceLocation{},
              "if expression has an incomplete canonical shape");
        return std::nullopt;
    }
    const auto* resultType = mModule->findType(expression->type);
    if (!resultType || resultType->kind != TypeKind::Unit ||
        expression->thenExpr->type != expression->type ||
        expression->elseExpr->type != expression->type) {
        error(expression->location,
              "block-style if expression must have unit-typed branches");
        return std::nullopt;
    }

    auto condition = normalizeControlFlowExpression(
        expression->cond, std::move(current), region, scope, false);
    if (!condition) return std::nullopt;
    current = std::move(*condition);
    if (!bindExpr(expression->cond.get())) return std::nullopt;

    struct BuiltArm {
        BlockId entry;
        std::optional<OpenBlock> exit;
    };
    const auto lowerArm = [this, region, scope](
        std::unique_ptr<Expr> arm) -> BuiltArm {
        BuiltArm built;
        if (!arm) {
            error({}, "if expression has a null branch");
            return built;
        }
        const SourceLocation location = arm->location;
        built.entry = addBlock(region, scope, location);
        built.exit = normalizeControlFlowExpression(
            arm, OpenBlock{built.entry, {}}, region, scope, true);
        if (!built.exit || !arm || dynamic_cast<UnitExpr*>(arm.get()))
            return built;
        if (!bindExpr(arm.get())) {
            built.exit = std::nullopt;
            return built;
        }
        auto statement = std::make_unique<ExprStmt>();
        statement->location = location;
        statement->expr = std::move(arm);
        mGraph->blocks[built.exit->block.value].operations.push_back(
            std::move(statement));
        return built;
    };

    auto thenArm = lowerArm(std::move(expression->thenExpr));
    auto elseArm = lowerArm(std::move(expression->elseExpr));
    if (thenArm.entry.empty() || elseArm.entry.empty())
        return std::nullopt;

    auto& terminator = mGraph->blocks[current.block.value].terminator;
    terminator.kind = TerminatorKind::Branch;
    terminator.location = expression->location;
    terminator.operand = std::move(expression->cond);
    terminator.primary.target = thenArm.entry;
    terminator.secondary.target = elseArm.entry;

    if (!thenArm.exit && !elseArm.exit) return std::nullopt;
    const BlockId merge = addBlock(region, scope, expression->location);
    if (thenArm.exit) connectJump(*thenArm.exit, merge);
    if (elseArm.exit) connectJump(*elseArm.exit, merge);

    auto unit = std::make_unique<UnitExpr>();
    unit->location = expression->location;
    unit->type = expression->type;
    replacement = std::move(unit);
    return OpenBlock{merge, {}};
}

std::optional<ControlFlowBuilder::OpenBlock>
ControlFlowBuilder::normalizeControlFlowExpression(
    std::unique_ptr<Expr>& expression, OpenBlock current,
    RegionId region, ScopeId scope, bool discardUnitResult) {
    if (!expression) return current;
    if (dynamic_cast<TryExpr*>(expression.get())) {
        std::unique_ptr<TryExpr> owned(
            static_cast<TryExpr*>(expression.release()));
        return lowerTryExpression(
            std::move(owned), std::move(current), region, scope,
            expression);
    }
    if (dynamic_cast<BlockExpr*>(expression.get())) {
        std::unique_ptr<BlockExpr> owned(
            static_cast<BlockExpr*>(expression.release()));
        return lowerBlockExpression(
            std::move(owned), std::move(current), region, scope,
            expression);
    }
    if (dynamic_cast<IfExpr*>(expression.get())) {
        std::unique_ptr<IfExpr> owned(
            static_cast<IfExpr*>(expression.release()));
        return lowerIfExpression(
            std::move(owned), std::move(current), region, scope,
            expression);
    }
    if (auto* binary = dynamic_cast<BinaryExpr*>(expression.get());
        binary && (binary->op == Operator::LogicalAnd ||
                   binary->op == Operator::LogicalOr)) {
        std::unique_ptr<BinaryExpr> owned(
            static_cast<BinaryExpr*>(expression.release()));
        return lowerShortCircuitExpression(
            std::move(owned), std::move(current), region, scope,
            expression);
    }
    auto* call = dynamic_cast<CallExpr*>(expression.get());
    if (call && containsIteratorTerminal(call) &&
        (call->iteratorOp == IteratorOp::Fold ||
         call->iteratorOp == IteratorOp::ForEach ||
         call->iteratorOp == IteratorOp::Count ||
         call->iteratorOp == IteratorOp::Collect)) {
        std::unique_ptr<CallExpr> owned(
            static_cast<CallExpr*>(expression.release()));
        return lowerIteratorTerminal(
            std::move(owned), std::move(current), region, scope,
            discardUnitResult, expression);
    }

    if (call && call->iteratorOp == IteratorOp::None) {
        std::vector<std::unique_ptr<Expr>*> operands;
        operands.reserve(call->args.size() + 1);
        operands.push_back(&call->callee);
        for (auto& argument : call->args)
            operands.push_back(&argument);
        return normalizeOrderedOperands(
            operands, std::move(current), region, scope);
    }
    if (auto* binary = dynamic_cast<BinaryExpr*>(expression.get())) {
        return normalizeOrderedOperands(
            {&binary->lhs, &binary->rhs}, std::move(current),
            region, scope);
    }
    if (auto* unary = dynamic_cast<UnaryExpr*>(expression.get()))
        return normalizeControlFlowExpression(
            unary->operand, std::move(current), region, scope, false);
    if (auto* field = dynamic_cast<FieldAccessExpr*>(expression.get()))
        return normalizeControlFlowExpression(
            field->object, std::move(current), region, scope, false);
    if (auto* index = dynamic_cast<IndexExpr*>(expression.get()))
        return normalizeOrderedOperands(
            {&index->object, &index->index}, std::move(current),
            region, scope);
    if (auto* length = dynamic_cast<SliceLengthExpr*>(expression.get()))
        return normalizeControlFlowExpression(
            length->slice, std::move(current), region, scope, false);
    if (auto* array = dynamic_cast<ArrayLiteralExpr*>(expression.get())) {
        std::vector<std::unique_ptr<Expr>*> operands;
        operands.reserve(array->elements.size());
        for (auto& element : array->elements)
            operands.push_back(&element);
        return normalizeOrderedOperands(
            operands, std::move(current), region, scope);
    }
    if (auto* record = dynamic_cast<RecordLiteralExpr*>(expression.get())) {
        const auto* recordType = mModule->findType(record->type);
        if (!recordType ||
            (recordType->kind != TypeKind::Record &&
             recordType->kind != TypeKind::Struct)) {
            error(record->location,
                  "record initializer has no frozen product type");
            return std::nullopt;
        }
        if (recordType->kind == TypeKind::Record) {
            std::vector<std::unique_ptr<Expr>*> operands;
            operands.reserve(record->fields.size());
            for (auto& field : record->fields)
                operands.push_back(&field.value);
            return normalizeOrderedOperands(
                operands, std::move(current), region, scope);
        }
        for (const auto& field : record->fields) {
            if (!containsPendingControlFlow(field.value.get())) continue;
            error(field.value->location,
                  "control flow in an allocating struct initializer requires "
                  "allocation-aware expression CFG normalization");
            return std::nullopt;
        }
        return current;
    }
    if (auto* allocation = dynamic_cast<HeapAllocExpr*>(expression.get())) {
        if (containsPendingControlFlow(allocation->initializer.get())) {
            error(allocation->initializer->location,
                  "control flow in a heap initializer requires "
                  "allocation-aware expression CFG normalization");
            return std::nullopt;
        }
        return current;
    }
    if (auto* selection =
            dynamic_cast<DynamicSelectExpr*>(expression.get())) {
        std::vector<std::unique_ptr<Expr>*> operands;
        operands.reserve(selection->filterArguments.size());
        for (auto& argument : selection->filterArguments)
            operands.push_back(&argument);
        return normalizeOrderedOperands(
            operands, std::move(current), region, scope);
    }
    if (auto* launch = dynamic_cast<LaunchExpr*>(expression.get())) {
        std::vector<std::unique_ptr<Expr>*> operands;
        operands.reserve(launch->args.size() + 1);
        operands.push_back(&launch->threads);
        for (auto& argument : launch->args)
            operands.push_back(&argument);
        return normalizeOrderedOperands(
            operands, std::move(current), region, scope);
    }
    if (auto* variant =
            dynamic_cast<VariantConstructExpr*>(expression.get())) {
        std::vector<std::unique_ptr<Expr>*> operands;
        operands.reserve(variant->args.size());
        for (auto& argument : variant->args)
            operands.push_back(&argument);
        return normalizeOrderedOperands(
            operands, std::move(current), region, scope);
    }
    if (auto* move = dynamic_cast<MoveExpr*>(expression.get()))
        return normalizeControlFlowExpression(
            move->operand, std::move(current), region, scope, false);
    if (auto* borrow = dynamic_cast<BorrowExpr*>(expression.get()))
        return normalizeControlFlowExpression(
            borrow->operand, std::move(current), region, scope, false);
    if (auto* dereference = dynamic_cast<DerefExpr*>(expression.get()))
        return normalizeControlFlowExpression(
            dereference->operand, std::move(current), region, scope, false);
    if (auto* address = dynamic_cast<AddrOfExpr*>(expression.get()))
        return normalizeControlFlowExpression(
            address->operand, std::move(current), region, scope, false);
    if (auto* assignment = dynamic_cast<AssignExpr*>(expression.get())) {
        if (containsIteratorTerminal(assignment->lhs.get())) {
            error(assignment->lhs->location,
                  "iterator terminal cannot form an assignment destination");
            return std::nullopt;
        }
        return normalizeControlFlowExpression(
            assignment->rhs, std::move(current), region, scope, false);
    }
    return current;
}

std::optional<ControlFlowBuilder::OpenBlock>
ControlFlowBuilder::normalizeOrderedOperands(
    const std::vector<std::unique_ptr<Expr>*>& operands,
    OpenBlock current, RegionId region, ScopeId scope) {
    const size_t cleanupDepth = mActiveExpressionCleanups.size();
    const auto restoreCleanupDepth = [this, cleanupDepth] {
        mActiveExpressionCleanups.resize(cleanupDepth);
    };
    for (size_t index = 0; index < operands.size(); ++index) {
        auto* operand = operands[index];
        if (!operand || !containsPendingControlFlow(operand->get())) continue;

        bool allowLinear = true;
        for (size_t following = index; following < operands.size();
             ++following) {
            const auto* candidate = operands[following];
            if (candidate && containsPotentialEarlyExit(candidate->get())) {
                allowLinear = false;
                break;
            }
        }

        // A later control-flow operand must not move evaluation of any
        // preceding value across its new CFG. Each eager value is frozen into
        // a synthetic local in source order. Copy values are read normally;
        // affine values are explicitly transferred once, while their cleanup
        // rows remain visible to any early exit until the parent is emitted.
        for (size_t previous = 0; previous < index; ++previous) {
            auto* earlier = operands[previous];
            if (earlier &&
                !hoistOrderedOperand(
                    *earlier, current, scope, allowLinear)) {
                restoreCleanupDepth();
                return std::nullopt;
            }
        }

        auto normalized = normalizeControlFlowExpression(
            *operand, std::move(current), region, scope, false);
        if (!normalized) {
            restoreCleanupDepth();
            return std::nullopt;
        }
        current = std::move(*normalized);
        if (containsPendingControlFlow(operand->get())) {
            error((*operand)->location,
                  "control-flow expression remains in an unsupported position");
            restoreCleanupDepth();
            return std::nullopt;
        }
    }
    restoreCleanupDepth();
    return current;
}

bool ControlFlowBuilder::containsIteratorTerminal(
    const Expr* expression) const {
    if (!expression) return false;
    if (const auto* call = dynamic_cast<const CallExpr*>(expression)) {
        if (call->iteratorOp == IteratorOp::Fold ||
            call->iteratorOp == IteratorOp::ForEach ||
            call->iteratorOp == IteratorOp::Count ||
            call->iteratorOp == IteratorOp::Collect)
            return true;
        if (containsIteratorTerminal(call->callee.get())) return true;
        for (const auto& argument : call->args)
            if (containsIteratorTerminal(argument.get())) return true;
        return false;
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(expression))
        return containsIteratorTerminal(binary->lhs.get()) ||
            containsIteratorTerminal(binary->rhs.get());
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(expression))
        return containsIteratorTerminal(unary->operand.get());
    if (const auto* selection =
            dynamic_cast<const DynamicSelectExpr*>(expression)) {
        for (const auto& argument : selection->filterArguments)
            if (containsIteratorTerminal(argument.get())) return true;
        return false;
    }
    if (const auto* launch = dynamic_cast<const LaunchExpr*>(expression)) {
        if (containsIteratorTerminal(launch->threads.get())) return true;
        for (const auto& argument : launch->args)
            if (containsIteratorTerminal(argument.get())) return true;
        return false;
    }
    if (const auto* variant =
            dynamic_cast<const VariantConstructExpr*>(expression)) {
        for (const auto& argument : variant->args)
            if (containsIteratorTerminal(argument.get())) return true;
        return false;
    }
    if (const auto* result =
            dynamic_cast<const ResultConstructExpr*>(expression))
        return containsIteratorTerminal(result->payload.get());
    if (const auto* field =
            dynamic_cast<const FieldAccessExpr*>(expression))
        return containsIteratorTerminal(field->object.get());
    if (const auto* index = dynamic_cast<const IndexExpr*>(expression))
        return containsIteratorTerminal(index->object.get()) ||
            containsIteratorTerminal(index->index.get());
    if (const auto* length =
            dynamic_cast<const SliceLengthExpr*>(expression))
        return containsIteratorTerminal(length->slice.get());
    if (const auto* array =
            dynamic_cast<const ArrayLiteralExpr*>(expression)) {
        for (const auto& element : array->elements)
            if (containsIteratorTerminal(element.get())) return true;
        return false;
    }
    if (const auto* record =
            dynamic_cast<const RecordLiteralExpr*>(expression)) {
        for (const auto& field : record->fields)
            if (containsIteratorTerminal(field.value.get())) return true;
        return false;
    }
    if (const auto* allocation =
            dynamic_cast<const HeapAllocExpr*>(expression))
        return containsIteratorTerminal(allocation->initializer.get());
    if (const auto* move = dynamic_cast<const MoveExpr*>(expression))
        return containsIteratorTerminal(move->operand.get());
    if (const auto* borrow = dynamic_cast<const BorrowExpr*>(expression))
        return containsIteratorTerminal(borrow->operand.get());
    if (const auto* dereference =
            dynamic_cast<const DerefExpr*>(expression))
        return containsIteratorTerminal(dereference->operand.get());
    if (const auto* address = dynamic_cast<const AddrOfExpr*>(expression))
        return containsIteratorTerminal(address->operand.get());
    if (const auto* assignment =
            dynamic_cast<const AssignExpr*>(expression))
        return containsIteratorTerminal(assignment->lhs.get()) ||
            containsIteratorTerminal(assignment->rhs.get());
    return false;
}

bool ControlFlowBuilder::containsPendingControlFlow(
    const Expr* expression) const {
    if (!expression) return false;
    if (dynamic_cast<const TryExpr*>(expression) ||
        dynamic_cast<const BlockExpr*>(expression) ||
        dynamic_cast<const IfExpr*>(expression))
        return true;
    if (const auto* call = dynamic_cast<const CallExpr*>(expression)) {
        if (call->iteratorOp == IteratorOp::Fold ||
            call->iteratorOp == IteratorOp::ForEach ||
            call->iteratorOp == IteratorOp::Count ||
            call->iteratorOp == IteratorOp::Collect)
            return true;
        if (containsPendingControlFlow(call->callee.get())) return true;
        for (const auto& argument : call->args)
            if (containsPendingControlFlow(argument.get())) return true;
        return false;
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(expression)) {
        if (binary->op == Operator::LogicalAnd ||
            binary->op == Operator::LogicalOr)
            return true;
        return containsPendingControlFlow(binary->lhs.get()) ||
            containsPendingControlFlow(binary->rhs.get());
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(expression))
        return containsPendingControlFlow(unary->operand.get());
    if (const auto* selection =
            dynamic_cast<const DynamicSelectExpr*>(expression)) {
        for (const auto& argument : selection->filterArguments)
            if (containsPendingControlFlow(argument.get())) return true;
        return false;
    }
    if (const auto* launch = dynamic_cast<const LaunchExpr*>(expression)) {
        if (containsPendingControlFlow(launch->threads.get())) return true;
        for (const auto& argument : launch->args)
            if (containsPendingControlFlow(argument.get())) return true;
        return false;
    }
    if (const auto* variant =
            dynamic_cast<const VariantConstructExpr*>(expression)) {
        for (const auto& argument : variant->args)
            if (containsPendingControlFlow(argument.get())) return true;
        return false;
    }
    if (const auto* result =
            dynamic_cast<const ResultConstructExpr*>(expression))
        return containsPendingControlFlow(result->payload.get());
    if (const auto* field = dynamic_cast<const FieldAccessExpr*>(expression))
        return containsPendingControlFlow(field->object.get());
    if (const auto* index = dynamic_cast<const IndexExpr*>(expression))
        return containsPendingControlFlow(index->object.get()) ||
            containsPendingControlFlow(index->index.get());
    if (const auto* length = dynamic_cast<const SliceLengthExpr*>(expression))
        return containsPendingControlFlow(length->slice.get());
    if (const auto* array = dynamic_cast<const ArrayLiteralExpr*>(expression)) {
        for (const auto& element : array->elements)
            if (containsPendingControlFlow(element.get())) return true;
        return false;
    }
    if (const auto* record = dynamic_cast<const RecordLiteralExpr*>(expression)) {
        for (const auto& field : record->fields)
            if (containsPendingControlFlow(field.value.get())) return true;
        return false;
    }
    if (const auto* allocation = dynamic_cast<const HeapAllocExpr*>(expression))
        return containsPendingControlFlow(allocation->initializer.get());
    if (const auto* move = dynamic_cast<const MoveExpr*>(expression))
        return containsPendingControlFlow(move->operand.get());
    if (const auto* borrow = dynamic_cast<const BorrowExpr*>(expression))
        return containsPendingControlFlow(borrow->operand.get());
    if (const auto* dereference = dynamic_cast<const DerefExpr*>(expression))
        return containsPendingControlFlow(dereference->operand.get());
    if (const auto* address = dynamic_cast<const AddrOfExpr*>(expression))
        return containsPendingControlFlow(address->operand.get());
    if (const auto* assignment = dynamic_cast<const AssignExpr*>(expression))
        return containsPendingControlFlow(assignment->lhs.get()) ||
            containsPendingControlFlow(assignment->rhs.get());
    return false;
}

bool ControlFlowBuilder::containsPotentialEarlyExit(
    const Expr* expression) const {
    if (!expression) return false;
    if (dynamic_cast<const TryExpr*>(expression) ||
        dynamic_cast<const BlockExpr*>(expression) ||
        dynamic_cast<const IfExpr*>(expression))
        return true;
    if (const auto* call = dynamic_cast<const CallExpr*>(expression)) {
        if (containsPotentialEarlyExit(call->callee.get())) return true;
        for (const auto& argument : call->args)
            if (containsPotentialEarlyExit(argument.get())) return true;
        return false;
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(expression))
        return containsPotentialEarlyExit(binary->lhs.get()) ||
            containsPotentialEarlyExit(binary->rhs.get());
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(expression))
        return containsPotentialEarlyExit(unary->operand.get());
    if (const auto* selection =
            dynamic_cast<const DynamicSelectExpr*>(expression)) {
        for (const auto& argument : selection->filterArguments)
            if (containsPotentialEarlyExit(argument.get())) return true;
        return false;
    }
    if (const auto* launch = dynamic_cast<const LaunchExpr*>(expression)) {
        if (containsPotentialEarlyExit(launch->threads.get())) return true;
        for (const auto& argument : launch->args)
            if (containsPotentialEarlyExit(argument.get())) return true;
        return false;
    }
    if (const auto* variant =
            dynamic_cast<const VariantConstructExpr*>(expression)) {
        for (const auto& argument : variant->args)
            if (containsPotentialEarlyExit(argument.get())) return true;
        return false;
    }
    if (const auto* result =
            dynamic_cast<const ResultConstructExpr*>(expression))
        return containsPotentialEarlyExit(result->payload.get());
    if (const auto* field = dynamic_cast<const FieldAccessExpr*>(expression))
        return containsPotentialEarlyExit(field->object.get());
    if (const auto* index = dynamic_cast<const IndexExpr*>(expression))
        return containsPotentialEarlyExit(index->object.get()) ||
            containsPotentialEarlyExit(index->index.get());
    if (const auto* length = dynamic_cast<const SliceLengthExpr*>(expression))
        return containsPotentialEarlyExit(length->slice.get());
    if (const auto* array = dynamic_cast<const ArrayLiteralExpr*>(expression)) {
        for (const auto& element : array->elements)
            if (containsPotentialEarlyExit(element.get())) return true;
        return false;
    }
    if (const auto* record = dynamic_cast<const RecordLiteralExpr*>(expression)) {
        for (const auto& field : record->fields)
            if (containsPotentialEarlyExit(field.value.get())) return true;
        return false;
    }
    if (const auto* allocation = dynamic_cast<const HeapAllocExpr*>(expression))
        return containsPotentialEarlyExit(allocation->initializer.get());
    if (const auto* move = dynamic_cast<const MoveExpr*>(expression))
        return containsPotentialEarlyExit(move->operand.get());
    if (const auto* borrow = dynamic_cast<const BorrowExpr*>(expression))
        return containsPotentialEarlyExit(borrow->operand.get());
    if (const auto* dereference = dynamic_cast<const DerefExpr*>(expression))
        return containsPotentialEarlyExit(dereference->operand.get());
    if (const auto* address = dynamic_cast<const AddrOfExpr*>(expression))
        return containsPotentialEarlyExit(address->operand.get());
    if (const auto* assignment = dynamic_cast<const AssignExpr*>(expression))
        return containsPotentialEarlyExit(assignment->lhs.get()) ||
            containsPotentialEarlyExit(assignment->rhs.get());
    return false;
}

bool ControlFlowBuilder::hoistOrderedOperand(
    std::unique_ptr<Expr>& expression, OpenBlock& current,
    ScopeId scope, bool allowLinear) {
    if (!expression) return true;
    if (dynamic_cast<IntLiteralExpr*>(expression.get()) ||
        dynamic_cast<FloatLiteralExpr*>(expression.get()) ||
        dynamic_cast<StringLiteralExpr*>(expression.get()) ||
        dynamic_cast<BoolLiteralExpr*>(expression.get()) ||
        dynamic_cast<UnitExpr*>(expression.get()))
        return true;

    if (!bindExpr(expression.get())) return false;
    if (const auto* identifier =
            dynamic_cast<const IdentifierExpr*>(expression.get())) {
        if (!identifier->declaration.empty()) return true;
        const auto* local = mGraph->findLocal(identifier->local);
        if (local && local->kind == LocalKind::Synthetic) return true;
    }
    if (const auto* transfer =
            dynamic_cast<const MoveExpr*>(expression.get())) {
        const auto* identifier = dynamic_cast<const IdentifierExpr*>(
            transfer->operand.get());
        const auto* local = identifier
            ? mGraph->findLocal(identifier->local) : nullptr;
        if (local && local->kind == LocalKind::Synthetic) return true;
    }

    const auto* type = mModule->findType(expression->type);
    if (!type) {
        error(expression->location,
              "expression sibling has no frozen type for CFG hoisting");
        return false;
    }
    auto usage = type->sysmeta.resource.usage;
    bool explicitFreshTransfer = false;
    if (const auto* call = dynamic_cast<const CallExpr*>(expression.get())) {
        usage = luna::ownership::strongerUsage(
            usage, call->returnUsage);
        explicitFreshTransfer = luna::ownership::isMoveOnly(
            call->returnUsage);
    } else if (const auto* transfer =
                   dynamic_cast<const MoveExpr*>(expression.get())) {
        explicitFreshTransfer = true;
        if (const auto* identifier =
                dynamic_cast<const IdentifierExpr*>(
                    transfer->operand.get())) {
            if (const auto* local = mGraph->findLocal(identifier->local))
                usage = luna::ownership::strongerUsage(
                    usage, local->usage);
        }
    }
    if (type->kind == TypeKind::Unit) {
        error(expression->location,
              "expression sibling hoisting requires a non-unit value");
        return false;
    }
    if (usage == luna::ownership::Usage::Linear && !allowLinear) {
        error(expression->location,
              "linear expression sibling may cross an early-exit CFG path");
        return false;
    }
    if (luna::ownership::isMoveOnly(usage) &&
        !explicitFreshTransfer) {
        error(expression->location,
              "move-only expression sibling hoisting requires an explicit transfer");
        return false;
    }

    const std::string name =
        "$expression.hoist." + std::to_string(mExpressionCounter++);
    const LocalId local = addLocal(
        scope, LocalKind::Synthetic, name, expression->type,
        usage);
    if (local.empty()) return false;

    auto declaration = std::make_unique<LetStmt>();
    declaration->location = expression->location;
    declaration->name = name;
    declaration->local = local;
    declaration->isLinear =
        usage == luna::ownership::Usage::Linear;
    declaration->usage = usage;
    declaration->type = expression->type;
    declaration->initializer = std::move(expression);
    mGraph->blocks[current.block.value].operations.push_back(
        std::move(declaration));
    if (type->sysmeta.resource.cleanupRequired) {
        const auto cleanup = mCleanupByLocal.find(local.value);
        if (cleanup == mCleanupByLocal.end()) {
            error(expression->location,
                  "cleanup-bearing expression sibling has no canonical cleanup row");
            return false;
        }
        mActiveExpressionCleanups.push_back(cleanup->second);
    }

    auto identifier = std::make_unique<IdentifierExpr>();
    identifier->location =
        mGraph->blocks[current.block.value].location;
    identifier->name = name;
    identifier->local = local;
    identifier->type = type->id;
    if (luna::ownership::isMoveOnly(usage)) {
        auto transfer = std::make_unique<MoveExpr>();
        transfer->location = identifier->location;
        transfer->type = type->id;
        transfer->operand = std::move(identifier);
        expression = std::move(transfer);
    } else {
        expression = std::move(identifier);
    }
    return true;
}

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
    if (!terminal->iteratorRecipeStateName.empty() ||
        !terminal->iteratorRecipeSourceType.empty()) {
        error(terminal->location,
              "move-only iterator terminal requires projected source cleanup state");
        return std::nullopt;
    }

    IteratorRecipePlan plan;
    if (!parseIteratorRecipe(
            std::move(member->object), plan, terminal->location))
        return std::nullopt;
    if (!bindIteratorRecipe(plan) ||
        !validateIteratorRecipe(
            plan, terminal->iteratorInputType, terminal->location))
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
        LocalKind kind = LocalKind::Binding) {
        const LocalId local = addLocal(
            scope, kind, name, type, usage);
        auto declaration = std::make_unique<LetStmt>();
        declaration->location = terminal->location;
        declaration->name = name;
        declaration->local = local;
        declaration->isLinear = usage == luna::ownership::Usage::Linear;
        declaration->usage = usage;
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
                if (!sourceType ||
                    sourceType->sysmeta.resource.cleanupRequired ||
                    sourceType->sysmeta.resource.usage !=
                        luna::ownership::Usage::Copy) {
                    error(terminal->location,
                          "temporary terminal source requires unsupported "
                          "canonical ownership state");
                    return std::nullopt;
                }
                recipe.source = addBinding(
                    "$terminal.source." + identity,
                    plan.sourceType, std::move(plan.source));
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

    auto loop = std::make_unique<ForStmt>();
    loop->location = terminal->location;
    loop->varName = "$terminal.item." + identity;
    loop->bindingUsage = luna::ownership::Usage::Copy;
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
            !reducerType || reducerType->kind != TypeKind::Function ||
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
            reducerType->parameterContracts[1].usage !=
                luna::ownership::Usage::Copy ||
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
        call->args.push_back(std::move(item));
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
        if (!actionType || actionType->kind != TypeKind::Function ||
            actionType->parameterTypeIds !=
                TypeRefVec{terminal->iteratorInputType} ||
            actionType->returnTypeId != unitType ||
            actionType->parameterContracts.size() != 1 ||
            actionType->parameterContracts[0].usage !=
                luna::ownership::Usage::Copy ||
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
        call->args.push_back(std::move(item));
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
        push->args.push_back(std::move(item));
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

std::optional<ControlFlowBuilder::OpenBlock> ControlFlowBuilder::lowerMatch(
    std::unique_ptr<MatchStmt> statement, OpenBlock current,
    RegionId region, ScopeId scope) {
    auto scrutinee = normalizeControlFlowExpression(
        statement->scrutinee, std::move(current), region, scope, false);
    if (!scrutinee) return std::nullopt;
    current = std::move(*scrutinee);
    if (!bindExpr(statement->scrutinee.get())) return std::nullopt;
    if (statement->arms.empty()) {
        error(statement->location, "match has no canonical switch cases");
        return std::nullopt;
    }

    struct BuiltArm {
        RegionId region;
        BlockId entry;
        std::optional<OpenBlock> exit;
        SwitchEdge edge;
    };
    std::vector<BuiltArm> arms;
    arms.reserve(statement->arms.size());
    for (auto& arm : statement->arms) {
        BuiltArm built;
        built.region = addRegion(
            region, RegionKind::MatchArm, arm.location);
        const ScopeId armScope = addScope(
            scope, built.region, arm.location);
        built.entry = addBlock(built.region, armScope, arm.location);
        built.edge.tag = arm.variantIndex;
        built.edge.edge.target = built.entry;
        pushBindings();
        if (arm.bindings.size() != arm.bindingTypes.size() ||
            arm.bindings.size() != arm.bindingUsages.size()) {
            error(arm.location,
                  "match arm binding metadata has inconsistent arity");
        }
        const size_t bindingCount = std::min(
            arm.bindings.size(),
            std::min(arm.bindingTypes.size(), arm.bindingUsages.size()));
        for (size_t index = 0; index < bindingCount; ++index) {
            const LocalId local = addLocal(
                armScope, LocalKind::Pattern, arm.bindings[index],
                arm.bindingTypes[index], arm.bindingUsages[index]);
            if (!local.empty()) built.edge.bindings.push_back(local);
        }
        if (!arm.body) {
            error(arm.location, "match arm has no body");
        } else {
            built.exit = lowerSequence(
                arm.body->stmts, OpenBlock{built.entry, {}},
                built.region, armScope);
        }
        popBindings();
        arms.push_back(std::move(built));
    }

    const BlockId invalid = addBlock(region, scope, statement->location);
    auto& invalidTerminator = mGraph->blocks[invalid.value].terminator;
    invalidTerminator.kind = TerminatorKind::Unreachable;
    invalidTerminator.location = statement->location;

    bool needsMerge = false;
    for (const auto& arm : arms)
        needsMerge = needsMerge || arm.exit.has_value();
    BlockId merge;
    if (needsMerge) {
        merge = addBlock(region, scope, statement->location);
        for (const auto& arm : arms) {
            if (arm.exit) {
                connectJump(*arm.exit, merge);
                mGraph->regions[arm.region.value].exit = merge;
            }
        }
    }

    Terminator terminator;
    terminator.kind = TerminatorKind::Switch;
    terminator.location = statement->location;
    terminator.operand = std::move(statement->scrutinee);
    terminator.switchType = statement->matchedType;
    terminator.primary.target = invalid;
    for (auto& arm : arms)
        terminator.cases.push_back(std::move(arm.edge));
    mGraph->blocks[current.block.value].terminator = std::move(terminator);
    return needsMerge
        ? std::optional<OpenBlock>(OpenBlock{merge, {}})
        : std::nullopt;
}

bool ControlFlowBuilder::bindExpr(Expr* expression) {
    if (!expression) return true;
    if (auto* identifier = dynamic_cast<IdentifierExpr*>(expression)) {
        if (identifier->declaration.empty()) {
            identifier->local = lookupLocal(identifier->name);
            if (identifier->local.empty()) {
                error(identifier->location,
                      "identifier '" + identifier->name +
                          "' has no canonical local or declaration reference");
                return false;
            }
        }
        return true;
    }
    if (auto* binary = dynamic_cast<BinaryExpr*>(expression))
        return bindExpr(binary->lhs.get()) && bindExpr(binary->rhs.get());
    if (auto* unary = dynamic_cast<UnaryExpr*>(expression))
        return bindExpr(unary->operand.get());
    if (auto* call = dynamic_cast<CallExpr*>(expression)) {
        if (call->iteratorOp != IteratorOp::None &&
            !mBindingIteratorRecipe) {
            error(call->location,
                  "iterator recipe must be expanded before entering a CFG operation");
            return false;
        }
        if (call->iteratorOp != IteratorOp::None) {
            if (auto* member = dynamic_cast<FieldAccessExpr*>(call->callee.get())) {
                if (!bindExpr(member->object.get())) return false;
            }
        } else if (!bindExpr(call->callee.get())) {
            return false;
        }
        for (auto& argument : call->args)
            if (!bindExpr(argument.get())) return false;
        return true;
    }
    if (auto* selection = dynamic_cast<DynamicSelectExpr*>(expression)) {
        for (auto& argument : selection->filterArguments)
            if (!bindExpr(argument.get())) return false;
        return true;
    }
    if (auto* launch = dynamic_cast<LaunchExpr*>(expression)) {
        if (!bindExpr(launch->threads.get())) return false;
        for (auto& argument : launch->args)
            if (!bindExpr(argument.get())) return false;
        return true;
    }
    if (auto* variant = dynamic_cast<VariantConstructExpr*>(expression)) {
        for (auto& argument : variant->args)
            if (!bindExpr(argument.get())) return false;
        return true;
    }
    if (auto* result = dynamic_cast<ResultConstructExpr*>(expression))
        return bindExpr(result->payload.get());
    if (auto* field = dynamic_cast<FieldAccessExpr*>(expression))
        return bindExpr(field->object.get());
    if (auto* index = dynamic_cast<IndexExpr*>(expression))
        return bindExpr(index->object.get()) && bindExpr(index->index.get());
    if (auto* length = dynamic_cast<SliceLengthExpr*>(expression))
        return bindExpr(length->slice.get());
    if (auto* array = dynamic_cast<ArrayLiteralExpr*>(expression)) {
        for (auto& element : array->elements)
            if (!bindExpr(element.get())) return false;
        return true;
    }
    if (auto* record = dynamic_cast<RecordLiteralExpr*>(expression)) {
        for (auto& field : record->fields)
            if (!bindExpr(field.value.get())) return false;
        return true;
    }
    if (auto* allocation = dynamic_cast<HeapAllocExpr*>(expression))
        return bindExpr(allocation->initializer.get());
    if (auto* lambda = dynamic_cast<LambdaExpr*>(expression)) {
        if (!lambda->captures.empty()) {
            error(lambda->location,
                  "capturing lambda requires a closure environment ABI");
            return false;
        }
        if (!lambda->body || lambda->controlFlow) {
            error(lambda->location,
                  "lambda entering CFG construction must have exactly one structured body");
            return false;
        }
        ControlFlowBuilder nestedBuilder;
        auto graph = nestedBuilder.build(
            std::move(lambda->body), lambda->params,
            RegionKind::Lambda, *mModule);
        if (!graph) {
            for (const auto& nestedError : nestedBuilder.errors())
                mErrors.push_back("lambda CFG: " + nestedError);
            return false;
        }
        lambda->controlFlow = std::move(graph);
        return true;
    }
    if (dynamic_cast<TryExpr*>(expression) ||
        dynamic_cast<BlockExpr*>(expression) ||
        dynamic_cast<IfExpr*>(expression)) {
        error(expression->location,
              "control-flow expression must be normalized in a later CFG subphase");
        return false;
    }
    if (auto* move = dynamic_cast<MoveExpr*>(expression))
        return bindExpr(move->operand.get());
    if (auto* borrow = dynamic_cast<BorrowExpr*>(expression))
        return bindExpr(borrow->operand.get());
    if (auto* dereference = dynamic_cast<DerefExpr*>(expression))
        return bindExpr(dereference->operand.get());
    if (auto* address = dynamic_cast<AddrOfExpr*>(expression))
        return bindExpr(address->operand.get());
    if (auto* assignment = dynamic_cast<AssignExpr*>(expression))
        return bindExpr(assignment->lhs.get()) &&
               bindExpr(assignment->rhs.get());
    return true;
}

LocalId ControlFlowBuilder::lookupLocal(const std::string& name) const {
    for (size_t depth = mBindings.size(); depth > 0; --depth) {
        const auto& bindings = mBindings[depth - 1];
        if (auto found = bindings.find(name); found != bindings.end())
            return found->second;
        if (depth <= mMaterializedIterators.size() &&
            mMaterializedIterators[depth - 1].count(name))
            return {};
    }
    return {};
}

const ControlFlowBuilder::MaterializedIteratorRecipe*
ControlFlowBuilder::lookupMaterializedIterator(
    const std::string& name) const {
    for (size_t depth = mMaterializedIterators.size(); depth > 0; --depth) {
        if (depth <= mBindings.size() &&
            mBindings[depth - 1].count(name))
            return nullptr;
        const auto& recipes = mMaterializedIterators[depth - 1];
        if (auto found = recipes.find(name); found != recipes.end())
            return &found->second;
    }
    return nullptr;
}

void ControlFlowBuilder::pushBindings() {
    mBindings.emplace_back();
    mMaterializedIterators.emplace_back();
}

void ControlFlowBuilder::popBindings() {
    if (!mBindings.empty()) mBindings.pop_back();
    if (!mMaterializedIterators.empty()) mMaterializedIterators.pop_back();
}

void ControlFlowBuilder::connectJump(
    const OpenBlock& source, BlockId target) {
    auto& terminator = mGraph->blocks[source.block.value].terminator;
    if (terminator.kind != TerminatorKind::Invalid) {
        error(terminator.location, "canonical block is terminated more than once");
        return;
    }
    terminator.kind = TerminatorKind::Jump;
    terminator.primary.target = target;
    terminator.primary.cleanups = canonicalCleanupOrder(
        source.cleanups, mGraph->blocks[source.block.value].scope,
        mGraph->blocks[target.value].scope);
}

std::vector<CleanupId> ControlFlowBuilder::lowerCleanupObligations(
    const std::vector<CleanupObligation>& obligations,
    ScopeId sourceScope) {
    std::vector<CleanupId> result;
    for (const auto& obligation : obligations) {
        const LocalId local = lookupLocal(obligation.place);
        if (local.empty()) {
            error({}, "cleanup for '" + obligation.place +
                      "' has no canonical local");
            continue;
        }
        const TypeRef type = obligation.typeId.empty()
            ? mGraph->locals[local.value].type : obligation.typeId;
        const CleanupId cleanup = addCleanup(
            local, type, obligation.action);
        if (!cleanup.empty()) result.push_back(cleanup);
    }
    return canonicalCleanupOrder(result, sourceScope, std::nullopt);
}

std::vector<CleanupId> ControlFlowBuilder::canonicalCleanupOrder(
    const std::vector<CleanupId>& active, ScopeId sourceScope,
    std::optional<ScopeId> targetScope) const {
    std::unordered_set<uint32_t> activeSet;
    for (const auto id : active)
        if (!id.empty()) activeSet.insert(id.value);
    std::unordered_set<uint32_t> targetAncestors;
    if (targetScope) {
        for (const ScopeRecord* scope = mGraph->findScope(*targetScope); scope;
             scope = mGraph->findScope(scope->parent))
            targetAncestors.insert(scope->id.value);
    }
    std::vector<CleanupId> result;
    for (const ScopeRecord* scope = mGraph->findScope(sourceScope); scope;
         scope = mGraph->findScope(scope->parent)) {
        if (targetAncestors.count(scope->id.value)) break;
        for (auto local = scope->locals.rbegin(); local != scope->locals.rend(); ++local) {
            auto cleanup = mCleanupByLocal.find(local->value);
            if (cleanup != mCleanupByLocal.end() &&
                activeSet.count(cleanup->second.value))
                result.push_back(cleanup->second);
        }
    }
    return result;
}

void ControlFlowBuilder::canonicalizeCleanupTable() {
    std::vector<uint32_t> order(mGraph->cleanups.size());
    for (uint32_t index = 0; index < order.size(); ++index) order[index] = index;
    std::sort(order.begin(), order.end(), [this](uint32_t lhs, uint32_t rhs) {
        const auto& left = mGraph->cleanups[lhs];
        const auto& right = mGraph->cleanups[rhs];
        if (left.scope != right.scope) return left.scope.value < right.scope.value;
        return left.place.root.value < right.place.root.value;
    });
    std::vector<uint32_t> remap(order.size());
    std::vector<CleanupRecord> canonical;
    canonical.reserve(order.size());
    for (uint32_t newIndex = 0; newIndex < order.size(); ++newIndex) {
        remap[order[newIndex]] = newIndex;
        auto record = std::move(mGraph->cleanups[order[newIndex]]);
        record.id = CleanupId{newIndex};
        canonical.push_back(std::move(record));
    }
    mGraph->cleanups = std::move(canonical);
    const auto rewrite = [&remap](std::vector<CleanupId>& references) {
        for (auto& reference : references)
            if (!reference.empty() && reference.value < remap.size())
                reference.value = remap[reference.value];
    };
    for (auto& scope : mGraph->scopes) {
        rewrite(scope.cleanups);
        std::sort(scope.cleanups.begin(), scope.cleanups.end(),
                  [this](CleanupId lhs, CleanupId rhs) {
            return mGraph->cleanups[lhs.value].place.root.value <
                   mGraph->cleanups[rhs.value].place.root.value;
        });
    }
    for (auto& block : mGraph->blocks) {
        rewrite(block.terminator.primary.cleanups);
        rewrite(block.terminator.secondary.cleanups);
        rewrite(block.terminator.exitCleanups);
        for (auto& item : block.terminator.cases)
            rewrite(item.edge.cleanups);
    }
}

void ControlFlowBuilder::error(
    const SourceLocation& location, const std::string& message) {
    if (location.path.empty()) {
        mErrors.push_back(message);
        return;
    }
    mErrors.push_back(location.path + ":" + std::to_string(location.line) +
                      ":" + std::to_string(location.column) + ": " + message);
}

} // namespace moon
