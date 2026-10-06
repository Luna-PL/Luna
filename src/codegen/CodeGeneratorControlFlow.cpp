#include "CodeGenerator.h"
#include "../core/TypeLayout.h"
#include "../runtime/RuntimeFragmentABI.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Metadata.h>

#include <algorithm>
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

void collectRuntimeContinuationLocals(
    const moon::Expr* expression,
    std::unordered_set<uint32_t>& locals) {
    if (!expression) return;
    if (const auto* identifier =
            dynamic_cast<const moon::IdentifierExpr*>(expression)) {
        if (!identifier->local.empty()) locals.insert(identifier->local.value);
    } else if (const auto* binary =
                   dynamic_cast<const moon::BinaryExpr*>(expression)) {
        collectRuntimeContinuationLocals(binary->lhs.get(), locals);
        collectRuntimeContinuationLocals(binary->rhs.get(), locals);
    } else if (const auto* unary =
                   dynamic_cast<const moon::UnaryExpr*>(expression)) {
        collectRuntimeContinuationLocals(unary->operand.get(), locals);
    } else if (const auto* call =
                   dynamic_cast<const moon::CallExpr*>(expression)) {
        collectRuntimeContinuationLocals(call->callee.get(), locals);
        for (const auto& argument : call->args)
            collectRuntimeContinuationLocals(argument.get(), locals);
    } else if (const auto* launch =
                   dynamic_cast<const moon::LaunchExpr*>(expression)) {
        collectRuntimeContinuationLocals(launch->threads.get(), locals);
        for (const auto& argument : launch->args)
            collectRuntimeContinuationLocals(argument.get(), locals);
    } else if (const auto* variant =
                   dynamic_cast<const moon::VariantConstructExpr*>(expression)) {
        for (const auto& argument : variant->args)
            collectRuntimeContinuationLocals(argument.get(), locals);
    } else if (const auto* result =
                   dynamic_cast<const moon::ResultConstructExpr*>(expression)) {
        collectRuntimeContinuationLocals(result->payload.get(), locals);
    } else if (const auto* field =
                   dynamic_cast<const moon::FieldAccessExpr*>(expression)) {
        collectRuntimeContinuationLocals(field->object.get(), locals);
    } else if (const auto* index =
                   dynamic_cast<const moon::IndexExpr*>(expression)) {
        collectRuntimeContinuationLocals(index->object.get(), locals);
        collectRuntimeContinuationLocals(index->index.get(), locals);
    } else if (const auto* length =
                   dynamic_cast<const moon::SliceLengthExpr*>(expression)) {
        collectRuntimeContinuationLocals(length->slice.get(), locals);
    } else if (const auto* array =
                   dynamic_cast<const moon::ArrayLiteralExpr*>(expression)) {
        for (const auto& element : array->elements)
            collectRuntimeContinuationLocals(element.get(), locals);
    } else if (const auto* record =
                   dynamic_cast<const moon::RecordLiteralExpr*>(expression)) {
        for (const auto& field : record->fields)
            collectRuntimeContinuationLocals(field.value.get(), locals);
    } else if (const auto* allocation =
                   dynamic_cast<const moon::HeapAllocExpr*>(expression)) {
        collectRuntimeContinuationLocals(allocation->initializer.get(), locals);
    } else if (const auto* allocation =
                   dynamic_cast<const moon::InitAllocationExpr*>(expression)) {
        for (const auto& element : allocation->elements)
            collectRuntimeContinuationLocals(element.value.get(), locals);
    } else if (const auto* move =
                   dynamic_cast<const moon::MoveExpr*>(expression)) {
        collectRuntimeContinuationLocals(move->operand.get(), locals);
    } else if (const auto* borrow =
                   dynamic_cast<const moon::BorrowExpr*>(expression)) {
        collectRuntimeContinuationLocals(borrow->operand.get(), locals);
    } else if (const auto* dereference =
                   dynamic_cast<const moon::DerefExpr*>(expression)) {
        collectRuntimeContinuationLocals(dereference->operand.get(), locals);
    } else if (const auto* address =
                   dynamic_cast<const moon::AddrOfExpr*>(expression)) {
        collectRuntimeContinuationLocals(address->operand.get(), locals);
    } else if (const auto* closure =
                   dynamic_cast<const moon::MakeClosureExpr*>(expression)) {
        for (const auto& value : closure->capturedValues)
            collectRuntimeContinuationLocals(value.get(), locals);
    } else if (const auto* assignment =
                   dynamic_cast<const moon::AssignExpr*>(expression)) {
        collectRuntimeContinuationLocals(assignment->lhs.get(), locals);
        collectRuntimeContinuationLocals(assignment->rhs.get(), locals);
    }
}

bool scopeWithin(const moon::ControlFlowGraph& graph, moon::ScopeId scope,
                 moon::ScopeId ancestor) {
    while (!scope.empty()) {
        if (scope == ancestor) return true;
        const auto* record = graph.findScope(scope);
        if (!record || record->parent == scope) break;
        scope = record->parent;
    }
    return false;
}

struct RuntimeContinuationPlan {
    const moon::RegionRecord* region = nullptr;
    std::vector<moon::BlockId> blocks;
    std::vector<moon::LocalId> captures;
};

