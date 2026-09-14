#include "ControlFlowBuilder.h"
#include "ControlFlowBuilderCloneInternal.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <type_traits>
#include <unordered_set>

namespace moon {

using namespace control_flow_builder_detail;

std::unique_ptr<ControlFlowGraph> ControlFlowBuilder::build(
    const BlockStmt& root,
    const std::vector<Param>& parameters,
    RegionKind rootKind,
    const Module& module) {
    resetStructureDepthLimit();
    auto cloned = cloneStructuredBlock(&root);
    if (!cloned) {
        error(root.location,
              structureDepthExceeded()
                  ? "structured body exceeds the maximum nesting depth"
                  : "cannot clone the structured body");
        return nullptr;
    }
    return build(
        std::move(cloned), parameters, rootKind, module);
}

std::unique_ptr<ControlFlowGraph> ControlFlowBuilder::build(
    std::unique_ptr<BlockStmt> root,
    const std::vector<Param>& parameters,
    RegionKind rootKind,
    const Module& module) {
    resetStructureDepthLimit();
    mErrors.clear();
    mBindings.clear();
    mHiddenCleanupBindingGroups.clear();
    mMaterializedIterators.clear();
    mSlotDefaults.clear();
    mStaticApplyScopes.clear();
    mFragmentContexts.clear();
    mCleanupByLocal.clear();
    mActiveExpressionCleanups.clear();
    mGuardedConsumingRecipeNames.clear();
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
    if (!mCaptureEnvParamName.empty()) {
        // The environment parameter's usage must match the closure type's
        // inherent usage. A closure with Non-Copy captured fields is Affine,
        // and the environment parameter cannot weaken that (C016 CL010).
        luna::ownership::Usage envUsage = luna::ownership::Usage::Copy;
        if (const auto* closureType = mModule->findType(mCaptureClosureType))
            envUsage = closureType->sysmeta.resource.usage;
        mCaptureEnvLocal = addLocal(
            rootScope, LocalKind::Parameter, mCaptureEnvParamName,
            mCaptureClosureType, envUsage,
            luna::ownership::Relation::Owned);
        if (mCaptureEnvLocal.empty()) {
            mGraph = nullptr;
            mModule = nullptr;
            return nullptr;
        }
    }
    for (const auto& parameter : parameters) {
        auto usage = parameter.usage;
        auto relation = parameter.relation;
        // A trait-method instance may carry a Copy usage for an Owned
        // parameter whose frozen type requires Affine/Linear. Strengthen
        // the parameter usage to meet the frozen type's requirement so the
        // canonical verifier's usage check does not reject a valid program.
        if (relation != luna::ownership::Relation::SharedBorrow &&
            relation != luna::ownership::Relation::MutableBorrow) {
            if (const auto* frozen = mModule->findType(parameter.type)) {
                if (!luna::ownership::satisfiesUsageRequirement(
                        usage, frozen->sysmeta.resource.usage))
                    usage = frozen->sysmeta.resource.usage;
            }
        }
        addLocal(rootScope, LocalKind::Parameter, parameter.name,
                 parameter.type, usage, relation);
    }
    if (!mCaptureEnvParamName.empty()) {
        for (auto& statement : root->stmts)
            statement = rewriteCaptureReadsStmt(
                std::move(statement), mCaptureNames, mCaptureEnvLocal,
                mCaptureClosureType, module);
        if (structureDepthExceeded()) {
            error(root->location,
                  "lambda capture rewrite exceeds the maximum nesting depth");
            mGraph = nullptr;
            mModule = nullptr;
            return nullptr;
        }
    }

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

std::optional<ControlFlowBuilder::OpenBlock> ControlFlowBuilder::lowerApply(
    std::unique_ptr<ApplyStmt> statement, OpenBlock current,
    RegionId region, ScopeId scope) {
    if (!resolveFragment(statement->fragmentRef)) {
        error(statement->location,
              "static apply references a missing canonical fragment");
        return std::nullopt;
    }

    // A blockless node can only arrive from independently constructed legacy
    // structured IR; retain the ordinary static lexical binding behavior.
    if (!statement->body) {
        mSlotDefaults.back()[statement->slotName] = statement->fragmentRef;
        return current;
    }

    mStaticApplyScopes.emplace_back();
    mStaticApplyScopes.back()[statement->slotName] = statement->fragmentRef;
    auto body = lowerNestedBlock(
        std::move(statement->body), region, scope, RegionKind::Apply);
    mStaticApplyScopes.pop_back();
    connectJump(current, body.entry);
    if (!body.exit) return std::nullopt;

    const BlockId continuation = addBlock(
        region, scope,
        mGraph->blocks[body.exit->block.value].location);
    connectJump(*body.exit, continuation);
    mGraph->regions[body.region.value].exit = continuation;
    return OpenBlock{continuation, {}};
}

const FragmentDecl* ControlFlowBuilder::resolveFragment(
    const DeclarationRef& reference) const {
    if (!mModule || !reference.complete()) return nullptr;
    const auto* record = mModule->findDeclaration(reference);
    if (!record || record->kind != DeclarationKind::Fragment) return nullptr;
    for (const auto& declaration : mModule->declarations) {
        const auto* fragment = dynamic_cast<const FragmentDecl*>(
            declaration.get());
        if (fragment && fragment->symbolId == reference.symbol &&
            fragment->contractId == reference.contract)
            return fragment;
    }
    return nullptr;
}

std::optional<ControlFlowBuilder::OpenBlock>
ControlFlowBuilder::lowerSlotInvoke(
    std::unique_ptr<SlotInvokeStmt> statement, OpenBlock current,
    RegionId region, ScopeId scope) {
    if (!statement->continuation) {
        error(statement->location,
              "slot invocation has no lexical continuation body");
        return std::nullopt;
    }

    DeclarationRef fragmentReference;
    bool boundByStaticApply = false;
    for (size_t depth = mStaticApplyScopes.size(); depth > 0; --depth) {
        const auto& bindings = mStaticApplyScopes[depth - 1];
        if (auto found = bindings.find(statement->name);
            found != bindings.end()) {
            fragmentReference = found->second;
            boundByStaticApply = true;
            break;
        }
    }
    if (fragmentReference.empty())
        fragmentReference = statement->defaultFragmentRef;
    if (fragmentReference.empty()) {
        for (size_t depth = mSlotDefaults.size(); depth > 0; --depth) {
            const auto& defaults = mSlotDefaults[depth - 1];
            if (auto found = defaults.find(statement->name);
                found != defaults.end()) {
                fragmentReference = found->second;
                break;
            }
        }
    }

    const auto buildUnmodifiedContinuation = [&]()
        -> std::optional<OpenBlock> {
        auto continuation = lowerNestedBlock(
            std::move(statement->continuation), region, scope,
            RegionKind::Continuation);
        connectJump(current, continuation.entry);
        if (!continuation.exit) return std::nullopt;
        const BlockId exit = addBlock(
            region, scope,
            mGraph->blocks[continuation.exit->block.value].location);
        connectJump(*continuation.exit, exit);
        mGraph->regions[continuation.region.value].exit = exit;
        return OpenBlock{exit, {}};
    };
    if (fragmentReference.empty()) return buildUnmodifiedContinuation();

    const FragmentDecl* fragment = resolveFragment(fragmentReference);
    if (!fragment) {
        error(statement->location,
              "slot invocation references a missing canonical fragment");
        return std::nullopt;
    }
    if (fragment->kind != statement->acceptedKind ||
        fragment->cardinality != statement->acceptedCardinality) {
        error(statement->location,
              "slot invocation and fragment control contracts disagree");
        return std::nullopt;
    }
    if (!fragment->body) {
        error(statement->location,
              "static fragment has no structured construction body");
        return std::nullopt;
    }
    if (fragment->params.size() != statement->args.size() &&
        fragment->params.size() != statement->resolvedParamNames.size()) {
        error(statement->location,
              "slot invocation cannot materialize the fragment parameter contract");
        return std::nullopt;
    }
    auto fragmentBody = cloneStructuredBlock(fragment->body.get());
    if (!fragmentBody) {
        error(statement->location,
              "static fragment body cannot be cloned before canonical construction");
        return std::nullopt;
    }

    // An explicit source apply already owns the composition region. A slot
    // default has the same static control contract without source-level apply
    // syntax, so materialize its application boundary here. This keeps every
    // Fragment/Continuation pair under one independently verifiable Apply
    // region without retaining a runtime descriptor or another language
    // concept.
    RegionId invocationRegion = region;
    ScopeId invocationScope = scope;
    OpenBlock invocationCurrent = current;
    RegionId implicitApplyRegion;
    if (!boundByStaticApply) {
        implicitApplyRegion = addRegion(
            region, RegionKind::Apply, statement->location);
        invocationScope = addScope(
            scope, implicitApplyRegion, statement->location);
        const BlockId entry = addBlock(
            implicitApplyRegion, invocationScope, statement->location);
        connectJump(current, entry);
        invocationRegion = implicitApplyRegion;
        invocationCurrent = OpenBlock{entry, current.cleanups};
    }

    std::vector<std::unique_ptr<Stmt>> parameterBindings;
    parameterBindings.reserve(fragment->params.size());
    for (size_t index = 0; index < fragment->params.size(); ++index) {
        const auto& parameter = fragment->params[index];
        auto binding = std::make_unique<LetStmt>();
        binding->location = statement->location;
        binding->name = parameter.name;
        binding->isLinear =
            parameter.usage == luna::ownership::Usage::Linear;
        binding->usage = parameter.usage;
        binding->relation = parameter.relation;
        binding->type = parameter.type;
        if (index < statement->args.size()) {
            binding->initializer = std::move(statement->args[index]);
        } else {
            auto capture = std::make_unique<IdentifierExpr>();
            capture->location = statement->location;
            capture->name = statement->resolvedParamNames[index];
            capture->type = parameter.type;
            binding->initializer = std::move(capture);
        }
        parameterBindings.push_back(std::move(binding));
    }
    const BlockId invocationExit = addBlock(
        invocationRegion, invocationScope, statement->location);
    const RegionId fragmentRegion = addRegion(
        invocationRegion, RegionKind::Fragment, fragmentBody->location);
    mGraph->regions[fragmentRegion.value].fragment = fragmentReference;
    const ScopeId fragmentScope = addScope(
        invocationScope, fragmentRegion, fragmentBody->location);
    const BlockId fragmentEntry = addBlock(
        fragmentRegion, fragmentScope, fragmentBody->location);
    const size_t outerBindingDepth = mBindings.size();
    pushBindings();
    mFragmentContexts.push_back({
        invocationExit, fragment->kind, statement->continuation.get(),
        outerBindingDepth});
    std::optional<OpenBlock> fragmentOpen = OpenBlock{fragmentEntry, {}};
    for (auto& binding : parameterBindings) {
        if (!fragmentOpen) break;
        const auto* parameter = static_cast<LetStmt*>(binding.get());
        const std::string name = parameter->name;
        fragmentOpen = lowerStatement(
            std::move(binding), std::move(*fragmentOpen),
            fragmentRegion, fragmentScope);
        const LocalId local = lookupLocal(name);
        if (local.empty()) {
            error(statement->location,
                  "fragment parameter has no canonical entry binding");
            fragmentOpen = std::nullopt;
            break;
        }
        mGraph->regions[fragmentRegion.value].parameters.push_back(local);
    }
    if (fragmentOpen)
        fragmentOpen = lowerSequence(
            fragmentBody->stmts, std::move(*fragmentOpen),
            fragmentRegion, fragmentScope);
    mFragmentContexts.pop_back();
    popBindings();
    mGraph->regions[fragmentRegion.value].exit = invocationExit;
    connectJump(invocationCurrent, fragmentEntry);

    if (fragmentOpen && fragment->kind == FragmentKind::Interceptor) {
        auto continuation = lowerNestedBlock(
            std::move(statement->continuation), invocationRegion,
            invocationScope,
            RegionKind::Continuation);
        auto& terminator =
            mGraph->blocks[fragmentOpen->block.value].terminator;
        terminator.kind = TerminatorKind::Jump;
        terminator.location = statement->location;
        terminator.primary.target = continuation.entry;
        terminator.primary.cleanups = canonicalCleanupOrder(
            fragmentOpen->cleanups, fragmentScope, continuation.scope);
        if (continuation.exit)
            connectJump(*continuation.exit, invocationExit);
        mGraph->regions[continuation.region.value].exit = invocationExit;
    } else if (fragmentOpen) {
        auto& terminator =
            mGraph->blocks[fragmentOpen->block.value].terminator;
        terminator.kind = TerminatorKind::Abort;
        terminator.location = statement->location;
        terminator.primary.target = invocationExit;
        terminator.primary.cleanups = canonicalCleanupOrder(
            fragmentOpen->cleanups, fragmentScope, invocationScope);
    }
    if (implicitApplyRegion.empty())
        return OpenBlock{invocationExit, {}};

    const BlockId exit = addBlock(region, scope, statement->location);
    connectJump(OpenBlock{invocationExit, {}}, exit);
    mGraph->regions[implicitApplyRegion.value].exit = exit;
    return OpenBlock{exit, {}};
}

std::optional<ControlFlowBuilder::OpenBlock> ControlFlowBuilder::lowerResume(
    std::unique_ptr<ResumeStmt> statement, OpenBlock current,
    RegionId region, ScopeId scope) {
    if (mFragmentContexts.empty() ||
        mFragmentContexts.back().kind != FragmentKind::Context ||
        !mFragmentContexts.back().continuation) {
        error(statement->location,
              "resume() has no active canonical context continuation");
        return std::nullopt;
    }
    const FragmentContext active = mFragmentContexts.back();
    auto continuationBody = cloneStructuredBlock(active.continuation);
    if (!continuationBody) {
        error(statement->location,
              "context continuation cannot be cloned before canonical construction");
        return std::nullopt;
    }

    using BindingMap = std::unordered_map<std::string, LocalId>;
    using IteratorMap = std::unordered_map<
        std::string, MaterializedIteratorRecipe>;
    using DefaultMap = std::unordered_map<std::string, DeclarationRef>;
    std::vector<BindingMap> fragmentBindings;
    std::vector<IteratorMap> fragmentIterators;
    std::vector<DefaultMap> fragmentDefaults;
    for (size_t index = active.outerBindingDepth;
         index < mBindings.size(); ++index) {
        fragmentBindings.push_back(std::move(mBindings[index]));
        fragmentIterators.push_back(std::move(mMaterializedIterators[index]));
        fragmentDefaults.push_back(std::move(mSlotDefaults[index]));
    }
    mBindings.resize(active.outerBindingDepth);
    mMaterializedIterators.resize(active.outerBindingDepth);
    mSlotDefaults.resize(active.outerBindingDepth);
    auto fragmentContexts = std::move(mFragmentContexts);
    mFragmentContexts.clear();

    mHiddenCleanupBindingGroups.push_back({
        active.outerBindingDepth, fragmentBindings});

    auto continuation = lowerNestedBlock(
        std::move(continuationBody), region, scope,
        RegionKind::Continuation);

    mHiddenCleanupBindingGroups.pop_back();

    mFragmentContexts = std::move(fragmentContexts);
    mBindings.insert(
        mBindings.end(),
        std::make_move_iterator(fragmentBindings.begin()),
        std::make_move_iterator(fragmentBindings.end()));
    mMaterializedIterators.insert(
        mMaterializedIterators.end(),
        std::make_move_iterator(fragmentIterators.begin()),
        std::make_move_iterator(fragmentIterators.end()));
    mSlotDefaults.insert(
        mSlotDefaults.end(),
        std::make_move_iterator(fragmentDefaults.begin()),
        std::make_move_iterator(fragmentDefaults.end()));

    auto& terminator = mGraph->blocks[current.block.value].terminator;
    terminator.kind = TerminatorKind::Resume;
    terminator.location = statement->location;
    terminator.primary.target = continuation.entry;
    terminator.primary.cleanups = canonicalCleanupOrder(
        current.cleanups, scope, continuation.scope);
    if (!continuation.exit) return std::nullopt;

    const BlockId resumed = addBlock(region, scope, statement->location);
    connectJump(*continuation.exit, resumed);
    mGraph->regions[continuation.region.value].exit = resumed;
    return OpenBlock{resumed, current.cleanups};
}

bool ControlFlowBuilder::bindExpr(Expr* expression) {
    if (!expression) return true;
    if (dynamic_cast<IntLiteralExpr*>(expression) ||
        dynamic_cast<FloatLiteralExpr*>(expression) ||
        dynamic_cast<StringLiteralExpr*>(expression) ||
        dynamic_cast<BoolLiteralExpr*>(expression) ||
        dynamic_cast<UnitExpr*>(expression))
        return true;
    if (auto* identifier = dynamic_cast<IdentifierExpr*>(expression)) {
        if (identifier->local.empty()) {
            const LocalId local = lookupLocal(identifier->name);
            if (!local.empty()) {
                identifier->local = local;
                const TypeRef localType =
                    mGraph->locals[local.value].type;
                if (identifier->type.empty())
                    identifier->type = localType;
            } else if (identifier->declaration.empty()) {
                error(identifier->location,
                      "identifier '" + identifier->name +
                          "' has no canonical local or declaration reference");
                return false;
            }
        }
        return true;
    }
    if (auto* binary = dynamic_cast<BinaryExpr*>(expression)) {
        if (!bindExpr(binary->lhs.get()) ||
            !bindExpr(binary->rhs.get()))
            return false;
        if (binary->type.empty()) {
            switch (binary->op) {
                case Operator::Equal:
                case Operator::NotEqual:
                case Operator::Less:
                case Operator::LessEqual:
                case Operator::Greater:
                case Operator::GreaterEqual:
                case Operator::LogicalAnd:
                case Operator::LogicalOr:
                    for (const auto& type : mModule->typeTable)
                        if (type.kind == TypeKind::Bool) {
                            binary->type = type.id;
                            break;
                        }
                    break;
                default:
                    if (binary->lhs)
                        binary->type = binary->lhs->type;
                    break;
            }
        }
        return true;
    }
    if (auto* unary = dynamic_cast<UnaryExpr*>(expression)) {
        if (!bindExpr(unary->operand.get())) return false;
        if (unary->type.empty()) {
            if (unary->op == Operator::LogicalNot) {
                for (const auto& type : mModule->typeTable)
                    if (type.kind == TypeKind::Bool) {
                        unary->type = type.id;
                        break;
                    }
            } else if (unary->op == Operator::Dereference &&
                       unary->operand) {
                const auto* operandType = mModule->findType(
                    unary->operand->type);
                if (operandType &&
                    (operandType->kind == TypeKind::Reference ||
                     operandType->kind == TypeKind::RawPointer))
                    unary->type = operandType->innerTypeId;
            } else if (unary->operand) {
                unary->type = unary->operand->type;
            }
        }
        return true;
    }
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
        } else {
            // Compiler intrinsics (print, panic, slice, type_size, etc.)
            // have no declaration table row. Skip the local/declaration
            // lookup for their callee identifier so the canonical CFG does
            // not reject them.
            const auto isCompilerIntrinsic = [](const Expr* callee) {
                const auto* id = dynamic_cast<const IdentifierExpr*>(callee);
                if (!id || !id->declaration.empty()) return false;
                return isCompilerIntrinsicName(id->name);
            };
            if (!isCompilerIntrinsic(call->callee.get())) {
                if (!bindExpr(call->callee.get())) return false;
            }
        }
        for (auto& argument : call->args)
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
    if (auto* field = dynamic_cast<FieldAccessExpr*>(expression)) {
        if (!bindExpr(field->object.get())) return false;
        if (field->type.empty() && field->object) {
            const auto* objectType = mModule->findType(
                field->object->type);
            if (objectType && objectType->kind == TypeKind::Reference)
                objectType = mModule->findType(objectType->innerTypeId);
            if (objectType)
                for (const auto& candidate : objectType->fields)
                    if (candidate.name == field->field) {
                        field->type = candidate.type;
                        break;
                    }
        }
        return true;
    }
    if (auto* index = dynamic_cast<IndexExpr*>(expression)) {
        if (!bindExpr(index->object.get()) ||
            !bindExpr(index->index.get()))
            return false;
        if (index->type.empty() && index->object) {
            const auto* objectType = mModule->findType(
                index->object->type);
            if (objectType && objectType->kind == TypeKind::Reference)
                objectType = mModule->findType(objectType->innerTypeId);
            if (objectType &&
                (objectType->kind == TypeKind::Array ||
                 objectType->kind == TypeKind::Slice))
                index->type = objectType->innerTypeId;
        }
        return true;
    }
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
    if (auto* allocation = dynamic_cast<HeapAllocExpr*>(expression)) {
        // The HeapAllocExpr initializer is a constructor CallExpr whose
        // callee names a struct type, not a function. Bind only the
        // arguments, not the callee, so the canonical path does not reject
        // the type name as an unresolved identifier.
        if (auto* call = dynamic_cast<CallExpr*>(
                allocation->initializer.get())) {
            for (auto& arg : call->args)
                if (!bindExpr(arg.get())) return false;
        } else if (allocation->initializer) {
            return bindExpr(allocation->initializer.get());
        }
        return true;
    }
    if (auto* initialized = dynamic_cast<InitAllocationExpr*>(expression)) {
        for (auto& element : initialized->elements)
            if (!bindExpr(element.value.get())) return false;
        return true;
    }
    if (auto* closure = dynamic_cast<MakeClosureExpr*>(expression)) {
        for (auto& value : closure->capturedValues)
            if (!bindExpr(value.get())) return false;
        if (closure->lambda)
            return bindExpr(closure->lambda.get());
        error(closure->location,
              "closure construction has no lambda executable");
        return false;
    }
    if (auto* lambda = dynamic_cast<LambdaExpr*>(expression)) {
        if (!lambda->body || lambda->controlFlow) {
            error(lambda->location,
                  "lambda entering CFG construction must have exactly one structured body");
            return false;
        }
        ControlFlowBuilder nestedBuilder;
        if (!lambda->captures.empty())
            nestedBuilder.setCaptureEnvironment(
                lambda->captures, lambda->closureType,
                lambda->envParamName);
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
    if (auto* move = dynamic_cast<MoveExpr*>(expression)) {
        if (!bindExpr(move->operand.get())) return false;
        if (move->type.empty() && move->operand)
            move->type = move->operand->type;
        return true;
    }
    if (auto* borrow = dynamic_cast<BorrowExpr*>(expression)) {
        if (!bindExpr(borrow->operand.get())) return false;
        if (borrow->type.empty() && borrow->operand)
            for (const auto& type : mModule->typeTable)
                if (type.kind == TypeKind::Reference &&
                    type.isMutable == borrow->isMutable &&
                    type.innerTypeId == borrow->operand->type) {
                    borrow->type = type.id;
                    break;
                }
        return true;
    }
    if (auto* dereference = dynamic_cast<DerefExpr*>(expression)) {
        if (!bindExpr(dereference->operand.get())) return false;
        if (dereference->type.empty() && dereference->operand) {
            const auto* operandType = mModule->findType(
                dereference->operand->type);
            if (operandType &&
                (operandType->kind == TypeKind::Reference ||
                 operandType->kind == TypeKind::RawPointer))
                dereference->type = operandType->innerTypeId;
        }
        return true;
    }
    if (auto* address = dynamic_cast<AddrOfExpr*>(expression)) {
        if (!bindExpr(address->operand.get())) return false;
        if (address->type.empty() && address->operand)
            for (const auto& type : mModule->typeTable)
                if (type.kind == TypeKind::Reference &&
                    type.isMutable == address->isMutable &&
                    type.innerTypeId == address->operand->type) {
                    address->type = type.id;
                    break;
                }
        return true;
    }
    if (auto* assignment = dynamic_cast<AssignExpr*>(expression)) {
        if (!bindExpr(assignment->lhs.get()) ||
            !bindExpr(assignment->rhs.get()))
            return false;
        if (assignment->type.empty() && assignment->lhs)
            assignment->type = assignment->lhs->type;
        return true;
    }
    if (auto* envLoad = dynamic_cast<EnvLoadExpr*>(expression)) {
        if (envLoad->envLocal.empty()) {
            error(envLoad->location,
                  "environment load has no environment local");
            return false;
        }
        if (envLoad->envLocal.value >= mGraph->locals.size()) {
            error(envLoad->location,
                  "environment load references a missing local");
            return false;
        }
        const auto& envLocal = mGraph->locals[envLoad->envLocal.value];
        if (envLocal.kind != LocalKind::Parameter) {
            error(envLoad->location,
                  "environment load does not reference an environment parameter");
            return false;
        }
        const auto* closure = mModule->findType(envLocal.type);
        if (!closure || closure->kind != TypeKind::Closure) {
            error(envLoad->location,
                  "environment parameter does not have a frozen closure type");
            return false;
        }
        if (envLoad->fieldIndex >= closure->capturedFields.size()) {
            error(envLoad->location,
                  "environment load field is outside its closure environment");
            return false;
        }
        if (envLoad->type.empty())
            envLoad->type =
                closure->capturedFields[envLoad->fieldIndex].type;
        if (envLoad->type !=
            closure->capturedFields[envLoad->fieldIndex].type)
            error(envLoad->location,
                  "environment load type disagrees with its environment field");
        return true;
    }
    return true;
}

} // namespace moon
