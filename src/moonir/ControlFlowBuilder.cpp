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
    mCleanupByLocal.clear();
    mBindingIteratorRecipe = false;
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

    mBindings.emplace_back();
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
    mBindings.pop_back();

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
    if (mBindings.back().count(name)) {
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
    mBindings.emplace_back();
    result.exit = lowerSequence(
        block->stmts, OpenBlock{result.entry, {}},
        result.region, result.scope);
    mBindings.pop_back();
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
        if (!bindExpr(declaration->initializer.get())) return std::nullopt;
        declaration->local = addLocal(
            scope, LocalKind::Binding, declaration->name,
            declaration->type, declaration->usage);
        mGraph->blocks[current.block.value].operations.push_back(
            std::move(statement));
        return current;
    }
    if (auto* expression = dynamic_cast<ExprStmt*>(statement.get())) {
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
        if (!bindExpr(returned->value.get())) return std::nullopt;
        auto cleanups = lowerCleanupObligations(returned->cleanups, scope);
        cleanups.insert(cleanups.end(), current.cleanups.begin(),
                        current.cleanups.end());
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
    const BlockId condition = addBlock(
        loopRegion, loopScope, statement->location);
    connectJump(current, condition);

    mBindings.emplace_back();
    if (!bindExpr(statement->cond.get())) {
        mBindings.pop_back();
        return std::nullopt;
    }
    auto body = lowerNestedBlock(
        std::move(statement->body), loopRegion, loopScope,
        RegionKind::Lexical);
    mBindings.pop_back();

    const BlockId exit = addBlock(region, scope, statement->location);
    auto& terminator = mGraph->blocks[condition.value].terminator;
    terminator.kind = TerminatorKind::Branch;
    terminator.location = statement->location;
    terminator.operand = std::move(statement->cond);
    terminator.primary.target = body.entry;
    terminator.secondary.target = exit;
    if (body.exit) connectJump(*body.exit, condition);
    mGraph->regions[body.region.value].exit = condition;
    mGraph->regions[loopRegion.value].exit = exit;
    return OpenBlock{exit, {}};
}