bool prepareRuntimeContinuation(
    const moon::ControlFlowGraph& graph,
    const moon::Terminator& terminator,
    RuntimeContinuationPlan& plan,
    std::string& reason) {
    if (!terminator.primary.cleanups.empty()) {
        reason = "runtime Slot continuation entry cleanup cannot be outlined";
        return false;
    }
    const auto* entry = graph.findBlock(terminator.primary.target);
    const auto* region = entry ? graph.findRegion(entry->region) : nullptr;
    if (!region || region->kind != moon::RegionKind::Continuation ||
        region->entry != terminator.primary.target ||
        region->exit != terminator.secondary.target) {
        reason = "runtime Slot has no exact continuation region";
        return false;
    }
    std::vector<moon::BlockId> outlineBlocks;
    std::unordered_set<uint32_t> regionBlocks;
    for (const auto& block : graph.blocks) {
        for (auto ancestor = block.region; !ancestor.empty();) {
            if (ancestor == region->id) {
                outlineBlocks.push_back(block.id);
                regionBlocks.insert(block.id.value);
                break;
            }
            const auto* record = graph.findRegion(ancestor);
            if (!record || record->kind == moon::RegionKind::Continuation)
                break;
            ancestor = record->parent;
        }
    }
    const auto hasOnlyLocalCleanups = [&](const auto& cleanups) {
        for (const auto cleanupId : cleanups) {
            const auto* cleanup = graph.findCleanup(cleanupId);
            const auto* local = cleanup
                ? graph.findLocal(cleanup->place.root) : nullptr;
            if (!cleanup || !local ||
                !scopeWithin(graph, local->scope, region->scope))
                return false;
        }
        return true;
    };
    std::unordered_set<uint32_t> referencedLocals;
    for (const auto blockId : outlineBlocks) {
        const auto* block = graph.findBlock(blockId);
        if (!block) return false;
        for (const auto& operation : block->operations) {
            if (const auto* declaration =
                    dynamic_cast<const moon::LetStmt*>(operation.get())) {
                collectRuntimeContinuationLocals(
                    declaration->initializer.get(), referencedLocals);
            } else if (const auto* statement =
                           dynamic_cast<const moon::ExprStmt*>(operation.get())) {
                collectRuntimeContinuationLocals(
                    statement->expr.get(), referencedLocals);
            } else if (const auto* release =
                           dynamic_cast<const moon::FreeStmt*>(operation.get())) {
                collectRuntimeContinuationLocals(
                    release->operand.get(), referencedLocals);
            } else if (dynamic_cast<const moon::AllocateStmt*>(
                           operation.get())) {
                // The allocation LocalId is continuation-local and uses the
                // same canonical storage type as the enclosing CFG.
            } else {
                reason = "runtime Slot continuation operation requires full outlining";
                return false;
            }
        }
        collectRuntimeContinuationLocals(
            block->terminator.operand.get(), referencedLocals);
        const auto supportedEdge = [&](const moon::ControlEdge& edge) {
            return hasOnlyLocalCleanups(edge.cleanups) &&
                   (edge.target == terminator.secondary.target ||
                    regionBlocks.count(edge.target.value) != 0);
        };
        if (block->terminator.kind == moon::TerminatorKind::Jump) {
            if (!supportedEdge(block->terminator.primary)) {
                reason = "runtime Slot continuation jump escapes its outline";
                return false;
            }
        } else if (block->terminator.kind == moon::TerminatorKind::Branch) {
            if (!supportedEdge(block->terminator.primary) ||
                !supportedEdge(block->terminator.secondary)) {
                reason = "runtime Slot continuation branch requires cleanup outlining";
                return false;
            }
        } else if (block->terminator.kind == moon::TerminatorKind::Switch) {
            if (!supportedEdge(block->terminator.primary)) {
                reason = "runtime Slot continuation switch default escapes its outline";
                return false;
            }
            for (const auto& item : block->terminator.cases) {
                if (!supportedEdge(item.edge)) {
                    reason = "runtime Slot continuation switch case escapes its outline";
                    return false;
                }
            }
        } else if (block->terminator.kind == moon::TerminatorKind::Return) {
            for (const auto cleanupId : block->terminator.exitCleanups) {
                const auto* cleanup = graph.findCleanup(cleanupId);
                const auto* local = cleanup
                    ? graph.findLocal(cleanup->place.root) : nullptr;
                if (!cleanup || !local) {
                    reason = "runtime Slot continuation return has a missing cleanup local";
                    return false;
                }
                // A return exits the source function, not merely this
                // outline. Its cleanup may own an enclosing local that is
                // otherwise never mentioned by the continuation.
                referencedLocals.insert(local->id.value);
            }
        } else if (block->terminator.kind ==
                   moon::TerminatorKind::RuntimeSlot) {
            if (!supportedEdge(block->terminator.secondary)) {
                reason = "nested runtime Slot completion escapes its outline";
                return false;
            }
            RuntimeContinuationPlan nestedPlan;
            if (!prepareRuntimeContinuation(
                    graph, block->terminator, nestedPlan, reason))
                return false;
            for (const auto local : nestedPlan.captures)
                referencedLocals.insert(local.value);
        } else if (block->terminator.kind !=
                   moon::TerminatorKind::Unreachable) {
            reason = "runtime Slot continuation control escape requires full outlining";
            return false;
        }
    }
    // A Ref used only by an Apply entry has no ordinary expression operand.
    // Its carrier still has to cross the outlined continuation frame.
    for (const auto& binding : graph.runtimeRefApplies) {
        const auto* apply = graph.findRegion(binding.region);
        if (apply && regionBlocks.count(apply->entry.value))
            referencedLocals.insert(binding.reference.value);
    }
    for (const uint32_t localId : referencedLocals) {
        const auto* local = graph.findLocal(moon::LocalId{localId});
        if (!local) {
            reason = "runtime Slot continuation references a missing local";
            return false;
        }
        if (scopeWithin(graph, local->scope, region->scope)) continue;
        // The dispatch ABI is synchronous and resume is single-shot. The
        // outlined storage is a temporary view of the same canonical owner,
        // never an independently live value: only sealed CFG cleanup/move
        // edges may consume it, and completion writes its representation back.
        plan.captures.push_back(local->id);
    }
    std::sort(plan.captures.begin(), plan.captures.end(),
              [](moon::LocalId left, moon::LocalId right) {
                  return left.value < right.value;
              });
    plan.region = region;
    plan.blocks = std::move(outlineBlocks);
    return true;
}

} // namespace

