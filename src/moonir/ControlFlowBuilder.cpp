#include "ControlFlowBuilder.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <type_traits>
#include <unordered_set>

namespace moon {

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
    std::optional<luna::ownership::Relation> relation,
    bool inferTypeCleanup) {
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
        inferTypeCleanup && frozen &&
        mGraph->locals[id.value].relation ==
            luna::ownership::Relation::Owned &&
        frozen->sysmeta.resource.cleanupRequired)
        addCleanup(id, type, frozen->sysmeta.resource.cleanup);
    return id;
}

CleanupId ControlFlowBuilder::addCleanup(
    LocalId local, const TypeRef& type,
    luna::ownership::CleanupAction action, CleanupKind kind) {
    if (local.empty() || local.value >= mGraph->locals.size()) {
        error({}, "cleanup references an unresolved canonical local");
        return {};
    }
    if (auto found = mCleanupByLocal.find(local.value);
        found != mCleanupByLocal.end()) {
        const auto& existing = mGraph->cleanups[found->second.value];
        if (existing.type != type || existing.action != action ||
            existing.kind != kind)
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
    cleanup.kind = kind;
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
            // Statements after a terminating path (return/abort/break) are
            // unreachable dead code. The structured backend silently skips
            // them; the canonical CFG must do the same rather than rejecting
            // the program.
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
            declaration->type, declaration->usage,
            declaration->relation);
        if (!declaration->local.empty())
            declaration->relation =
                mGraph->locals[declaration->local.value].relation;
        if (dynamic_cast<InitAllocationExpr*>(
                declaration->initializer.get())) {
            const auto* type = mModule->findType(declaration->type);
            if (type && !type->sysmeta.resource.cleanupRequired)
                addCleanup(
                    declaration->local, declaration->type,
                    luna::ownership::CleanupAction::Deallocate,
                    CleanupKind::Allocation);
        }
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
        const auto* expressionType = mModule->findType(
            expression->expr->type);
        const bool diverges = expressionType &&
            expressionType->kind == TypeKind::Never;
        const auto expressionLocation = expression->location;
        mGraph->blocks[current.block.value].operations.push_back(
            std::move(statement));
        if (diverges) {
            auto& terminator =
                mGraph->blocks[current.block.value].terminator;
            terminator.kind = TerminatorKind::Unreachable;
            terminator.location = expressionLocation;
            return std::nullopt;
        }
        return current;
    }
    if (auto* release = dynamic_cast<FreeStmt*>(statement.get())) {
        // An implicit scope-exit FreeStmt for a materialized iterator binding
        // fires only when the iterator was never consumed: a consumed iterator
        // is fully accounted for by its for-loop / terminal guarded tail and
        // emits no FreeStmt. Clean the owning source array as a whole via its
        // type's cleanup action, which drops every unconsumed element.
        if (release->isImplicit) {
            auto* id = dynamic_cast<IdentifierExpr*>(
                release->operand.get());
            if (id && id->declaration.empty()) {
                const auto* recipe =
                    lookupMaterializedIterator(id->name);
                if (recipe && recipe->ownsSource) {
                    if (!recipe->sourceTailCleanups.empty()) {
                        current.cleanups.insert(
                            current.cleanups.end(),
                            recipe->sourceTailCleanups.begin(),
                            recipe->sourceTailCleanups.end());
                        return current;
                    }
                    const auto* sourceType =
                        mModule->findType(recipe->sourceType);
                    if (sourceType &&
                        sourceType->sysmeta.resource.cleanupRequired) {
                        const CleanupId cleanup = addCleanup(
                            recipe->source, recipe->sourceType,
                            sourceType->sysmeta.resource.cleanup,
                            CleanupKind::Value);
                        if (!cleanup.empty())
                            current.cleanups.push_back(cleanup);
                    }
                    return current;
                }
            }
        }
        if (!bindExpr(release->operand.get())) return std::nullopt;
        if (!release->isImplicit) {
            mGraph->blocks[current.block.value].operations.push_back(
                std::move(statement));
            return current;
        }
        // After capture-read rewriting, an implicit FreeStmt for a captured
        // binding has an EnvLoadExpr operand. The captured binding's cleanup
        // is owned by the closure value (the environment parameter), not by
        // the lambda function. Redirect the cleanup to the environment
        // parameter's own cleanup obligation (C016 CL010).
        auto* envLoad = dynamic_cast<EnvLoadExpr*>(release->operand.get());
        if (envLoad) {
            if (mCaptureEnvLocal.empty())
                return current; // no environment parameter to clean
            const auto* closureType =
                mModule->findType(mGraph->locals[
                    mCaptureEnvLocal.value].type);
            if (!closureType ||
                !closureType->sysmeta.resource.cleanupRequired)
                return current;
            const CleanupId cleanup = addCleanup(
                mCaptureEnvLocal,
                mGraph->locals[mCaptureEnvLocal.value].type,
                closureType->sysmeta.resource.cleanup,
                CleanupKind::Value);
            if (!cleanup.empty()) current.cleanups.push_back(cleanup);
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
        const auto* cleanupType = mModule->findType(local.type);
        const CleanupKind cleanupKind =
            cleanupType && !cleanupType->sysmeta.resource.cleanupRequired &&
                release->action ==
                    luna::ownership::CleanupAction::Deallocate
            ? CleanupKind::Allocation
            : CleanupKind::Value;
        const CleanupId cleanup = addCleanup(
            identifier->local, local.type, release->action,
            cleanupKind);
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
        // The closure environment parameter is an owned local whose cleanup
        // must run on every exit edge, including return. It is not in the
        // structured return-cleanup list because the OwnershipChecker does not
        // know about the synthetic environment parameter (C016 CL010).
        if (!mCaptureEnvLocal.empty()) {
            const auto* closureType = mModule->findType(
                mGraph->locals[mCaptureEnvLocal.value].type);
            if (closureType &&
                closureType->sysmeta.resource.cleanupRequired) {
                const CleanupId envCleanup = addCleanup(
                    mCaptureEnvLocal,
                    mGraph->locals[mCaptureEnvLocal.value].type,
                    closureType->sysmeta.resource.cleanup,
                    CleanupKind::Value);
                if (!envCleanup.empty()) cleanups.push_back(envCleanup);
            }
        }
        cleanups.insert(cleanups.end(), current.cleanups.begin(),
                        current.cleanups.end());
        cleanups.insert(cleanups.end(),
                        mActiveExpressionCleanups.begin(),
                        mActiveExpressionCleanups.end());
        auto& terminator = mGraph->blocks[current.block.value].terminator;
        if (!mFragmentContexts.empty()) {
            if (returned->value) {
                error(returned->location,
                      "0.3 fragment return must not carry a value");
                return std::nullopt;
            }
            const BlockId exit = mFragmentContexts.back().exit;
            terminator.kind = TerminatorKind::Jump;
            terminator.location = returned->location;
            terminator.primary.target = exit;
            terminator.primary.cleanups = canonicalCleanupOrder(
                cleanups, scope, mGraph->blocks[exit.value].scope);
            return std::nullopt;
        }
        terminator.kind = TerminatorKind::Return;
        terminator.location = returned->location;
        terminator.operand = std::move(returned->value);
        terminator.exitCleanups = canonicalCleanupOrder(
            cleanups, scope, std::nullopt);
        return std::nullopt;
    }
    if (auto* slot = dynamic_cast<SlotDeclStmt*>(statement.get())) {
        if (!slot->defaultFragmentRef.empty())
            mSlotDefaults.back()[slot->name] = slot->defaultFragmentRef;
        return current;
    }
    if (dynamic_cast<SlotInvokeStmt*>(statement.get())) {
        std::unique_ptr<SlotInvokeStmt> owned(
            static_cast<SlotInvokeStmt*>(statement.release()));
        return lowerSlotInvoke(
            std::move(owned), std::move(current), region, scope);
    }
    if (dynamic_cast<ApplyStmt*>(statement.get())) {
        std::unique_ptr<ApplyStmt> owned(
            static_cast<ApplyStmt*>(statement.release()));
        return lowerApply(
            std::move(owned), std::move(current), region, scope);
    }
    if (auto* abort = dynamic_cast<AbortStmt*>(statement.get())) {
        if (mFragmentContexts.empty()) {
            error(abort->location,
                  "abort() has no active canonical fragment boundary");
            return std::nullopt;
        }
        auto cleanups = lowerCleanupObligations(abort->cleanups, scope);
        cleanups.insert(cleanups.end(), current.cleanups.begin(),
                        current.cleanups.end());
        cleanups.insert(cleanups.end(),
                        mActiveExpressionCleanups.begin(),
                        mActiveExpressionCleanups.end());
        const BlockId exit = mFragmentContexts.back().exit;
        auto& terminator = mGraph->blocks[current.block.value].terminator;
        terminator.kind = TerminatorKind::Abort;
        terminator.location = abort->location;
        terminator.primary.target = exit;
        terminator.primary.cleanups = canonicalCleanupOrder(
            cleanups, scope, mGraph->blocks[exit.value].scope);
        return std::nullopt;
    }
    if (dynamic_cast<ResumeStmt*>(statement.get())) {
        std::unique_ptr<ResumeStmt> owned(
            static_cast<ResumeStmt*>(statement.release()));
        return lowerResume(
            std::move(owned), std::move(current), region, scope);
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
        state->relation = mGraph->locals[stateLocal.value].relation;
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

LocalId ControlFlowBuilder::lookupCleanupLocal(
    const std::string& name, size_t shadowOrdinal) const {
    const auto consider = [&name, &shadowOrdinal](const auto& bindings)
        -> LocalId {
        auto found = bindings.find(name);
        if (found == bindings.end()) return {};
        if (shadowOrdinal == 0) return found->second;
        --shadowOrdinal;
        return {};
    };

    // Hidden fragment bindings are inserted at their recorded outer depth:
    // continuation scopes are more nested, invoking scopes less nested.
    for (size_t depth = mBindings.size();;) {
        for (size_t groupIndex = mHiddenCleanupBindingGroups.size();
             groupIndex > 0; --groupIndex) {
            const auto& group =
                mHiddenCleanupBindingGroups[groupIndex - 1];
            if (group.outerBindingDepth != depth) continue;
            for (size_t bindingIndex = group.bindings.size();
                 bindingIndex > 0; --bindingIndex) {
                const LocalId local = consider(
                    group.bindings[bindingIndex - 1]);
                if (!local.empty()) return local;
            }
        }
        if (depth == 0) break;
        const LocalId local = consider(mBindings[depth - 1]);
        if (!local.empty()) return local;
        --depth;
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
    mSlotDefaults.emplace_back();
}

void ControlFlowBuilder::popBindings() {
    if (!mBindings.empty()) mBindings.pop_back();
    if (!mMaterializedIterators.empty()) mMaterializedIterators.pop_back();
    if (!mSlotDefaults.empty()) mSlotDefaults.pop_back();
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


} // namespace moon