std::optional<ControlFlowBuilder::OpenBlock> ControlFlowBuilder::lowerFor(
    std::unique_ptr<ForStmt> statement, OpenBlock current,
    RegionId region, ScopeId scope) {
    const bool previousRecipeAllowance = mBindingIteratorRecipe;
    mBindingIteratorRecipe = statement->protocolNext.empty();
    const bool bound = bindExpr(statement->iterable.get());
    mBindingIteratorRecipe = previousRecipeAllowance;
    if (!bound) return std::nullopt;
    if (statement->protocolNext.empty())
        return lowerIteratorRecipeFor(
            std::move(statement), std::move(current), region, scope);

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
    mBindings.emplace_back();

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
            mBindings.pop_back();
            return std::nullopt;
        }
        const bool cleanupRequired =
            iteratorType->sysmeta.resource.cleanupRequired;
        if (statement->protocolStateNeedsCleanup != cleanupRequired ||
            (cleanupRequired && statement->protocolStateCleanup !=
                iteratorType->sysmeta.resource.cleanup)) {
            error(statement->location,
                  "for-loop hidden iterator state disagrees with frozen cleanup facts");
            mBindings.pop_back();
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
            mBindings.pop_back();
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
    mBindings.emplace_back();
    const LocalId itemLocal = addLocal(
        bodyScope, LocalKind::Pattern, statement->varName,
        statement->elementType, statement->bindingUsage);
    auto bodyExit = statement->body
        ? lowerSequence(statement->body->stmts, OpenBlock{bodyEntry, {}},
                        bodyRegion, bodyScope)
        : std::optional<OpenBlock>{};
    if (!statement->body)
        error(statement->location, "for-loop has no canonical body");
    mBindings.pop_back();

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
    mBindings.pop_back();
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
            IteratorOp::Take, std::move(argument), inputType, outputType});
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
        auto* lambda = dynamic_cast<LambdaExpr*>(callable.get());
        const auto* closure = mModule->findType(callable->type);
        TypeRef expectedResult = outputType;
        if (call->iteratorOp == IteratorOp::Filter)
            for (const auto& type : mModule->typeTable)
                if (type.kind == TypeKind::Bool) expectedResult = type.id;
        if ((lambda &&
             (lambda->body || !lambda->controlFlow ||
              !lambda->captures.empty() ||
              callable->type != lambda->closureType)) ||
            !closure || closure->kind != TypeKind::Function ||
            closure->parameterTypeIds != TypeRefVec{inputType} ||
            closure->returnTypeId != expectedResult ||
            (call->iteratorOp == IteratorOp::Filter &&
             outputType != inputType)) {
            error(location,
                  "iterator map/filter requires one canonical capture-free callable");
            return false;
        }
        plan.steps.push_back({call->iteratorOp, std::move(callable),
                              inputType, outputType});
        plan.itemType = outputType;
        return true;
    }
    error(location, "unsupported compiler iterator recipe operation");
    return false;
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

    TypeRef boolType;
    TypeRef indexType;
    for (const auto& type : mModule->typeTable) {
        if (type.kind == TypeKind::Bool) boolType = type.id;
        if (type.kind == TypeKind::I32) indexType = type.id;
    }
    if (boolType.empty() || indexType.empty()) {
        error(statement->location,
              "iterator recipe requires frozen bool and i32 compiler types");
        return std::nullopt;
    }
    if (plan.mode == IteratorMode::Range) {
        if (!plan.rangeStart || !plan.rangeEnd ||
            plan.rangeStart->type != indexType ||
            plan.rangeEnd->type != indexType) {
            error(statement->location,
                  "range recipe must be normalized to i32 start/end/item values");
            return std::nullopt;
        }
    } else {
        const auto* sourceType = mModule->findType(plan.sourceType);
        if (!sourceType || sourceType->kind == TypeKind::Slice) {
            error(statement->location,
                  "slice recipes require a canonical length projection in the next subphase");
            return std::nullopt;
        }
        if (sourceType->kind != TypeKind::Array ||
            sourceType->innerTypeId.empty()) {
            error(statement->location,
                  "iterator recipe source is not a frozen array");
            return std::nullopt;
        }
        const auto* elementType = mModule->findType(sourceType->innerTypeId);
        if (plan.mode == IteratorMode::Consuming &&
            (!elementType || elementType->sysmeta.resource.usage !=
                luna::ownership::Usage::Copy)) {
            error(statement->location,
                  "move-only consuming arrays require projected canonical cleanup state");
            return std::nullopt;
        }
    }
    const TypeRef sourceItemType = plan.steps.empty()
        ? plan.itemType : plan.steps.front().inputType;
    if (plan.mode == IteratorMode::Range) {
        if (sourceItemType != indexType) {
            error(statement->location,
                  "range recipe adapter input is not canonical i32");
            return std::nullopt;
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
            error(statement->location,
                  "iterator recipe source mode disagrees with its first item type");
            return std::nullopt;
        }
    }
    if (plan.itemType != statement->elementType) {
        error(statement->location,
              "iterator recipe item type disagrees with its for binding");
        return std::nullopt;
    }
    for (const auto& step : plan.steps) {
        if (step.op == IteratorOp::Take) {
            if (!step.argument || step.argument->type != indexType) {
                error(statement->location,
                      "iterator take count must be normalized to i32");
                return std::nullopt;
            }
            continue;
        }
        if (step.op != IteratorOp::Map && step.op != IteratorOp::Filter) {
            error(statement->location,
                  "iterator recipe contains an unsupported canonical adapter");
            return std::nullopt;
        }
        const auto* input = mModule->findType(step.inputType);
        const auto* output = mModule->findType(step.outputType);
        const auto* callable = step.argument
            ? mModule->findType(step.argument->type) : nullptr;
        if (!step.argument || !input || !output ||
            !callable || callable->kind != TypeKind::Function ||
            callable->parameterContracts.size() != 1 ||
            callable->parameterContracts.front().usage !=
                luna::ownership::Usage::Copy ||
            callable->returnContract.usage !=
                luna::ownership::Usage::Copy ||
            input->sysmeta.resource.usage !=
                luna::ownership::Usage::Copy ||
            output->sysmeta.resource.usage !=
                luna::ownership::Usage::Copy) {
            error(statement->location,
                  "non-Copy map/filter item or callable contract requires "
                  "canonical per-item ownership state");
            return std::nullopt;
        }
    }

    const RegionId loopRegion = addRegion(
        region, RegionKind::Loop, statement->location);
    const ScopeId loopScope = addScope(
        scope, loopRegion, statement->location);
    const BlockId init = addBlock(
        loopRegion, loopScope, statement->location);
    mBindings.emplace_back();
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
    const auto integer = [&statement, &indexType](int64_t value) {
        auto literal = std::make_unique<IntLiteralExpr>();
        literal->location = statement->location;
        literal->value = value;
        literal->type = indexType;
        return literal;
    };

    LocalId sourceLocal;
    const TypeRecord* sourceType = nullptr;
    if (plan.mode != IteratorMode::Range) {
        sourceType = mModule->findType(plan.sourceType);
        auto* sourceIdentifier = dynamic_cast<IdentifierExpr*>(plan.source.get());
        const bool snapshot = plan.mode == IteratorMode::Consuming;
        if (!snapshot && sourceIdentifier && !sourceIdentifier->local.empty()) {
            sourceLocal = sourceIdentifier->local;
            plan.source.reset();
        } else {
            const auto* frozen = mModule->findType(plan.sourceType);
            if (!frozen || frozen->sysmeta.resource.cleanupRequired) {
                error(statement->location,
                      "temporary iterator source requires unsupported synthetic cleanup");
                mBindings.pop_back();
                return std::nullopt;
            }
            sourceLocal = addBinding(
                "$for.source." + identity, plan.sourceType,
                std::move(plan.source));
        }
    }

    std::unique_ptr<Expr> initial;
    std::unique_ptr<Expr> limit;
    if (plan.mode == IteratorMode::Range) {
        initial = std::move(plan.rangeStart);
        limit = std::move(plan.rangeEnd);
    } else {
        initial = integer(0);
        limit = integer(static_cast<int64_t>(sourceType->arrayLength));
    }
    const LocalId indexLocal = addBinding(
        "$for.index." + identity, indexType, std::move(initial));
    const LocalId limitLocal = addBinding(
        "$for.limit." + identity, indexType, std::move(limit));
    std::vector<LocalId> adapterLocals;
    for (size_t stepIndex = 0; stepIndex < plan.steps.size(); ++stepIndex) {
        auto& step = plan.steps[stepIndex];
        const char* adapterName = step.op == IteratorOp::Map
            ? "map" : (step.op == IteratorOp::Filter ? "filter" : "take");
        const TypeRef adapterType = step.op == IteratorOp::Take
            ? indexType : step.argument->type;
        adapterLocals.push_back(addBinding(
            "$for." + std::string(adapterName) + "." + identity + "." +
                std::to_string(stepIndex),
            adapterType, std::move(step.argument)));
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

    mBindings.emplace_back();
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
            canTake->rhs = integer(0);
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
            assignment->rhs = integer(1);
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
    mBindings.pop_back();

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
        assignment->rhs = integer(1);
        assignment->type = indexType;
        advance->expr = std::move(assignment);
        mGraph->blocks[increment.value].operations.push_back(
            std::move(advance));
        connectJump(OpenBlock{increment, {}}, condition);
        mGraph->regions[bodyRegion.value].exit = increment;
    }
    mGraph->regions[loopRegion.value].exit = exit;
    mBindings.pop_back();
    return OpenBlock{exit, {}};
}

std::optional<ControlFlowBuilder::OpenBlock> ControlFlowBuilder::lowerMatch(
    std::unique_ptr<MatchStmt> statement, OpenBlock current,
    RegionId region, ScopeId scope) {
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
        mBindings.emplace_back();
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
        mBindings.pop_back();
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
    if (auto* field = dynamic_cast<FieldAccessExpr*>(expression))
        return bindExpr(field->object.get());
    if (auto* index = dynamic_cast<IndexExpr*>(expression))
        return bindExpr(index->object.get()) && bindExpr(index->index.get());
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
    for (auto scope = mBindings.rbegin(); scope != mBindings.rend(); ++scope) {
        auto found = scope->find(name);
        if (found != scope->end()) return found->second;
    }
    return {};
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