void CodeGenerator::generateControlFlowBody(
    moon::ControlFlowGraph& graph, llvm::Function* func,
    llvm::BasicBlock* abiEntry, size_t hiddenParameterCount) {
    mCanonicalLocals.assign(graph.locals.size(), nullptr);
    mCanonicalLocalTypes.assign(graph.locals.size(), nullptr);
    mCanonicalDeviceBufferLengths.assign(graph.locals.size(), nullptr);
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

    size_t parameterIndex = hiddenParameterCount;
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
        const TypePtr& localType = mCanonicalLocalTypes[local.id.value];
        if (mCurrentFunctionIsKernel && localType &&
            localType->kind == TypeKind::Reference && localType->inner &&
            localType->inner->kind == TypeKind::DeviceBuffer) {
            if (parameterIndex >= func->arg_size()) {
                error("kernel device-buffer parameter is missing its length");
            } else {
                mCanonicalDeviceBufferLengths[local.id.value] = func->getArg(
                    static_cast<unsigned>(parameterIndex++));
            }
        }
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

    std::optional<moon::RuntimeRefApplyFlowPlan> refApplyFlow;
    std::vector<llvm::AllocaInst*> refContextCells(graph.regions.size(), nullptr);
    llvm::Value* parentFragmentContext = mCurrentFragmentContext;
    if (!graph.runtimeRefApplies.empty()) {
        std::string flowError;
        if (!mPrivateRefApplyEnabled ||
            graph.runtimeRefApplies.size() > 2 ||
            !parentFragmentContext ||
            !(refApplyFlow = moon::planRuntimeRefApplyFlow(graph, flowError))) {
            error("private Ref apply body requires one or two verified context regions: " +
                  flowError);
            return;
        }
        for (size_t index = 0; index < graph.runtimeRefApplies.size(); ++index) {
            const auto& binding = graph.runtimeRefApplies[index];
            const auto* apply = graph.findRegion(binding.region);
            if (!apply) {
                error("private Ref apply has no verified region");
                return;
            }
            if (index == 0) {
                if (apply->parent != graph.rootRegion) {
                    error("private Ref apply body requires a top-level region");
                    return;
                }
            } else {
                bool insidePrevious = false;
                for (auto parent = apply->parent; !parent.empty();) {
                    if (parent == graph.runtimeRefApplies[index - 1].region) {
                        insidePrevious = true;
                        break;
                    }
                    const auto* enclosing = graph.findRegion(parent);
                    if (!enclosing)
                        break;
                    parent = enclosing->parent;
                }
                if (!insidePrevious) {
                    error("private nested Ref apply requires a child region");
                    return;
                }
            }
        }
        std::vector<size_t> entries(graph.regions.size(), 0);
        std::vector<size_t> exits(graph.regions.size(), 0);
        for (const auto& transition : refApplyFlow->edges) {
            if (transition.enters.empty() && transition.exits.empty())
                continue;
            const auto* source = graph.findBlock(transition.source);
            if (!source || source->terminator.kind != moon::TerminatorKind::Jump ||
                transition.enters.size() + transition.exits.size() != 1) {
                error("private Ref apply body cannot lower a non-jump context transition");
                return;
            }
            for (const auto region : transition.enters) ++entries[region.value];
            for (const auto region : transition.exits) ++exits[region.value];
        }
        const bool validTerminals = std::all_of(
            refApplyFlow->terminals.begin(), refApplyFlow->terminals.end(),
            [refApplyFlow, &graph, this](const auto& terminal) {
                if (terminal.kind == moon::TerminatorKind::Unreachable &&
                    mProgram && moon::isExhaustiveResultDefault(
                        graph, *mProgram, *refApplyFlow, terminal.block))
                    return true;
                if (terminal.kind != moon::TerminatorKind::Return ||
                    terminal.block.value >= refApplyFlow->activeByBlock.size())
                    return false;
                const auto& active = refApplyFlow->activeByBlock[terminal.block.value];
                return std::equal(terminal.exits.begin(), terminal.exits.end(),
                                  active.rbegin(), active.rend());
            });
        if (!validTerminals) {
            error("private Ref apply body requires verified return/Jump exits");
            return;
        }
        for (const auto& binding : graph.runtimeRefApplies) {
            const auto region = binding.region;
            size_t terminalExits = 0;
            for (const auto& terminal : refApplyFlow->terminals)
                terminalExits += std::count(
                    terminal.exits.begin(), terminal.exits.end(), region);
            if (entries[region.value] != 1 ||
                exits[region.value] + terminalExits == 0) {
                error("private Ref apply body requires one entry per region and an exit");
                return;
            }
            auto* cell = createEntryBlockAlloca(
                func, mHelpers->ptrTy(),
                "ref.apply.context.owner." + std::to_string(region.value));
            mBuilder->CreateStore(llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(mHelpers->ptrTy())), cell);
            refContextCells[region.value] = cell;
        }
    }

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
    const auto contextOwnersFor =
        [&refApplyFlow, &refContextCells](moon::BlockId block) {
            std::vector<llvm::Value*> owners;
            if (!refApplyFlow || block.value >= refApplyFlow->activeByBlock.size())
                return owners;
            for (const auto region : refApplyFlow->activeByBlock[block.value])
                owners.push_back(refContextCells[region.value]);
            return owners;
        };
    const auto emitContextExitCleanups =
        [this, &graph, &emitCleanups](
            const std::vector<moon::CleanupId>& cleanups,
            const std::vector<moon::RegionId>& exits,
            const std::vector<llvm::Value*>& owners,
            const std::string& label) -> bool {
        if (owners.size() < exits.size()) {
            error("private Ref apply exit lost its context owner stack");
            return false;
        }
        std::vector<std::vector<moon::CleanupId>> ordered(exits.size() + 1);
        size_t lastGroup = 0;
        for (const auto cleanup : cleanups) {
            const auto* record = graph.findCleanup(cleanup);
            if (!record) {
                error("private Ref apply exit has no cleanup row");
                return false;
            }
            size_t group = exits.size();
            for (size_t index = 0; index < exits.size(); ++index) {
                const auto* apply = graph.findRegion(exits[index]);
                if (!apply) {
                    error("private Ref apply exit has no region");
                    return false;
                }
                for (auto scope = record->scope; !scope.empty();) {
                    if (scope == apply->scope) {
                        group = index;
                        break;
                    }
                    const auto* parent = graph.findScope(scope);
                    if (!parent) {
                        error("private Ref apply exit has no cleanup scope");
                        return false;
                    }
                    scope = parent->parent;
                }
                if (group != exits.size()) break;
            }
            if (group < lastGroup) {
                error("private Ref apply exit interleaves context cleanups");
                return false;
            }
            lastGroup = group;
            ordered[group].push_back(cleanup);
        }
        for (size_t index = 0; index < exits.size(); ++index) {
            emitCleanups(ordered[index], label);
            auto* owner = owners[owners.size() - 1 - index];
            if (!owner) {
                error("private Ref apply exit has no context owner cell");
                return false;
            }
            mHelpers->emitRuntimeFragmentContextDrop(
                *mBuilder, *mModule, owner);
        }
        emitCleanups(ordered.back(), label);
        return true;
    };
    const auto emitReturnCleanups =
        [&refApplyFlow, &emitContextExitCleanups](
            moon::BlockId source,
            const std::vector<moon::CleanupId>& cleanups,
            const std::vector<llvm::Value*>& owners) -> bool {
        const moon::RuntimeRefApplyFlowTerminal* terminal = nullptr;
        if (refApplyFlow)
            for (const auto& item : refApplyFlow->terminals)
                if (item.block == source) terminal = &item;
        return emitContextExitCleanups(
            cleanups, terminal ? terminal->exits :
                std::vector<moon::RegionId>{},
            owners, "canonical Ref apply return cleanup");
    };
    const auto emitEdge = [this, &blocks, &emitCleanups, &graph,
                           &refApplyFlow, &refContextCells, &contextOwnersFor,
                           &emitContextExitCleanups](
        moon::BlockId source, const moon::ControlEdge& edge,
        const std::string& context) {
        if (edge.target.empty() || edge.target.value >= blocks.size()) {
            error(context + " references no LLVM block");
            return;
        }
        if (refApplyFlow) {
            const auto found = std::find_if(
                refApplyFlow->edges.begin(), refApplyFlow->edges.end(),
                [&](const moon::RuntimeRefApplyFlowEdge& transition) {
                    return transition.source == source &&
                        transition.target == edge.target;
                });
            if (found == refApplyFlow->edges.end()) {
                error("private Ref apply edge has no verified transition");
                return;
            }
            const auto owners = contextOwnersFor(source);
            if (!emitContextExitCleanups(
                    edge.cleanups, found->exits, owners, context))
                return;
            if (!found->enters.empty()) {
                const auto region = found->enters.front();
                const auto binding = std::find_if(
                    graph.runtimeRefApplies.begin(),
                    graph.runtimeRefApplies.end(),
                    [region](const auto& item) {
                        return item.region == region;
                    });
                if (binding == graph.runtimeRefApplies.end()) {
                    error("private Ref apply entry has no binding");
                    return;
                }
                if (binding->reference.value >= mCanonicalLocals.size() ||
                    !mCanonicalLocals[binding->reference.value]) {
                    error("private Ref apply has no local Ref carrier");
                    return;
                }
                auto* borrowed = mBuilder->CreateLoad(
                    mHelpers->ptrTy(),
                    mCanonicalLocals[binding->reference.value],
                    "ref.apply.borrowed");
                auto* status = mHelpers->emitRuntimeFragmentRefContextOverride(
                    *mBuilder, *mModule, mCurrentFragmentContext,
                    borrowed, binding->slot, refContextCells[region.value]);
                if (!status) {
                    error("private Ref apply could not emit context derivation");
                    return;
                }
                auto* accepted = llvm::BasicBlock::Create(
                    *mCtx, "ref.apply.accepted", mCurrentFunc);
                auto* failed = llvm::BasicBlock::Create(
                    *mCtx, "ref.apply.failed", mCurrentFunc);
                mBuilder->CreateCondBr(
                    mBuilder->CreateICmpEQ(status,
                        llvm::ConstantInt::get(mHelpers->i32Ty(), 0)),
                    accepted, failed);
                mBuilder->SetInsertPoint(failed);
                for (auto it = owners.rbegin(); it != owners.rend(); ++it)
                    mHelpers->emitRuntimeFragmentContextDrop(
                        *mBuilder, *mModule, *it);
                auto* trap = llvm::Intrinsic::getOrInsertDeclaration(
                    mModule.get(), llvm::Intrinsic::trap);
                mBuilder->CreateCall(trap);
                mBuilder->CreateUnreachable();
                mBuilder->SetInsertPoint(accepted);
            }
        } else
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
        mCurrentFragmentContext = parentFragmentContext;
        if (refApplyFlow &&
            !refApplyFlow->activeByBlock[block.id.value].empty())
            mCurrentFragmentContext = mBuilder->CreateLoad(
                mHelpers->ptrTy(),
                refContextCells[
                    refApplyFlow->activeByBlock[block.id.value].back().value],
                "ref.apply.context");
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
                const TypePtr& localType =
                    mCanonicalLocalTypes[declaration->local.value];
                if (localType && localType->kind == TypeKind::Array &&
                    llvm::isa<llvm::Constant>(value)) {
                    auto* initializer = llvm::cast<llvm::Constant>(value);
                    auto* global = new llvm::GlobalVariable(
                        *mModule, storage->getAllocatedType(), true,
                        llvm::GlobalValue::PrivateLinkage, initializer,
                        "array.literal");
                    global->setUnnamedAddr(
                        llvm::GlobalValue::UnnamedAddr::Global);
                    const auto alignment = llvm::Align(std::max<uint64_t>(
                        1, luna::layout::valueAlignment(localType)));
                    global->setAlignment(alignment);
                    mBuilder->CreateMemCpy(
                        storage, alignment, global, alignment,
                        luna::layout::valueSize(localType));
                } else {
                    mBuilder->CreateStore(
                        coerceCallArgument(
                            value, storage->getAllocatedType()),
                        storage);
                }
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
                emitEdge(block.id, terminator.primary, "canonical jump edge");
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
                    if (!emitReturnCleanups(
                            block.id, terminator.exitCleanups,
                            contextOwnersFor(block.id)))
                        break;
                    if (!mBuilder->GetInsertBlock()->getTerminator())
                        mBuilder->CreateRetVoid();
                } else {
                    llvm::Value* value = nullptr;
                    const TypePtr operandType = terminator.operand
                        ? resolveType(terminator.operand->type) : nullptr;
                    if (operandType &&
                        operandType->kind == TypeKind::RuntimeFragmentRef) {
                        if (auto* identifier = dynamic_cast<moon::IdentifierExpr*>(
                                terminator.operand.get())) {
                            const auto* local = graph.findLocal(identifier->local);
                            if (!local || local->relation !=
                                    luna::ownership::Relation::Owned ||
                                identifier->local.value >= mCanonicalLocals.size() ||
                                !mCanonicalLocals[identifier->local.value]) {
                                error("RuntimeFragmentRef return requires an owned local carrier");
                                break;
                            }
                            value = mHelpers->emitRuntimeFragmentRefTake(
                                *mBuilder, mCanonicalLocals[identifier->local.value]);
                        } else if (dynamic_cast<moon::FieldAccessExpr*>(
                                       terminator.operand.get()) ||
                                   dynamic_cast<moon::IndexExpr*>(
                                       terminator.operand.get())) {
                            error("RuntimeFragmentRef projected return requires in-place transfer");
                            break;
                        }
                    }
                    if (!value)
                        value = generateExpr(terminator.operand.get());
                    if (!value) {
                        error("canonical non-void return has no value");
                        break;
                    }
                    if (!emitReturnCleanups(
                            block.id, terminator.exitCleanups,
                            contextOwnersFor(block.id)))
                        break;
                    mBuilder->CreateRet(
                        coerceCallArgument(value, returnType));
                }
                break;
            }
            case moon::TerminatorKind::Resume:
                emitEdge(block.id, terminator.primary, "canonical resume edge");
                break;
            case moon::TerminatorKind::Discard:
                emitEdge(block.id, terminator.primary, "canonical fragment discard edge");
                break;
            case moon::TerminatorKind::Unreachable:
                mBuilder->CreateUnreachable();
                break;
            case moon::TerminatorKind::RuntimeSlot: {
                using RuntimeCompletionTarget =
                    std::function<llvm::BasicBlock*(const moon::ControlEdge&, const std::string&)>;
                // The same emitter handles a source-function site and sites
                // inside an outlined continuation. Each nested frame inherits
                // the original return storage; only the outermost frame
                // materializes the source-level return value.
                std::function<void(const moon::BasicBlock&, const moon::Terminator&,
                                   llvm::Function*, const RuntimeCompletionTarget&, llvm::Type*,
                                   llvm::Value*, const std::vector<llvm::Value*>&,
                                   const std::function<void()>&)>
                    emitRuntimeSlot;
                emitRuntimeSlot = [&](const moon::BasicBlock& block,
                                      const moon::Terminator& terminator, llvm::Function* func,
                                      const RuntimeCompletionTarget& completionTarget,
                                      llvm::Type* sourceReturnType, llvm::Value* inheritedReturn,
                                      const std::vector<llvm::Value*>& inheritedRefContextOwners,
                                      const std::function<void()>& propagateEscape) {
                    if (!mCurrentFragmentContext) {
                        error("runtime Slot has no explicit Fragment execution context");
                        mBuilder->CreateUnreachable();
                        return;
                    }
                    RuntimeContinuationPlan continuationPlan;
                    std::string continuationError;
                    if (!prepareRuntimeContinuation(graph, terminator, continuationPlan,
                                                    continuationError)) {
                        error(continuationError);
                        mBuilder->CreateUnreachable();
                        return;
                    }
                    const auto* slot = resolveDeclaration(terminator.runtimeSlot);
                    TypePtr argumentType = resolveType(terminator.runtimeArgumentsType);
                    const auto* argumentRecord =
                        mProgram ? mProgram->findType(terminator.runtimeArgumentsType) : nullptr;
                    llvm::Value* arguments = generateExpr(terminator.operand.get());
                    auto* completion =
                        completionTarget(terminator.secondary, "runtime.slot.completion.cleanup");
                    if (!slot || !argumentType || !argumentRecord || !arguments || !completion) {
                        error("runtime Slot dispatch has incomplete nominal or layout facts");
                        if (!mBuilder->GetInsertBlock()->getTerminator())
                            mBuilder->CreateUnreachable();
                        return;
                    }

                    auto* argumentStorage = createEntryBlockAlloca(func, arguments->getType(),
                                                                   "runtime.slot.arguments");
                    mBuilder->CreateStore(arguments, argumentStorage);

                    std::vector<llvm::Type*> frameFields(
                        continuationPlan.captures.size() + 2 +
                            inheritedRefContextOwners.size(),
                        mHelpers->ptrTy());
                    auto* frameType = llvm::StructType::get(*mCtx, frameFields);
                    auto* frame = createEntryBlockAlloca(func, frameType, "runtime.slot.frame");
                    mBuilder->CreateStore(mCurrentFragmentContext,
                                          mBuilder->CreateStructGEP(frameType, frame, 0,
                                                                    "runtime.slot.frame.context"));
                    llvm::Value* escapedReturn = inheritedReturn;
                    if (!sourceReturnType->isVoidTy() && !escapedReturn)
                        escapedReturn = createEntryBlockAlloca(func, sourceReturnType,
                                                               "runtime.slot.escaped.return");
                    mBuilder->CreateStore(
                        escapedReturn ? escapedReturn
                                      : llvm::ConstantPointerNull::get(
                                            llvm::cast<llvm::PointerType>(mHelpers->ptrTy())),
                        mBuilder->CreateStructGEP(frameType, frame, 1,
                                                  "runtime.slot.frame.return"));
                    for (size_t index = 0; index < continuationPlan.captures.size(); ++index) {
                        const auto local = continuationPlan.captures[index];
                        if (local.value >= mCanonicalLocals.size() ||
                            !mCanonicalLocals[local.value]) {
                            error("runtime Slot continuation capture has no LLVM storage");
                            continue;
                        }
                        mBuilder->CreateStore(
                            mCanonicalLocals[local.value],
                            mBuilder->CreateStructGEP(frameType, frame,
                                                      static_cast<unsigned>(index + 2),
                                                      "runtime.slot.frame.capture"));
                    }
                    for (size_t index = 0;
                         index < inheritedRefContextOwners.size(); ++index)
                        mBuilder->CreateStore(
                            inheritedRefContextOwners[index],
                            mBuilder->CreateStructGEP(
                                frameType, frame,
                                static_cast<unsigned>(
                                    continuationPlan.captures.size() + 2 + index),
                                "runtime.slot.frame.ref.context.owner"));

                    auto* callbackType =
                        llvm::FunctionType::get(mHelpers->i32Ty(), {mHelpers->ptrTy()}, false);
                    const std::string callbackName = func->getName().str() + ".runtime_slot." +
                                                     std::to_string(block.id.value) + ".continue";
                    auto* callback =
                        llvm::Function::Create(callbackType, llvm::GlobalValue::InternalLinkage,
                                               callbackName, mModule.get());
                    auto* callbackEntry = llvm::BasicBlock::Create(*mCtx, "entry", callback);
                    callback->getArg(0)->setName("runtime.slot.frame");

                    const auto parentInsertPoint = mBuilder->saveIP();
                    auto* savedFunction = mCurrentFunc;
                    auto* savedFragmentContext = mCurrentFragmentContext;
                    const bool savedKernelMode = mCurrentFunctionIsKernel;
                    auto savedCanonicalLocals = std::move(mCanonicalLocals);
                    auto savedCanonicalLocalTypes = std::move(mCanonicalLocalTypes);
                    auto savedCanonicalDeviceBufferLengths =
                        std::move(mCanonicalDeviceBufferLengths);

                    mCurrentFunc = callback;
                    mCurrentFunctionIsKernel = false;
                    mBuilder->SetInsertPoint(callbackEntry);
                    mCurrentFragmentContext = mBuilder->CreateLoad(
                        mHelpers->ptrTy(),
                        mBuilder->CreateStructGEP(frameType, callback->getArg(0), 0,
                                                  "runtime.slot.context.address"),
                        "fragment.context");
                    llvm::Value* callbackBaseContext = mCurrentFragmentContext;
                    std::vector<llvm::Value*> callbackRefContextOwners;
                    callbackRefContextOwners.reserve(
                        inheritedRefContextOwners.size());
                    for (size_t index = 0;
                         index < inheritedRefContextOwners.size(); ++index)
                        callbackRefContextOwners.push_back(mBuilder->CreateLoad(
                            mHelpers->ptrTy(),
                            mBuilder->CreateStructGEP(
                                frameType, callback->getArg(0),
                                static_cast<unsigned>(
                                    continuationPlan.captures.size() + 2 + index),
                                "runtime.slot.ref.context.owner.address"),
                            "runtime.slot.ref.context.owner"));
                    std::vector<llvm::Value*> callbackOwnerByRegion(
                        graph.regions.size(), nullptr);
                    std::vector<moon::RegionId> callbackBaseRegions;
                    if (refApplyFlow) {
                        callbackBaseRegions =
                            refApplyFlow->activeByBlock[block.id.value];
                        if (callbackBaseRegions.size() !=
                            callbackRefContextOwners.size())
                            error("outlined Ref apply lost its inherited owner stack");
                        for (size_t index = 0;
                             index < callbackBaseRegions.size() &&
                             index < callbackRefContextOwners.size(); ++index)
                            callbackOwnerByRegion[
                                callbackBaseRegions[index].value] =
                                    callbackRefContextOwners[index];
                        for (const auto& transition : refApplyFlow->edges) {
                            if (transition.enters.empty() ||
                                std::find(
                                    continuationPlan.blocks.begin(),
                                    continuationPlan.blocks.end(),
                                    transition.source) ==
                                    continuationPlan.blocks.end())
                                continue;
                            const auto region = transition.enters.front();
                            if (callbackOwnerByRegion[region.value]) continue;
                            auto* cell = createEntryBlockAlloca(
                                callback, mHelpers->ptrTy(),
                                "ref.apply.context.owner." +
                                    std::to_string(region.value) + ".outlined");
                            mBuilder->CreateStore(
                                llvm::ConstantPointerNull::get(
                                    llvm::cast<llvm::PointerType>(
                                        mHelpers->ptrTy())),
                                cell);
                            callbackOwnerByRegion[region.value] = cell;
                        }
                    }
                    const auto callbackOwnersFor =
                        [&refApplyFlow, &callbackOwnerByRegion](
                            moon::BlockId blockId) {
                            std::vector<llvm::Value*> owners;
                            if (!refApplyFlow ||
                                blockId.value >=
                                    refApplyFlow->activeByBlock.size())
                                return owners;
                            for (const auto region :
                                 refApplyFlow->activeByBlock[blockId.value])
                                owners.push_back(
                                    callbackOwnerByRegion[region.value]);
                            return owners;
                        };
                    mCanonicalLocals.assign(graph.locals.size(), nullptr);
                    mCanonicalLocalTypes.assign(graph.locals.size(), nullptr);
                    mCanonicalDeviceBufferLengths.assign(graph.locals.size(), nullptr);
                    for (const auto& local : graph.locals) {
                        if (local.id.value >= savedCanonicalLocals.size() ||
                            !savedCanonicalLocals[local.id.value]) {
                            error("runtime Slot outline cannot recover local storage type");
                            continue;
                        }
                        auto* localStorage = createEntryBlockAlloca(
                            callback, savedCanonicalLocals[local.id.value]->getAllocatedType(),
                            localName(local) + ".outlined");
                        mCanonicalLocals[local.id.value] = localStorage;
                        mCanonicalLocalTypes[local.id.value] = resolveType(local.type);
                    }
                    for (size_t index = 0; index < continuationPlan.captures.size(); ++index) {
                        const auto local = continuationPlan.captures[index];
                        if (local.value >= mCanonicalLocals.size() ||
                            !mCanonicalLocals[local.value])
                            continue;
                        auto* source = mBuilder->CreateLoad(
                            mHelpers->ptrTy(),
                            mBuilder->CreateStructGEP(frameType, callback->getArg(0),
                                                      static_cast<unsigned>(index + 2),
                                                      "runtime.slot.capture.address"),
                            "runtime.slot.capture");
                        auto* storage = mCanonicalLocals[local.value];
                        mBuilder->CreateStore(mBuilder->CreateLoad(storage->getAllocatedType(),
                                                                   source,
                                                                   "runtime.slot.capture.value"),
                                              storage);
                    }

                    std::vector<llvm::BasicBlock*> continuationBlocks(graph.blocks.size(), nullptr);
                    for (const auto blockId : continuationPlan.blocks)
                        continuationBlocks[blockId.value] = llvm::BasicBlock::Create(
                            *mCtx, "runtime.slot.cfg." + std::to_string(blockId.value), callback);
                    auto* callbackCompletion =
                        llvm::BasicBlock::Create(*mCtx, "runtime.slot.completed", callback);
                    mBuilder->CreateBr(continuationBlocks[continuationPlan.region->entry.value]);

                    const auto callbackTarget = [&](moon::BlockId target) -> llvm::BasicBlock* {
                        if (target == terminator.secondary.target) return callbackCompletion;
                        return target.value < continuationBlocks.size()
                                   ? continuationBlocks[target.value]
                                   : nullptr;
                    };
                    const auto outlinedEdgeTarget =
                        [&](const moon::ControlEdge& edge,
                            const std::string& label) -> llvm::BasicBlock* {
                        auto* target = callbackTarget(edge.target);
                        if (!target || edge.cleanups.empty()) return target;
                        const auto saved = mBuilder->saveIP();
                        auto* bridge = llvm::BasicBlock::Create(*mCtx, label, callback);
                        mBuilder->SetInsertPoint(bridge);
                        for (const auto cleanupId : edge.cleanups) {
                            const auto* cleanup = graph.findCleanup(cleanupId);
                            if (!cleanup)
                                error("runtime Slot outline references no cleanup row");
                            else
                                emitCanonicalCleanup(*cleanup);
                        }
                        if (!mBuilder->GetInsertBlock()->getTerminator())
                            mBuilder->CreateBr(target);
                        mBuilder->restoreIP(saved);
                        return bridge;
                    };
                    const auto emitOutlinedJump =
                        [&](moon::BlockId source,
                            const moon::ControlEdge& edge) {
                            auto* target = callbackTarget(edge.target);
                            if (!target) {
                                error("outlined Ref apply Jump has no target");
                                mBuilder->CreateUnreachable();
                                return;
                            }
                            if (!refApplyFlow) {
                                mBuilder->CreateBr(outlinedEdgeTarget(
                                    edge, "runtime.slot.jump.cleanup"));
                                return;
                            }
                            const auto found = std::find_if(
                                refApplyFlow->edges.begin(),
                                refApplyFlow->edges.end(),
                                [source, &edge](const auto& transition) {
                                    return transition.source == source &&
                                           transition.target == edge.target;
                                });
                            if (found == refApplyFlow->edges.end()) {
                                error("outlined Ref apply Jump has no verified transition");
                                mBuilder->CreateUnreachable();
                                return;
                            }
                            const auto owners = callbackOwnersFor(source);
                            if (std::any_of(owners.begin(), owners.end(),
                                            [](llvm::Value* owner) {
                                                return !owner;
                                            }) ||
                                !emitContextExitCleanups(
                                    edge.cleanups, found->exits, owners,
                                    "outlined Ref apply Jump cleanup")) {
                                error("outlined Ref apply Jump lost an owner cell");
                                mBuilder->CreateUnreachable();
                                return;
                            }
                            if (!found->enters.empty()) {
                                const auto region = found->enters.front();
                                const auto binding = std::find_if(
                                    graph.runtimeRefApplies.begin(),
                                    graph.runtimeRefApplies.end(),
                                    [region](const auto& item) {
                                        return item.region == region;
                                    });
                                if (binding == graph.runtimeRefApplies.end() ||
                                    !callbackOwnerByRegion[region.value] ||
                                    binding->reference.value >=
                                        mCanonicalLocals.size() ||
                                    !mCanonicalLocals[
                                        binding->reference.value]) {
                                    error("outlined Ref apply entry lost its Ref or owner cell");
                                    mBuilder->CreateUnreachable();
                                    return;
                                }
                                auto* borrowed = mBuilder->CreateLoad(
                                    mHelpers->ptrTy(),
                                    mCanonicalLocals[
                                        binding->reference.value],
                                    "ref.apply.borrowed.outlined");
                                auto* status =
                                    mHelpers->emitRuntimeFragmentRefContextOverride(
                                        *mBuilder, *mModule,
                                        mCurrentFragmentContext, borrowed,
                                        binding->slot,
                                        callbackOwnerByRegion[region.value]);
                                if (!status) {
                                    error("outlined Ref apply could not derive its context");
                                    mBuilder->CreateUnreachable();
                                    return;
                                }
                                auto* accepted = llvm::BasicBlock::Create(
                                    *mCtx, "ref.apply.accepted", callback);
                                auto* failed = llvm::BasicBlock::Create(
                                    *mCtx, "ref.apply.failed", callback);
                                mBuilder->CreateCondBr(
                                    mBuilder->CreateICmpEQ(
                                        status,
                                        llvm::ConstantInt::get(
                                            mHelpers->i32Ty(), 0)),
                                    accepted, failed);
                                mBuilder->SetInsertPoint(failed);
                                for (auto it = owners.rbegin();
                                     it != owners.rend(); ++it)
                                    mHelpers->emitRuntimeFragmentContextDrop(
                                        *mBuilder, *mModule, *it);
                                auto* trap =
                                    llvm::Intrinsic::getOrInsertDeclaration(
                                        mModule.get(), llvm::Intrinsic::trap);
                                mBuilder->CreateCall(trap);
                                mBuilder->CreateUnreachable();
                                mBuilder->SetInsertPoint(accepted);
                            }
                            mBuilder->CreateBr(target);
                        };
                    const auto emitCaptureWriteback = [&]() {
                        for (size_t index = 0; index < continuationPlan.captures.size(); ++index) {
                            const auto local = continuationPlan.captures[index];
                            if (local.value >= mCanonicalLocals.size() ||
                                !mCanonicalLocals[local.value])
                                continue;
                            auto* destination = mBuilder->CreateLoad(
                                mHelpers->ptrTy(),
                                mBuilder->CreateStructGEP(frameType, callback->getArg(0),
                                                          static_cast<unsigned>(index + 2),
                                                          "runtime.slot.capture.writeback.address"),
                                "runtime.slot.capture.writeback");
                            auto* storage = mCanonicalLocals[local.value];
                            mBuilder->CreateStore(
                                mBuilder->CreateLoad(storage->getAllocatedType(), storage,
                                                     "runtime.slot.capture.writeback.value"),
                                destination);
                        }
                    };
                    for (const auto blockId : continuationPlan.blocks) {
                        const auto* continuationBlock = graph.findBlock(blockId);
                        auto* llvmBlock = continuationBlocks[blockId.value];
                        if (!continuationBlock || !llvmBlock) continue;
                        mBuilder->SetInsertPoint(llvmBlock);
                        if (refApplyFlow) {
                            const auto& active =
                                refApplyFlow->activeByBlock[blockId.value];
                            const auto owners = callbackOwnersFor(blockId);
                            if (owners.size() != active.size() ||
                                std::any_of(owners.begin(), owners.end(),
                                            [](llvm::Value* owner) {
                                                return !owner;
                                            })) {
                                error("outlined Ref apply block lost its owner stack");
                                mBuilder->CreateUnreachable();
                                continue;
                            }
                            mCurrentFragmentContext =
                                active == callbackBaseRegions
                                    ? callbackBaseContext
                                    : mBuilder->CreateLoad(
                                        mHelpers->ptrTy(), owners.back(),
                                        "ref.apply.context.outlined");
                        }
                        for (const auto& operation : continuationBlock->operations) {
                            if (const auto* declaration =
                                    dynamic_cast<const moon::LetStmt*>(operation.get())) {
                                llvm::Value* value = generateExpr(declaration->initializer.get());
                                if (value && !declaration->local.empty() &&
                                    declaration->local.value < mCanonicalLocals.size() &&
                                    mCanonicalLocals[declaration->local.value] &&
                                    !mBuilder->GetInsertBlock()->getTerminator()) {
                                    auto* storage = mCanonicalLocals[declaration->local.value];
                                    mBuilder->CreateStore(
                                        coerceCallArgument(value, storage->getAllocatedType()),
                                        storage);
                                }
                            } else if (const auto* statement =
                                           dynamic_cast<const moon::ExprStmt*>(operation.get())) {
                                (void)generateExpr(statement->expr.get());
                            } else if (const auto* release =
                                           dynamic_cast<const moon::FreeStmt*>(operation.get())) {
                                emitCanonicalFree(*release);
                            } else if (const auto* allocation =
                                           dynamic_cast<const moon::AllocateStmt*>(
                                               operation.get())) {
                                if (allocation->local.empty() ||
                                    allocation->local.value >= mCanonicalLocals.size() ||
                                    !mCanonicalLocals[allocation->local.value]) {
                                    error("outlined canonical allocation has no local storage");
                                } else {
                                    auto allocatedType = resolveType(allocation->allocatedType);
                                    auto rtAlloc = mModule->getOrInsertFunction(
                                        "rt_alloc", mHelpers->ptrTy(), mHelpers->sizeTy(),
                                        mHelpers->sizeTy());
                                    auto* pointer = mBuilder->CreateCall(
                                        rtAlloc,
                                        {llvm::ConstantInt::get(mHelpers->sizeTy(),
                                                                typeSize(allocatedType)),
                                         llvm::ConstantInt::get(mHelpers->sizeTy(),
                                                                typeAlignment(allocatedType))},
                                        "runtime.slot.allocation");
                                    mBuilder->CreateStore(
                                        pointer, mCanonicalLocals[allocation->local.value]);
                                }
                            }
                            if (mBuilder->GetInsertBlock()->getTerminator()) break;
                        }
                        if (mBuilder->GetInsertBlock()->getTerminator()) continue;
                        const auto& callbackTerminator = continuationBlock->terminator;
                        if (callbackTerminator.kind == moon::TerminatorKind::Jump) {
                            emitOutlinedJump(continuationBlock->id,
                                             callbackTerminator.primary);
                        } else if (callbackTerminator.kind == moon::TerminatorKind::Branch) {
                            llvm::Value* condition = generateExpr(callbackTerminator.operand.get());
                            auto* yes = outlinedEdgeTarget(callbackTerminator.primary,
                                                           "runtime.slot.branch.true.cleanup");
                            auto* no = outlinedEdgeTarget(callbackTerminator.secondary,
                                                          "runtime.slot.branch.false.cleanup");
                            if (!condition || !yes || !no)
                                error("runtime Slot outlined branch has no LLVM target");
                            else
                                mBuilder->CreateCondBr(condition, yes, no);
                        } else if (callbackTerminator.kind == moon::TerminatorKind::Switch) {
                            const TypePtr switchType = resolveType(callbackTerminator.switchType);
                            llvm::Value* value = generateExpr(callbackTerminator.operand.get());
                            auto* aggregateType =
                                value ? llvm::dyn_cast<llvm::StructType>(value->getType())
                                      : nullptr;
                            if (!switchType ||
                                (switchType->kind != TypeKind::Enum &&
                                 switchType->kind != TypeKind::Result) ||
                                !aggregateType || aggregateType->getNumElements() != 2) {
                                error("runtime Slot outlined switch has no sum layout");
                                mBuilder->CreateUnreachable();
                                continue;
                            }
                            llvm::Value* tag =
                                mBuilder->CreateExtractValue(value, {0}, "runtime.slot.switch.tag");
                            llvm::Value* payload = mBuilder->CreateExtractValue(
                                value, {1}, "runtime.slot.switch.payload");
                            auto* tagType = llvm::dyn_cast<llvm::IntegerType>(tag->getType());
                            auto* defaultTarget = outlinedEdgeTarget(
                                callbackTerminator.primary, "runtime.slot.switch.default.cleanup");
                            if (!tagType || !defaultTarget) {
                                error("runtime Slot outlined switch has no dispatch target");
                                mBuilder->CreateUnreachable();
                                continue;
                            }
                            const auto dispatchPoint = mBuilder->saveIP();
                            std::vector<llvm::BasicBlock*> caseTargets;
                            caseTargets.reserve(callbackTerminator.cases.size());
                            for (const auto& item : callbackTerminator.cases) {
                                std::vector<TypePtr> bindingTypes;
                                std::vector<uint64_t> bindingOffsets;
                                if (switchType->kind == TypeKind::Enum &&
                                    item.tag < switchType->variants.size()) {
                                    const auto& variant = switchType->variants[item.tag];
                                    bindingTypes = variant.fields;
                                    for (size_t field = 0; field < variant.fields.size(); ++field)
                                        bindingOffsets.push_back(
                                            luna::layout::variantFieldOffset(variant, field));
                                } else if (switchType->kind == TypeKind::Result &&
                                           switchType->typeArgs.size() == 2 && item.tag < 2) {
                                    bindingTypes.push_back(
                                        switchType->typeArgs[item.tag == 1 ? 0 : 1]);
                                    bindingOffsets.push_back(0);
                                } else {
                                    error("runtime Slot switch case is outside its sum type");
                                }
                                if (item.bindings.empty()) {
                                    caseTargets.push_back(outlinedEdgeTarget(
                                        item.edge, "runtime.slot.switch.case.cleanup"));
                                    continue;
                                }
                                auto* bridge = llvm::BasicBlock::Create(
                                    *mCtx, "runtime.slot.switch.case." + std::to_string(item.tag),
                                    callback);
                                caseTargets.push_back(bridge);
                                mBuilder->SetInsertPoint(bridge);
                                for (const auto cleanupId : item.edge.cleanups) {
                                    const auto* cleanup = graph.findCleanup(cleanupId);
                                    if (cleanup) emitCanonicalCleanup(*cleanup);
                                }
                                const size_t comparable =
                                    std::min(bindingTypes.size(), item.bindings.size());
                                for (size_t binding = 0; binding < comparable; ++binding) {
                                    const auto local = item.bindings[binding];
                                    if (local.empty() || local.value >= mCanonicalLocals.size() ||
                                        !mCanonicalLocals[local.value]) {
                                        error("runtime Slot switch binding has no storage");
                                        continue;
                                    }
                                    auto* storage = mCanonicalLocals[local.value];
                                    llvm::Value* field = unpackResultPayload(
                                        payload, bindingTypes[binding], bindingOffsets[binding]);
                                    mBuilder->CreateStore(
                                        coerceCallArgument(field, storage->getAllocatedType()),
                                        storage);
                                }
                                if (!mBuilder->GetInsertBlock()->getTerminator())
                                    mBuilder->CreateBr(callbackTarget(item.edge.target));
                            }
                            mBuilder->restoreIP(dispatchPoint);
                            auto* dispatch = mBuilder->CreateSwitch(
                                tag, defaultTarget, callbackTerminator.cases.size());
                            for (size_t index = 0; index < callbackTerminator.cases.size();
                                 ++index) {
                                if (!caseTargets[index]) continue;
                                dispatch->addCase(llvm::ConstantInt::get(
                                                      tagType, callbackTerminator.cases[index].tag),
                                                  caseTargets[index]);
                            }
                        } else if (callbackTerminator.kind == moon::TerminatorKind::RuntimeSlot) {
                            llvm::Value* returnStorage = mBuilder->CreateLoad(
                                mHelpers->ptrTy(),
                                mBuilder->CreateStructGEP(frameType, callback->getArg(0), 1,
                                                          "runtime.slot.nested.return.address"),
                                "runtime.slot.nested.return");
                            emitRuntimeSlot(*continuationBlock, callbackTerminator, callback,
                                            outlinedEdgeTarget, sourceReturnType, returnStorage,
                                            callbackOwnersFor(continuationBlock->id),
                                            [&]() {
                                                emitCaptureWriteback();
                                                mBuilder->CreateRet(llvm::ConstantInt::get(
                                                    mHelpers->i32Ty(),
                                                    LUNA_RUNTIME_FRAGMENT_CONTINUATION_ESCAPED_V1));
                                            });
                        } else if (callbackTerminator.kind == moon::TerminatorKind::Return) {
                            llvm::Value* returnValue = nullptr;
                            if (callbackTerminator.operand)
                                returnValue = generateExpr(callbackTerminator.operand.get());
                            if (!emitReturnCleanups(
                                    continuationBlock->id,
                                    callbackTerminator.exitCleanups,
                                    callbackOwnersFor(continuationBlock->id))) {
                                mBuilder->CreateUnreachable();
                                continue;
                            }
                            emitCaptureWriteback();
                            if (!sourceReturnType->isVoidTy()) {
                                auto* destination = mBuilder->CreateLoad(
                                    mHelpers->ptrTy(),
                                    mBuilder->CreateStructGEP(frameType, callback->getArg(0), 1,
                                                              "runtime.slot.return.address"),
                                    "runtime.slot.return");
                                if (!returnValue)
                                    error("runtime Slot outlined return has no value");
                                else
                                    mBuilder->CreateStore(
                                        coerceCallArgument(returnValue, sourceReturnType),
                                        destination);
                            }
                            mBuilder->CreateRet(llvm::ConstantInt::get(
                                mHelpers->i32Ty(), LUNA_RUNTIME_FRAGMENT_CONTINUATION_ESCAPED_V1));
                        } else {
                            mBuilder->CreateUnreachable();
                        }
                    }

                    mBuilder->SetInsertPoint(callbackCompletion);
                    emitCaptureWriteback();
                    mBuilder->CreateRet(llvm::ConstantInt::get(
                        mHelpers->i32Ty(), LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1));

                    mCanonicalLocals = std::move(savedCanonicalLocals);
                    mCanonicalLocalTypes = std::move(savedCanonicalLocalTypes);
                    mCanonicalDeviceBufferLengths = std::move(savedCanonicalDeviceBufferLengths);
                    mCurrentFunc = savedFunction;
                    mCurrentFragmentContext = savedFragmentContext;
                    mCurrentFunctionIsKernel = savedKernelMode;
                    mBuilder->restoreIP(parentInsertPoint);

                    auto* slotId =
                        mBuilder->CreateGlobalString(slot->symbolId.value, "runtime.slot.id");
                    auto* slotContract = mBuilder->CreateGlobalString(slot->contractId.value,
                                                                      "runtime.slot.contract");
                    auto* argumentsLayout = mBuilder->CreateGlobalString(
                        argumentRecord->abiLayoutId.value, "runtime.slot.arguments.layout");
                    auto dispatch = mModule->getOrInsertFunction(
                        "luna_runtime_fragment_dispatch_v1", mHelpers->i32Ty(), mHelpers->ptrTy(),
                        mHelpers->ptrTy(), mHelpers->ptrTy(), mHelpers->ptrTy(), mHelpers->i64Ty(),
                        mHelpers->i64Ty(), mHelpers->ptrTy(), mHelpers->ptrTy(), mHelpers->ptrTy());
                    auto* status = mBuilder->CreateCall(
                        dispatch,
                        {mCurrentFragmentContext, slotId, slotContract, argumentsLayout,
                         llvm::ConstantInt::get(mHelpers->i64Ty(), argumentRecord->valueSize),
                         llvm::ConstantInt::get(mHelpers->i64Ty(), argumentRecord->valueAlignment),
                         argumentStorage, callback, frame},
                        "runtime.slot.dispatch");
                    auto* failed = llvm::BasicBlock::Create(*mCtx, "runtime.slot.failed", func);
                    auto* escaped = llvm::BasicBlock::Create(*mCtx, "runtime.slot.escaped", func);
                    auto* nonCompleted =
                        llvm::BasicBlock::Create(*mCtx, "runtime.slot.non_completed", func);
                    mBuilder->CreateCondBr(
                        mBuilder->CreateICmpEQ(
                            status,
                            llvm::ConstantInt::get(mHelpers->i32Ty(),
                                                   LUNA_RUNTIME_FRAGMENT_DISPATCH_SUCCESS_V1)),
                        completion, nonCompleted);
                    mBuilder->SetInsertPoint(nonCompleted);
                    mBuilder->CreateCondBr(
                        mBuilder->CreateICmpEQ(
                            status, llvm::ConstantInt::get(
                                        mHelpers->i32Ty(),
                                        LUNA_RUNTIME_FRAGMENT_DISPATCH_CONTINUATION_ESCAPED_V1)),
                        escaped, failed);
                    mBuilder->SetInsertPoint(escaped);
                    if (propagateEscape)
                        propagateEscape();
                    else if (sourceReturnType->isVoidTy())
                        mBuilder->CreateRetVoid();
                    else
                        mBuilder->CreateRet(mBuilder->CreateLoad(sourceReturnType, escapedReturn,
                                                                 "runtime.slot.escaped.value"));
                    mBuilder->SetInsertPoint(failed);
                    for (auto it = inheritedRefContextOwners.rbegin();
                         it != inheritedRefContextOwners.rend(); ++it)
                        mHelpers->emitRuntimeFragmentContextDrop(
                            *mBuilder, *mModule, *it);
                    auto* trap = llvm::Intrinsic::getOrInsertDeclaration(mModule.get(),
                                                                         llvm::Intrinsic::trap);
                    mBuilder->CreateCall(trap);
                    mBuilder->CreateUnreachable();
                };
                emitRuntimeSlot(block, terminator, func, edgeTarget,
                                func->getReturnType(), nullptr,
                                contextOwnersFor(block.id),
                                {});
                break;
            }
            case moon::TerminatorKind::Switch: {
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
