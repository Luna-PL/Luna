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
                 parameter.type, parameter.usage);

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
    const TypeRef& type, luna::ownership::Usage usage) {
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
    if (const auto* frozen = mModule->findType(type))
        record.relation = frozen->sysmeta.resource.relation;
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
        if (!bindExpr(call->callee.get())) return false;
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
    if (dynamic_cast<TryExpr*>(expression) ||
        dynamic_cast<BlockExpr*>(expression) ||
        dynamic_cast<IfExpr*>(expression) ||
        dynamic_cast<LambdaExpr*>(expression)) {
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
