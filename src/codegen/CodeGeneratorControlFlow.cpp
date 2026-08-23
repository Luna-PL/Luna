#include "CodeGenerator.h"
#include "../core/TypeLayout.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Metadata.h>

#include <functional>
#include <unordered_set>

namespace {

std::string localName(const moon::LocalRecord& local) {
    return "local." + std::to_string(local.id.value) + "." + local.name;
}

bool shouldUnrollCanonicalLatch(const llvm::BasicBlock* body,
                                const llvm::BranchInst* latch) {
    if (!body || !latch || latch->getParent() != body ||
        !latch->isUnconditional())
        return false;
    unsigned instructionCount = 0;
    for (const llvm::Instruction& instruction : *body) {
        if (++instructionCount > 48 ||
            llvm::isa<llvm::CallBase>(instruction))
            return false;
        if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
            load && (load->isVolatile() || load->isAtomic()))
            return false;
        if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
            store && (store->isVolatile() || store->isAtomic()))
            return false;
    }
    return instructionCount >= 24;
}

void setCanonicalLoopUnrollCount(llvm::BranchInst* latch,
                                 llvm::LLVMContext& context,
                                 unsigned count) {
    auto temporary = llvm::MDNode::getTemporary(context, {});
    llvm::Metadata* countOperands[] = {
        llvm::MDString::get(context, "llvm.loop.unroll.count"),
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
            llvm::Type::getInt32Ty(context), count)),
    };
    auto* countNode = llvm::MDNode::get(context, countOperands);
    llvm::Metadata* loopOperands[] = {temporary.get(), countNode};
    auto* loopID = llvm::MDNode::getDistinct(context, loopOperands);
    loopID->replaceOperandWith(0, loopID);
    latch->setMetadata(llvm::LLVMContext::MD_loop, loopID);
}

} // namespace

