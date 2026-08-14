#include "CodeGenerator.h"
#include "CodeGeneratorRangeAnalysis.h"
#include "../core/TypeLayout.h"

#include <optional>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Metadata.h>

using moon::AbortStmt;
using moon::ApplyStmt;
using moon::AwaitStmt;
using moon::BlockStmt;
using moon::ExprStmt;
using moon::ForStmt;
using moon::FragmentDecl;
using moon::FreeStmt;
using moon::HeapAllocExpr;
using moon::IdentifierExpr;
using moon::IfStmt;
using moon::LetStmt;
using moon::MatchStmt;
using moon::ResumeStmt;
using moon::ReturnStmt;
using moon::SlotDeclStmt;
using moon::SlotInvokeStmt;
using moon::Stmt;
using moon::WhileStmt;

namespace {

// Four-way unrolling exposes small scalar recurrences more effectively to the
// native backend. Restrict the hint to a single straight-line, call-free latch
// so effectful or control-flow-heavy loops retain LLVM's own cost decision.
bool shouldUnrollStraightLineLoop(const llvm::BasicBlock* body,
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
    // Very small single-recurrence loops generally gain nothing from forced
    // unrolling and can lengthen their dependency chain (notably nested-loop
    // kernels). Require enough independent work for the four-way hint.
    return instructionCount >= 24;
}

void setLoopUnrollCount(llvm::BranchInst* latch, llvm::LLVMContext& context,
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

// ─── Statement generation ──────────────────────────────────────────

void CodeGenerator::generateStmt(Stmt* stmt, llvm::Function* func) {
    if (auto* bs = dynamic_cast<BlockStmt*>(stmt)) {
        generateBlock(bs, func);
        return;
    }
    if (auto* ls = dynamic_cast<LetStmt*>(stmt)) {
        if (ls->materializesIteratorRecipe) {
            IteratorPlan plan;
            if (!buildIteratorPlan(
                    ls->initializer.get(), plan) ||
                !materializeIteratorBinding(
                    ls->name, plan)) {
                error("failed to materialize iterator "
                      "binding '" + ls->name + "'");
                return;
            }
            auto materialized =
                mMaterializedIterators.find(
                    ls->name);
            if (materialized ==
                    mMaterializedIterators.end() ||
                materialized->second.ownsSource !=
                    ls->materializedIteratorOwnsSource) {
                error("materialized iterator binding '" +
                      ls->name +
                      "' disagrees with its verified owning-source witness");
                return;
            }
            auto* token = createEntryBlockAlloca(
                func, mHelpers->ptrTy(),
                ls->name + ".iterator");
            mBuilder->CreateStore(
                llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(
                        mHelpers->ptrTy())),
                token);
            mLocals[ls->name] = token;
            mLocalTypes[ls->name] = resolveType(ls->type);
            mLocalKnownUpperBounds.erase(ls->name);
            return;
        }
        llvm::Value* initVal = generateExpr(ls->initializer.get());
        if (mBuilder->GetInsertBlock()->getTerminator()) return;
        llvm::Type* valType;
        mLocalKnownUpperBounds.erase(ls->name);

        if (dynamic_cast<HeapAllocExpr*>(ls->initializer.get())) {
            auto* varPtr = createEntryBlockAlloca(func, initVal->getType(), ls->name);
            mBuilder->CreateStore(initVal, varPtr);
            mLocals[ls->name] = varPtr;
            if (auto* heap = dynamic_cast<HeapAllocExpr*>(ls->initializer.get()))
                mLocalTypes[ls->name] = resolveType(
                    heap->type.empty() ? heap->allocatedType : heap->type);
        } else {
            valType = initVal->getType();
            if (valType->isVoidTy()) valType = mHelpers->i32Ty();
            auto* alloca = createEntryBlockAlloca(func, valType, ls->name);
            mBuilder->CreateStore(initVal, alloca);
            mLocals[ls->name] = alloca;
            // Inferred aggregate types (notably array literals) have no AST
            // annotation. Semantic analysis has already canonicalised them in
            // the symbol table; preserve that information for GEP lowering.
            if (!ls->type.empty())
                mLocalTypes[ls->name] = resolveType(ls->type);
        }
        if (auto upperBound = luna::codegen::knownArrayIndexUpperBound(
                ls->initializer.get(), mLocalKnownUpperBounds))
            mLocalKnownUpperBounds[ls->name] = *upperBound;
        return;
    }
    if (auto* slot = dynamic_cast<SlotDeclStmt*>(stmt)) {
        if (!slot->defaultFragmentRef.empty()) {
            auto* fragment = resolveFragment(slot->defaultFragmentRef);
            if (!fragment)
                error("unknown default fragment '" + slot->defaultFragment + "' for slot '" + slot->name + "'");
            else
                mSlotDefaults[slot->name] = fragment;
        }
        return;
    }
    if (auto* slot = dynamic_cast<SlotInvokeStmt*>(stmt)) {
        generateSlotInvoke(slot, func);
        return;
    }
    if (auto* apply = dynamic_cast<ApplyStmt*>(stmt)) {
        auto* fragment = resolveFragment(apply->fragmentRef);
        if (!fragment) {
            error("unknown fragment '" + apply->fragmentName + "' in apply");
            return;
        }
        if (apply->isDynamic) {
            std::vector<FragmentDecl*> candidates{fragment};
            for (const auto& reference : apply->alternativeFragmentRefs) {
                auto* alternative = resolveFragment(reference);
                if (!alternative) {
                    error("unknown dynamic fragment candidate '" +
                          reference.symbol.value + "'");
                    return;
                }
                candidates.push_back(alternative);
            }
            if (apply->body) {
                mApplyScopes.emplace_back();
                mDynamicApplyScopes.emplace_back();
                mApplyScopes.back()[apply->slotName] = fragment;
                mDynamicApplyScopes.back()[apply->slotName] = std::move(candidates);
                generateBlock(apply->body.get(), func);
                mDynamicApplyScopes.pop_back();
                mApplyScopes.pop_back();
            } else if (!mApplyScopes.empty()) {
                mApplyScopes.back()[apply->slotName] = fragment;
                if (mDynamicApplyScopes.empty()) mDynamicApplyScopes.emplace_back();
                mDynamicApplyScopes.back()[apply->slotName] = std::move(candidates);
            }
            return;
        }
        if (apply->body) {
            mApplyScopes.emplace_back();
            mApplyScopes.back()[apply->slotName] = fragment;
            generateBlock(apply->body.get(), func);
            mApplyScopes.pop_back();
        } else if (!mApplyScopes.empty()) {
            mApplyScopes.back()[apply->slotName] = fragment;
        }
        return;
    }
    if (dynamic_cast<ResumeStmt*>(stmt)) {
        if (!mCurrentSlotContinuation) {
            error("resume() reached code generation without an active slot");
            return;
        }
        generateStructuredContinuation(mCurrentSlotContinuation, func);
        return;
    }
    if (auto* abort = dynamic_cast<AbortStmt*>(stmt)) {
        if (!mCurrentFragmentExit) {
            error("abort() reached code generation without an active fragment");
            return;
        }
        if (!abort->cleanups.empty()) {
            for (const auto& cleanup : abort->cleanups)
                emitCleanup(cleanup.place, cleanup.action);
        } else {
            for (const auto& name : abort->autoFrees)
                emitCleanup(name, luna::ownership::CleanupAction::Deallocate);
        }
        mBuilder->CreateBr(mCurrentFragmentExit);
        return;
    }
    if (auto* await = dynamic_cast<AwaitStmt*>(stmt)) {
        // The simulator completes a launch before returning its event.  A
        // device launch or synchronization can fail, however, and must never
        // be mistaken for a completed event.  Make await the explicit runtime
        // error boundary for both backends.
        if (await->event) {
            auto* event = coerceCallArgument(generateExpr(await->event.get()), mHelpers->i32Ty());
            auto wait = mModule->getOrInsertFunction(
                "rt_gpu_await_event", mHelpers->i32Ty(), mHelpers->i32Ty());
            auto* completed = mBuilder->CreateCall(wait, {event}, "gpu.awaited");
            emitGpuOperationFailureCheck(completed, func);
        }
        return;
    }
    if (auto* rs = dynamic_cast<ReturnStmt*>(stmt)) {
        llvm::Value* retVal = rs->value ? generateExpr(rs->value.get()) : nullptr;
        if (mBuilder->GetInsertBlock()->getTerminator()) return;
        // Ownership checking records the heap values that are still live on
        // this exact return path.  Emitting cleanup here (rather than at a
        // surrounding block's textual end) also covers returns nested in
        // conditionals and loops without double-freeing values already moved
        // or explicitly freed by that path.
        if (!rs->cleanups.empty()) {
            for (const auto& cleanup : rs->cleanups)
                emitCleanup(cleanup.place, cleanup.action);
        } else {
            for (const auto& name : rs->autoFrees)
                emitCleanup(name, luna::ownership::CleanupAction::Deallocate);
        }
        if (mCurrentFragmentReturn) {
            // A fragment return ends only the fragment. It does not return
            // from the enclosing Luna function and it does not enter the
            // slot continuation.
            mBuilder->CreateBr(mCurrentFragmentReturn);
            return;
        }
        if (!mContinuationFrames.empty()) {
            // A return in a slot continuation must not return from the LLVM
            // block that happens to contain it. Store the result in the
            // explicit frame and transfer to its return dispatcher. The
            // context's post-resume path is therefore reachable only after a
            // normal continuation completion.
            const auto& frame = mContinuationFrames.back();
            auto* status = mBuilder->CreateStructGEP(frame.llvmType,
                                                     frame.storage, 0,
                                                     "continuation.status");
            if (retVal && frame.returnType && !frame.returnType->isVoidTy()) {
                retVal = coerceCallArgument(retVal, frame.returnType);
                auto* result = mBuilder->CreateStructGEP(frame.llvmType,
                                                         frame.storage, 1,
                                                         "continuation.result");
                mBuilder->CreateStore(retVal, result);
            }
            mBuilder->CreateStore(llvm::ConstantInt::get(
                                      llvm::Type::getInt8Ty(*mCtx), 1), status);
            mBuilder->CreateBr(frame.returnDispatch);
            return;
        }
        if (rs->value) {
            mBuilder->CreateRet(retVal);
        } else {
            mBuilder->CreateRetVoid();
        }
        return;
    }
    if (auto* es = dynamic_cast<ExprStmt*>(stmt)) {
        generateExpr(es->expr.get());
        return;
    }
    if (auto* is_ = dynamic_cast<IfStmt*>(stmt)) {
        llvm::Value* condVal = generateExpr(is_->cond.get());
        condVal = mBuilder->CreateICmpNE(
            condVal, llvm::ConstantInt::get(mHelpers->boolTy(), 0), "ifcond");

        auto* thenBB = llvm::BasicBlock::Create(*mCtx, "then", func);
        // Keep the merge block detached until a branch actually needs it.
        // If both branches return, inserting an empty merge block would leave
        // invalid host IR and make code after the if spuriously reachable.
        auto* mergeBB = llvm::BasicBlock::Create(*mCtx, "ifmerge");
        llvm::BasicBlock* elseBB = nullptr;

        if (is_->elseBranch) {
            elseBB = llvm::BasicBlock::Create(*mCtx, "else");
            mBuilder->CreateCondBr(condVal, thenBB, elseBB);
        } else {
            mBuilder->CreateCondBr(condVal, thenBB, mergeBB);
        }

        mBuilder->SetInsertPoint(thenBB);
        if (auto* thenBlock = dynamic_cast<BlockStmt*>(is_->thenBlock.get())) {
            generateBlock(thenBlock, func);
        }
        const bool thenFallsThrough = !mBuilder->GetInsertBlock()->getTerminator();
        if (thenFallsThrough)
            mBuilder->CreateBr(mergeBB);

        bool elseFallsThrough = true;
        if (elseBB) {
            elseBB->insertInto(func, nullptr);
            mBuilder->SetInsertPoint(elseBB);
            generateStmt(is_->elseBranch.get(), func);
            elseFallsThrough = !mBuilder->GetInsertBlock()->getTerminator();
            if (elseFallsThrough)
                mBuilder->CreateBr(mergeBB);
        }

        const bool mergeIsNeeded = !is_->elseBranch || thenFallsThrough || elseFallsThrough;

        if (mergeIsNeeded) {
            mergeBB->insertInto(func, nullptr);
            mBuilder->SetInsertPoint(mergeBB);
        } else {
            // Neither branch reaches a continuation.  Leave the builder on a
            // terminated block so generateBlock stops before unreachable
            // statements, and destroy the unused detached merge block.
            delete mergeBB;
            mBuilder->SetInsertPoint(elseBB);
        }
        return;
    }
    if (auto* match = dynamic_cast<MatchStmt*>(stmt)) {
        const TypePtr matchedType = resolveType(match->matchedType);
        if (!matchedType ||
            (matchedType->kind != TypeKind::Enum &&
             matchedType->kind != TypeKind::Result)) {
            error("match has no validated enum type");
            return;
        }
        llvm::Value* value = generateExpr(match->scrutinee.get());
        llvm::Value* tag =
            mBuilder->CreateExtractValue(value, {0}, "match.tag");
        llvm::Value* payload =
            mBuilder->CreateExtractValue(value, {1}, "match.payload");
        auto* tagType = llvm::dyn_cast<llvm::IntegerType>(tag->getType());
        if (!tagType) {
            error("match tag is not an integer");
            return;
        }

        auto* mergeBB = llvm::BasicBlock::Create(*mCtx, "match.merge");
        auto* invalidBB = llvm::BasicBlock::Create(
            *mCtx, "match.invalid_tag", func);
        auto* dispatch = mBuilder->CreateSwitch(
            tag, invalidBB, match->arms.size());
        bool anyFallsThrough = false;
        const auto outerLocals = mLocals;
        const auto outerTypes = mLocalTypes;
        const auto outerBounds = mLocalKnownUpperBounds;

        for (const auto& arm : match->arms) {
            auto* armBB = llvm::BasicBlock::Create(
                *mCtx, "match." + arm.variantName, func);
            dispatch->addCase(
                llvm::ConstantInt::get(tagType, arm.variantIndex),
                armBB);
            mBuilder->SetInsertPoint(armBB);
            mLocals = outerLocals;
            mLocalTypes = outerTypes;
            mLocalKnownUpperBounds = outerBounds;

            const TypeVariant* enumVariant = nullptr;
            if (matchedType->kind == TypeKind::Enum &&
                arm.variantIndex < matchedType->variants.size())
                enumVariant =
                    &matchedType->variants[arm.variantIndex];
            for (size_t index = 0;
                 index < arm.bindings.size() &&
                 index < arm.bindingTypes.size(); ++index) {
                uint64_t offset = 0;
                if (enumVariant)
                    offset = luna::layout::variantFieldOffset(
                        *enumVariant, index);
                llvm::Value* fieldValue = unpackResultPayload(
                    payload, resolveType(arm.bindingTypes[index]), offset);
                auto* storage = createEntryBlockAlloca(
                    func, mHelpers->toLLVMType(
                        resolveType(arm.bindingTypes[index])),
                    arm.bindings[index]);
                mBuilder->CreateStore(fieldValue, storage);
                mLocals[arm.bindings[index]] = storage;
                mLocalTypes[arm.bindings[index]] =
                    resolveType(arm.bindingTypes[index]);
                mLocalKnownUpperBounds.erase(arm.bindings[index]);
            }
            generateBlock(arm.body.get(), func);
            if (!mBuilder->GetInsertBlock()->getTerminator()) {
                mBuilder->CreateBr(mergeBB);
                anyFallsThrough = true;
            }
        }
        mLocals = outerLocals;
        mLocalTypes = outerTypes;
        mLocalKnownUpperBounds = outerBounds;

        mBuilder->SetInsertPoint(invalidBB);
        auto panic = mModule->getOrInsertFunction(
            "rt_panic_cstr", mHelpers->voidTy(), mHelpers->ptrTy());
        auto* message = mBuilder->CreateGlobalString(
            "invalid enum tag in match", "match.invalid_tag.message");
        mBuilder->CreateCall(panic, {message});
        mBuilder->CreateUnreachable();

        if (anyFallsThrough) {
            mergeBB->insertInto(func, nullptr);
            mBuilder->SetInsertPoint(mergeBB);
        } else {
            delete mergeBB;
            mBuilder->SetInsertPoint(invalidBB);
        }
        return;
    }
    if (auto* ws = dynamic_cast<WhileStmt*>(stmt)) {
        auto* condBB = llvm::BasicBlock::Create(*mCtx, "whilecond", func);
        auto* bodyBB = llvm::BasicBlock::Create(*mCtx, "whilebody");
        auto* exitBB = llvm::BasicBlock::Create(*mCtx, "whileexit");

        mBuilder->CreateBr(condBB);
        mBuilder->SetInsertPoint(condBB);
        llvm::Value* condVal = generateExpr(ws->cond.get());
        condVal = mBuilder->CreateICmpNE(
            condVal, llvm::ConstantInt::get(mHelpers->boolTy(), 0), "whilecond_val");
        mBuilder->CreateCondBr(condVal, bodyBB, exitBB);

        exitBB->insertInto(func, nullptr);
        bodyBB->insertInto(func, exitBB);
        mBuilder->SetInsertPoint(bodyBB);
        generateBlock(ws->body.get(), func);
        if (!mBuilder->GetInsertBlock()->getTerminator()) {
            auto* latch = mBuilder->CreateBr(condBB);
            if (mOptimizationLevel == LunaOptimizationLevel::O3 &&
                shouldUnrollStraightLineLoop(bodyBB, latch))
                setLoopUnrollCount(latch, *mCtx, 4);
        }

        mBuilder->SetInsertPoint(exitBB);
        return;
    }
    if (auto* fs = dynamic_cast<ForStmt*>(stmt)) {
        if (!fs->protocolNext.empty()) {
            auto* sourceName =
                dynamic_cast<IdentifierExpr*>(
                    fs->iterable.get());
            auto sourceLocal = sourceName
                ? mLocals.find(sourceName->name)
                : mLocals.end();
            llvm::Function* nextFunction =
                resolveFunction(fs->protocolNext);
            if (!sourceName ||
                sourceLocal == mLocals.end() ||
                !nextFunction) {
                error("for-loop Core Iterator protocol references "
                      "unmaterialized state or next method");
                return;
            }

            TypePtr elementType = fs->elementType.empty()
                ? TyI32 : resolveType(fs->elementType);
            auto* loopVariable = createEntryBlockAlloca(
                func, mHelpers->toLLVMType(elementType),
                fs->varName);
            llvm::Function* next = nextFunction;
            llvm::AllocaInst* stateStorage = nullptr;

            if (!fs->protocolInto.empty()) {
                llvm::Function* intoFunction =
                    resolveFunction(fs->protocolInto);
                if (!intoFunction ||
                    intoFunction->arg_size() != 1) {
                    error("for-loop Core IntoIterator protocol "
                          "references an invalid conversion method");
                    return;
                }
                llvm::Function* into = intoFunction;
                llvm::Value* source =
                    generateExpr(fs->iterable.get());
                source = coerceCallArgument(
                    source,
                    into->getFunctionType()->
                        getParamType(0));
                llvm::Value* state =
                    mBuilder->CreateCall(
                        into, {source},
                        "iterator.into_state");
                llvm::Type* stateType =
                    mHelpers->toLLVMType(
                        resolveType(fs->protocolIteratorType));
                stateStorage = createEntryBlockAlloca(
                    func, stateType,
                    fs->protocolStateName);
                mBuilder->CreateStore(
                    coerceCallArgument(state, stateType),
                    stateStorage);
                mLocals[fs->protocolStateName] =
                    stateStorage;
                mLocalTypes[fs->protocolStateName] =
                    resolveType(fs->protocolIteratorType);
                mLocalKnownUpperBounds.erase(
                    fs->protocolStateName);
            } else {
                stateStorage = sourceLocal->second;
            }

            llvm::AllocaInst* shadowedLocal = nullptr;
            TypePtr shadowedType;
            std::optional<uint64_t> shadowedBound;
            if (auto found = mLocals.find(fs->varName);
                found != mLocals.end())
                shadowedLocal = found->second;
            if (auto found =
                    mLocalTypes.find(fs->varName);
                found != mLocalTypes.end())
                shadowedType = found->second;
            if (auto found =
                    mLocalKnownUpperBounds.find(
                        fs->varName);
                found != mLocalKnownUpperBounds.end())
                shadowedBound = found->second;

            mLocals[fs->varName] = loopVariable;
            mLocalTypes[fs->varName] = elementType;
            mLocalKnownUpperBounds.erase(fs->varName);

            auto* nextBlock = llvm::BasicBlock::Create(
                *mCtx, "iterator.next", func);
            auto* someBlock = llvm::BasicBlock::Create(
                *mCtx, "iterator.some", func);
            auto* noneBlock = llvm::BasicBlock::Create(
                *mCtx, "iterator.none", func);
            auto* invalidBlock = llvm::BasicBlock::Create(
                *mCtx, "iterator.invalid_tag", func);
            mBuilder->CreateBr(nextBlock);

            mBuilder->SetInsertPoint(nextBlock);
            llvm::Value* receiver = stateStorage;
            const TypePtr protocolIteratorType = resolveType(
                fs->protocolIteratorType);
            if (protocolIteratorType &&
                (protocolIteratorType->kind ==
                     TypeKind::Struct ||
                 protocolIteratorType->kind ==
                     TypeKind::Record))
                receiver = mBuilder->CreateLoad(
                    stateStorage->getAllocatedType(),
                    stateStorage,
                    "iterator.state");
            if (next->arg_size() != 1) {
                error("Core Iterator::next has an invalid LLVM "
                      "function signature");
                return;
            }
            receiver = coerceCallArgument(
                receiver,
                next->getFunctionType()->getParamType(0));
            llvm::Value* option = mBuilder->CreateCall(
                next, {receiver}, "iterator.option");
            if (!option->getType()->isStructTy()) {
                error("Core Iterator::next did not lower to an "
                      "inline Option value");
                return;
            }
            llvm::Value* tag = mBuilder->CreateExtractValue(
                option, {0}, "iterator.tag");
            auto* tagType =
                llvm::dyn_cast<llvm::IntegerType>(
                    tag->getType());
            if (!tagType) {
                error("Core Iterator Option tag is not an integer");
                return;
            }
            auto* dispatch = mBuilder->CreateSwitch(
                tag, invalidBlock, 2);
            dispatch->addCase(
                llvm::ConstantInt::get(
                    tagType, fs->protocolNoneVariant),
                noneBlock);
            dispatch->addCase(
                llvm::ConstantInt::get(
                    tagType, fs->protocolSomeVariant),
                someBlock);

            mBuilder->SetInsertPoint(someBlock);
            llvm::Value* payload =
                mBuilder->CreateExtractValue(
                    option, {1}, "iterator.payload");
            llvm::Value* item = unpackResultPayload(
                payload, elementType);
            mBuilder->CreateStore(
                coerceCallArgument(
                    item,
                    loopVariable->getAllocatedType()),
                loopVariable);
            generateBlock(fs->body.get(), func);
            if (!mBuilder->GetInsertBlock()->getTerminator())
                mBuilder->CreateBr(nextBlock);

            mBuilder->SetInsertPoint(invalidBlock);
            auto panic = mModule->getOrInsertFunction(
                "rt_panic_cstr", mHelpers->voidTy(),
                mHelpers->ptrTy());
            auto* message = mBuilder->CreateGlobalString(
                "invalid Core Iterator Option tag",
                "iterator.invalid_tag.message");
            mBuilder->CreateCall(panic, {message});
            mBuilder->CreateUnreachable();

            mBuilder->SetInsertPoint(noneBlock);
            if (!fs->protocolInto.empty()) {
                if (fs->protocolStateNeedsCleanup)
                    emitCleanup(
                        fs->protocolStateName,
                        fs->protocolStateCleanup);
                mLocals.erase(
                    fs->protocolStateName);
                mLocalTypes.erase(
                    fs->protocolStateName);
                mLocalKnownUpperBounds.erase(
                    fs->protocolStateName);
            }
            if (shadowedLocal)
                mLocals[fs->varName] = shadowedLocal;
            else
                mLocals.erase(fs->varName);
            if (shadowedType)
                mLocalTypes[fs->varName] = shadowedType;
            else
                mLocalTypes.erase(fs->varName);
            if (shadowedBound)
                mLocalKnownUpperBounds[fs->varName] =
                    *shadowedBound;
            else
                mLocalKnownUpperBounds.erase(fs->varName);
            return;
        }

        IteratorPlan plan;
        if (!buildIteratorPlan(fs->iterable.get(), plan)) {
            error("for-loop iterable is not a materializable iterator recipe");
            return;
        }
        plan.ownedStateName =
            fs->recipeStateName;
        TypePtr elementType = fs->elementType.empty()
            ? plan.itemType : resolveType(fs->elementType);
        auto* loopVariable = createEntryBlockAlloca(
            func, mHelpers->toLLVMType(elementType), fs->varName);

        llvm::AllocaInst* shadowedLocal = nullptr;
        TypePtr shadowedType;
        std::optional<uint64_t> shadowedBound;
        if (auto found = mLocals.find(fs->varName); found != mLocals.end())
            shadowedLocal = found->second;
        if (auto found = mLocalTypes.find(fs->varName);
            found != mLocalTypes.end())
            shadowedType = found->second;
        if (auto found = mLocalKnownUpperBounds.find(fs->varName);
            found != mLocalKnownUpperBounds.end())
            shadowedBound = found->second;

        mLocals[fs->varName] = loopVariable;
        mLocalTypes[fs->varName] = elementType;
        mLocalKnownUpperBounds.erase(fs->varName);
        emitIteratorPipeline(plan, [&](llvm::Value* item) {
            mBuilder->CreateStore(
                coerceCallArgument(item, loopVariable->getAllocatedType()),
                loopVariable);
            generateBlock(fs->body.get(), func);
        });

        if (!plan.ownedStateName.empty()) {
            emitCleanup(
                plan.ownedStateName,
                luna::ownership::CleanupAction::ArrayDrop);
            mArrayDropFlags.erase(
                plan.ownedStateName);
            mLocals.erase(
                plan.ownedStateName);
            mLocalTypes.erase(
                plan.ownedStateName);
            mLocalKnownUpperBounds.erase(
                plan.ownedStateName);
        }

        if (shadowedLocal) mLocals[fs->varName] = shadowedLocal;
        else mLocals.erase(fs->varName);
        if (shadowedType) mLocalTypes[fs->varName] = shadowedType;
        else mLocalTypes.erase(fs->varName);
        if (shadowedBound)
            mLocalKnownUpperBounds[fs->varName] = *shadowedBound;
        else
            mLocalKnownUpperBounds.erase(fs->varName);
        return;
    }
    if (auto* freeStmt = dynamic_cast<FreeStmt*>(stmt)) {
        if (auto* id = dynamic_cast<IdentifierExpr*>(freeStmt->operand.get()))
            emitCleanup(id->name, freeStmt->action);
        else {
            llvm::Value* ptr = generateExpr(freeStmt->operand.get());
            emitLunaDeallocation(ptr, allocationTypeForExpr(freeStmt->operand.get()));
        }
        return;
    }
}

void CodeGenerator::generateBlock(BlockStmt* block, llvm::Function* func) {
    const auto savedLocals = mLocals;
    const auto savedLocalTypes = mLocalTypes;
    const auto savedKnownBounds = mLocalKnownUpperBounds;
    const auto savedArrayDropFlags = mArrayDropFlags;
    const auto savedMaterializedIterators = mMaterializedIterators;
    mApplyScopes.emplace_back();
    mDynamicApplyScopes.emplace_back();
    for (auto& stmt : block->stmts) {
        generateStmt(stmt.get(), func);
        if (mBuilder->GetInsertBlock()->getTerminator()) break;
    }
    mApplyScopes.pop_back();
    mDynamicApplyScopes.pop_back();
    mLocals = savedLocals;
    mLocalTypes = savedLocalTypes;
    mLocalKnownUpperBounds = savedKnownBounds;
    mArrayDropFlags = savedArrayDropFlags;
    mMaterializedIterators = savedMaterializedIterators;
}