void CodeGenerator::generateControlFlowBody(
    moon::ControlFlowGraph& graph, llvm::Function* func,
    llvm::BasicBlock* abiEntry) {
    mCanonicalLocals.assign(graph.locals.size(), nullptr);
    mCanonicalLocalTypes.assign(graph.locals.size(), nullptr);
    std::unordered_set<uint32_t> pointerBackedLocals;
    for (const auto& block : graph.blocks) {
        for (const auto& operation : block.operations) {
            const auto* declaration =
                dynamic_cast<const moon::LetStmt*>(operation.get());
            if (declaration && !declaration->local.empty() &&
                dynamic_cast<const moon::InitAllocationExpr*>(
                    declaration->initializer.get()))
                pointerBackedLocals.insert(declaration->local.value);
        }
    }
    for (const auto& local : graph.locals) {
        TypePtr type = resolveType(local.type);
        llvm::Type* llvmType = local.kind == moon::LocalKind::Allocation ||
                pointerBackedLocals.count(local.id.value)
            ? mHelpers->ptrTy()
            : (type ? mHelpers->toLLVMType(type) : nullptr);
        if (!llvmType || llvmType->isVoidTy()) {
            error("canonical local '" + local.name +
                  "' has no storable LLVM type");
            continue;
        }
        auto* storage = createEntryBlockAlloca(
            func, llvmType, localName(local));
        mCanonicalLocals[local.id.value] = storage;
        mCanonicalLocalTypes[local.id.value] = std::move(type);
    }

    size_t parameterIndex = 0;
    for (const auto& local : graph.locals) {
        if (local.kind != moon::LocalKind::Parameter) continue;
        if (parameterIndex >= func->arg_size() ||
            !mCanonicalLocals[local.id.value]) {
            error("canonical parameter table disagrees with its LLVM function");
            ++parameterIndex;
            continue;
        }
        llvm::Value* argument = func->getArg(
            static_cast<unsigned>(parameterIndex++));
        auto* storage = mCanonicalLocals[local.id.value];
        auto* storageType = storage->getAllocatedType();
        // A closure environment parameter arrives as a pointer to the env
        // struct ({ptr, i32}), but the canonical local is typed as the
        // Closure struct value. Load the struct from the pointer so the
        // store matches the alloca type.
        if (argument->getType()->isPointerTy() &&
            storageType->isStructTy() &&
            argument->getType() != storageType) {
            argument = mBuilder->CreateLoad(
                storageType, argument,
                "closure.env.param");
        }
        mBuilder->CreateStore(
            coerceCallArgument(argument, storageType),
            storage);
    }
    if (parameterIndex != func->arg_size())
        error("canonical parameter table has the wrong LLVM arity");

    std::vector<llvm::BasicBlock*> blocks;
    blocks.reserve(graph.blocks.size());
    for (const auto& block : graph.blocks)
        blocks.push_back(llvm::BasicBlock::Create(
            *mCtx, "cfg." + std::to_string(block.id.value), func));
    if (graph.entry.empty() || graph.entry.value >= blocks.size()) {
        error("canonical CFG has no LLVM entry target");
        return;
    }
    if (!mBuilder->GetInsertBlock()->getTerminator())
        mBuilder->CreateBr(blocks[graph.entry.value]);

    const auto emitCleanups = [this, &graph](
        const std::vector<moon::CleanupId>& cleanups,
        const std::string& context) {
        for (const auto cleanup : cleanups) {
            const auto* record = graph.findCleanup(cleanup);
            if (!record) {
                error(context + " references no canonical cleanup row");
                continue;
            }
            emitCanonicalCleanup(*record);
        }
    };
    const auto emitEdge = [this, &blocks, &emitCleanups](
        const moon::ControlEdge& edge, const std::string& context) {
        if (edge.target.empty() || edge.target.value >= blocks.size()) {
            error(context + " references no LLVM block");
            return;
        }
        emitCleanups(edge.cleanups, context);
        mBuilder->CreateBr(blocks[edge.target.value]);
    };
    const auto edgeTarget = [this, func, &blocks, &emitCleanups](
        const moon::ControlEdge& edge,
        const std::string& context) -> llvm::BasicBlock* {
        if (edge.target.empty() || edge.target.value >= blocks.size()) {
            error(context + " references no LLVM block");
            return nullptr;
        }
        if (edge.cleanups.empty()) return blocks[edge.target.value];

        const auto saved = mBuilder->saveIP();
        auto* cleanupBlock = llvm::BasicBlock::Create(
            *mCtx, context, func);
        mBuilder->SetInsertPoint(cleanupBlock);
        emitCleanups(edge.cleanups, context);
        if (!mBuilder->GetInsertBlock()->getTerminator())
            mBuilder->CreateBr(blocks[edge.target.value]);
        mBuilder->restoreIP(saved);
        return cleanupBlock;
    };

    std::function<std::optional<moon::PlaceRef>(moon::Expr*)> placeOf;
    placeOf = [&placeOf, this](moon::Expr* expression)
        -> std::optional<moon::PlaceRef> {
        if (!expression) return std::nullopt;
        if (auto* identifier = dynamic_cast<moon::IdentifierExpr*>(expression)) {
            if (!identifier->local.empty())
                return moon::PlaceRef{identifier->local, {}};
            return std::nullopt;
        }
        if (auto* field = dynamic_cast<moon::FieldAccessExpr*>(expression)) {
            auto place = placeOf(field->object.get());
            auto objectType = field->object
                ? resolveType(field->object->type) : nullptr;
            if (!place || !objectType) return std::nullopt;
            for (size_t index = 0; index < objectType->fields.size(); ++index) {
                if (objectType->fields[index].name == field->field) {
                    place->projections.push_back({
                        moon::ProjectionKind::Field,
                        static_cast<uint64_t>(index), {}});
                    return place;
                }
            }
            return std::nullopt;
        }
        if (auto* index = dynamic_cast<moon::IndexExpr*>(expression)) {
            auto place = placeOf(index->object.get());
            if (!place) return std::nullopt;
            if (auto* constant = dynamic_cast<moon::IntLiteralExpr*>(
                    index->index.get()); constant && constant->value >= 0) {
                place->projections.push_back({
                    moon::ProjectionKind::ConstantIndex,
                    static_cast<uint64_t>(constant->value), {}});
                return place;
            }
            if (auto* dynamic = dynamic_cast<moon::IdentifierExpr*>(
                    index->index.get()); dynamic && !dynamic->local.empty()) {
                place->projections.push_back({
                    moon::ProjectionKind::DynamicIndex, 0, dynamic->local});
                return place;
            }
            return std::nullopt;
        }
        if (auto* dereference = dynamic_cast<moon::DerefExpr*>(expression)) {
            auto place = placeOf(dereference->operand.get());
            if (place)
                place->projections.push_back({
                    moon::ProjectionKind::Dereference, 0, {}});
            return place;
        }
        return std::nullopt;
    };
    const auto emitCanonicalFree = [this, &graph, &placeOf](
        const moon::FreeStmt& release) {
        const auto place = placeOf(release.operand.get());
        if (!place) {
            error("canonical free has no resolvable PlaceRef");
            return;
        }
        const moon::CleanupRecord* selected = nullptr;
        for (const auto& cleanup : graph.cleanups) {
            if (cleanup.place == *place && cleanup.action == release.action) {
                selected = &cleanup;
                break;
            }
        }
        if (!selected) {
            error("canonical free has no matching cleanup row");
            return;
        }
        emitCanonicalCleanup(*selected);
    };

    for (auto& block : graph.blocks) {
        mBuilder->SetInsertPoint(blocks[block.id.value]);
        for (auto& operation : block.operations) {
            if (auto* declaration =
                    dynamic_cast<moon::LetStmt*>(operation.get())) {
                if (declaration->local.empty() ||
                    declaration->local.value >= mCanonicalLocals.size() ||
                    !mCanonicalLocals[declaration->local.value]) {
                    error("canonical let has no LLVM local storage");
                    continue;
                }
                llvm::Value* value = generateExpr(
                    declaration->initializer.get());
                if (!value || mBuilder->GetInsertBlock()->getTerminator())
                    continue;
                auto* storage = mCanonicalLocals[declaration->local.value];
                mBuilder->CreateStore(
                    coerceCallArgument(value, storage->getAllocatedType()),
                    storage);
            } else if (auto* release =
                           dynamic_cast<moon::FreeStmt*>(operation.get())) {
                if (release->isImplicit) {
                    error("implicit lexical cleanup remains a canonical operation");
                    continue;
                }
                emitCanonicalFree(*release);
            } else if (auto* allocation =
                           dynamic_cast<moon::AllocateStmt*>(operation.get())) {
                if (allocation->local.empty() ||
                    allocation->local.value >= mCanonicalLocals.size() ||
                    !mCanonicalLocals[allocation->local.value]) {
                    error("canonical allocation has no LLVM local storage");
                    continue;
                }
                auto rtAlloc = mModule->getOrInsertFunction(
                    "rt_alloc", mHelpers->ptrTy(), mHelpers->sizeTy(),
                    mHelpers->sizeTy());
                auto type = resolveType(allocation->allocatedType);
                auto pointer = mBuilder->CreateCall(
                    rtAlloc,
                    {llvm::ConstantInt::get(
                         mHelpers->sizeTy(), typeSize(type)),
                     llvm::ConstantInt::get(
                         mHelpers->sizeTy(), typeAlignment(type))},
                    "canonical.allocation");
                mBuilder->CreateStore(
                    coerceCallArgument(
                        pointer,
                        mCanonicalLocals[allocation->local.value]
                            ->getAllocatedType()),
                    mCanonicalLocals[allocation->local.value]);
            } else if (auto* expression =
                           dynamic_cast<moon::ExprStmt*>(
                               operation.get())) {
                (void)generateExpr(expression->expr.get());
            } else if (auto* await =
                           dynamic_cast<moon::AwaitStmt*>(
                               operation.get())) {
                // The simulator completes a launch before returning its
                // event. A device launch or synchronization can fail, so
                // await is the explicit runtime error boundary.
                if (await->event) {
                    auto* event = coerceCallArgument(
                        generateExpr(await->event.get()),
                        mHelpers->i32Ty());
                    auto wait = mModule->getOrInsertFunction(
                        "rt_gpu_await_event", mHelpers->i32Ty(),
                        mHelpers->i32Ty());
                    auto* completed = mBuilder->CreateCall(
                        wait, {event}, "gpu.awaited");
                    emitGpuOperationFailureCheck(completed, func);
                }
            } else {
                error("canonical CFG operation is outside the initial LLVM slice");
            }
            if (mBuilder->GetInsertBlock()->getTerminator()) break;
        }
        if (mBuilder->GetInsertBlock()->getTerminator()) continue;

        const auto& terminator = block.terminator;
        switch (terminator.kind) {
            case moon::TerminatorKind::Jump:
                emitEdge(terminator.primary, "canonical jump edge");
                break;
            case moon::TerminatorKind::Branch: {
                llvm::Value* condition = generateExpr(
                    terminator.operand.get());
                auto* primary = edgeTarget(
                    terminator.primary, "cfg.edge.true.cleanup");
                auto* secondary = edgeTarget(
                    terminator.secondary, "cfg.edge.false.cleanup");
                if (!condition || !primary || !secondary) {
                    error("canonical branch has no LLVM condition or target");
                    break;
                }
                mBuilder->CreateCondBr(condition, primary, secondary);
                break;
            }
            case moon::TerminatorKind::Return: {
                llvm::Type* returnType = func->getReturnType();
                if (returnType->isVoidTy()) {
                    if (terminator.operand)
                        (void)generateExpr(terminator.operand.get());
                    emitCleanups(
                        terminator.exitCleanups,
                        "canonical return cleanup");
                    if (!mBuilder->GetInsertBlock()->getTerminator())
                        mBuilder->CreateRetVoid();
                } else {
                    llvm::Value* value = generateExpr(
                        terminator.operand.get());
                    if (!value) {
                        error("canonical non-void return has no value");
                        break;
                    }
                    emitCleanups(
                        terminator.exitCleanups,
                        "canonical return cleanup");
                    mBuilder->CreateRet(
                        coerceCallArgument(value, returnType));
                }
                break;
            }
            case moon::TerminatorKind::Resume:
                emitEdge(terminator.primary, "canonical resume edge");
                break;
            case moon::TerminatorKind::Abort:
                emitEdge(terminator.primary, "canonical abort edge");
                break;
            case moon::TerminatorKind::Unreachable:
                mBuilder->CreateUnreachable();
                break;
            case moon::TerminatorKind::Switch:
            {
                const TypePtr switchType = resolveType(
                    terminator.switchType);
                if (!switchType ||
                    (switchType->kind != TypeKind::Enum &&
                     switchType->kind != TypeKind::Result)) {
                    error("canonical switch has no LLVM sum type");
                    break;
                }
                llvm::Value* value = generateExpr(
                    terminator.operand.get());
                auto* aggregateType = value
                    ? llvm::dyn_cast<llvm::StructType>(value->getType())
                    : nullptr;
                if (!aggregateType || aggregateType->getNumElements() != 2) {
                    error("canonical switch operand has no tagged-union layout");
                    break;
                }
                llvm::Value* tag = mBuilder->CreateExtractValue(
                    value, {0}, "cfg.switch.tag");
                llvm::Value* payload = mBuilder->CreateExtractValue(
                    value, {1}, "cfg.switch.payload");
                auto* tagType = llvm::dyn_cast<llvm::IntegerType>(
                    tag->getType());
                if (!tagType) {
                    error("canonical switch tag is not an integer");
                    break;
                }

                struct PreparedCase {
                    const moon::SwitchEdge* source = nullptr;
                    std::vector<TypePtr> bindingTypes;
                    std::vector<uint64_t> bindingOffsets;
                };
                std::vector<PreparedCase> prepared;
                prepared.reserve(terminator.cases.size());
                for (const auto& item : terminator.cases) {
                    PreparedCase current;
                    current.source = &item;
                    if (switchType->kind == TypeKind::Enum &&
                        item.tag < switchType->variants.size()) {
                        const auto& variant =
                            switchType->variants[item.tag];
                        current.bindingTypes = variant.fields;
                        current.bindingOffsets.reserve(
                            variant.fields.size());
                        for (size_t index = 0;
                             index < variant.fields.size(); ++index)
                            current.bindingOffsets.push_back(
                                luna::layout::variantFieldOffset(
                                    variant, index));
                    } else if (switchType->kind == TypeKind::Result &&
                               switchType->typeArgs.size() == 2 &&
                               item.tag < 2) {
                        current.bindingTypes.push_back(
                            switchType->typeArgs[
                                item.tag == 1 ? 0 : 1]);
                        current.bindingOffsets.push_back(0);
                    } else {
                        error("canonical switch case is outside its sum type");
                    }
                    if (current.bindingTypes.size() !=
                        item.bindings.size()) {
                        error("canonical switch binding arity disagrees with its case");
                    }
                    prepared.push_back(std::move(current));
                }

                auto* defaultTarget = edgeTarget(
                    terminator.primary, "cfg.switch.default.cleanup");
                if (!defaultTarget) {
                    error("canonical switch has no LLVM default target");
                    break;
                }
                const auto dispatchPoint = mBuilder->saveIP();
                std::vector<llvm::BasicBlock*> caseTargets;
                caseTargets.reserve(prepared.size());
                for (const auto& item : prepared) {
                    if (!item.source) {
                        caseTargets.push_back(nullptr);
                        continue;
                    }
                    const auto& edge = item.source->edge;
                    if (edge.target.empty() ||
                        edge.target.value >= blocks.size()) {
                        error("canonical switch case references no LLVM block");
                        caseTargets.push_back(nullptr);
                        continue;
                    }
                    if (edge.cleanups.empty() &&
                        item.source->bindings.empty()) {
                        caseTargets.push_back(blocks[edge.target.value]);
                        continue;
                    }
                    auto* bridge = llvm::BasicBlock::Create(
                        *mCtx,
                        "cfg.switch.case." +
                            std::to_string(item.source->tag),
                        func);
                    caseTargets.push_back(bridge);
                    mBuilder->SetInsertPoint(bridge);
                    emitCleanups(
                        edge.cleanups, "canonical switch case cleanup");
                    const size_t comparable = std::min(
                        item.bindingTypes.size(),
                        item.source->bindings.size());
                    for (size_t index = 0; index < comparable; ++index) {
                        const auto local = item.source->bindings[index];
                        if (local.empty() ||
                            local.value >= mCanonicalLocals.size() ||
                            !mCanonicalLocals[local.value]) {
                            error("canonical switch binding has no LLVM local storage");
                            continue;
                        }
                        auto* storage = mCanonicalLocals[local.value];
                        llvm::Value* field = unpackResultPayload(
                            payload, item.bindingTypes[index],
                            item.bindingOffsets[index]);
                        mBuilder->CreateStore(
                            coerceCallArgument(
                                field,
                                storage->getAllocatedType()),
                            storage);
                    }
                    if (!mBuilder->GetInsertBlock()->getTerminator())
                        mBuilder->CreateBr(blocks[edge.target.value]);
                }
                mBuilder->restoreIP(dispatchPoint);
                auto* dispatch = mBuilder->CreateSwitch(
                    tag, defaultTarget, prepared.size());
                for (size_t index = 0; index < prepared.size(); ++index) {
                    if (!prepared[index].source || !caseTargets[index])
                        continue;
                    dispatch->addCase(
                        llvm::ConstantInt::get(
                            tagType, prepared[index].source->tag),
                        caseTargets[index]);
                }
                break;
            }
            case moon::TerminatorKind::Invalid:
                error("canonical block has no terminator");
                break;
        }
    }

    if (mOptimizationLevel == LunaOptimizationLevel::O3) {
        for (const auto& block : graph.blocks) {
            const auto& terminator = block.terminator;
            if (terminator.kind != moon::TerminatorKind::Jump ||
                !terminator.primary.cleanups.empty() ||
                terminator.primary.target.empty() ||
                terminator.primary.target.value > block.id.value)
                continue;
            auto* latch = llvm::dyn_cast_or_null<llvm::BranchInst>(
                blocks[block.id.value]->getTerminator());
            if (shouldUnrollCanonicalLatch(blocks[block.id.value], latch))
                setCanonicalLoopUnrollCount(latch, *mCtx, 4);
        }
    }

    mBuilder->SetInsertPoint(abiEntry);
}
