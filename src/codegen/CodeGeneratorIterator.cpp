#include "CodeGenerator.h"

using moon::CallExpr;
using moon::Expr;
using moon::FieldAccessExpr;
using moon::IdentifierExpr;

bool CodeGenerator::buildIteratorPlan(Expr* expr, IteratorPlan& plan) {
    if (!expr) return false;
    auto* call = dynamic_cast<CallExpr*>(expr);
    if (!call) {
        TypePtr sourceType = resolveType(expr->type);
        if (auto* id = dynamic_cast<IdentifierExpr*>(expr)) {
            auto materialized =
                mMaterializedIterators.find(
                    id->name);
            if (materialized !=
                mMaterializedIterators.end()) {
                plan = materialized->second.plan;
                plan.materializedName = id->name;
                return true;
            }
            auto found = mLocalTypes.find(id->name);
            if (found != mLocalTypes.end()) sourceType = found->second;
        }
        if (!sourceType ||
            (sourceType->kind != TypeKind::Array &&
             sourceType->kind != TypeKind::Slice))
            return false;
        plan.source = expr;
        plan.sourceType = sourceType;
        plan.mode = sourceType->kind == TypeKind::Slice
            ? IteratorMode::Shared : IteratorMode::Consuming;
        plan.itemType = plan.mode == IteratorMode::Shared
            ? Type::makeReference(sourceType->inner)
            : sourceType->inner;
        return true;
    }

    if (call->iteratorOp == IteratorOp::Range) {
        if (call->args.size() != 2) return false;
        plan.rangeStart = call->args[0].get();
        plan.rangeEnd = call->args[1].get();
        plan.itemType = TyI32;
        plan.mode = IteratorMode::Range;
        return true;
    }

    auto* member = dynamic_cast<FieldAccessExpr*>(call->callee.get());
    if (!member) return false;
    if (call->iteratorOp == IteratorOp::Iter ||
        call->iteratorOp == IteratorOp::IterMut ||
        call->iteratorOp == IteratorOp::IntoIter) {
        plan.source = member->object.get();
        const TypePtr callType = resolveType(call->type);
        if (callType && callType->kind == TypeKind::Iterator &&
            !callType->typeArgs.empty())
            plan.sourceType = callType->typeArgs.front();
        if (!plan.sourceType) {
            if (auto* id = dynamic_cast<IdentifierExpr*>(plan.source)) {
                auto found = mLocalTypes.find(id->name);
                if (found != mLocalTypes.end()) plan.sourceType = found->second;
            }
        }
        plan.itemType = resolveType(call->iteratorOutputType);
        plan.mode = call->iteratorOp == IteratorOp::Iter
            ? IteratorMode::Shared
            : (call->iteratorOp == IteratorOp::IterMut
                ? IteratorMode::Mutable : IteratorMode::Consuming);
        return plan.sourceType &&
               (plan.sourceType->kind == TypeKind::Array ||
                plan.sourceType->kind == TypeKind::Slice);
    }

    if (call->iteratorOp == IteratorOp::Map ||
        call->iteratorOp == IteratorOp::Filter ||
        call->iteratorOp == IteratorOp::Take) {
        if (!buildIteratorPlan(member->object.get(), plan)) return false;
        if (call->args.size() != 1) return false;
        plan.steps.push_back({
            call->iteratorOp,
            call->args.front().get(),
            resolveType(call->iteratorInputType),
            resolveType(call->iteratorOutputType)
        });
        plan.itemType = resolveType(call->iteratorOutputType);
        return true;
    }
    return false;
}

bool CodeGenerator::materializeIteratorBinding(
    const std::string& name,
    const IteratorPlan& plan) {
    MaterializedIterator state;
    state.plan = plan;
    state.plan.materializedName.clear();
    TypePtr sourceType = plan.sourceType;
    llvm::Value* initial = nullptr;
    state.ownsSource =
        plan.mode == IteratorMode::Consuming &&
        sourceType &&
        sourceType->kind == TypeKind::Array &&
        sourceType->inner &&
        defaultUsageForType(sourceType->inner) !=
            luna::ownership::Usage::Copy;

    if (plan.mode == IteratorMode::Range) {
        initial = coerceCallArgument(
            generateExpr(plan.rangeStart),
            mHelpers->i32Ty());
        state.limit = coerceCallArgument(
            generateExpr(plan.rangeEnd),
            mHelpers->i32Ty());
    } else {
        if (!sourceType) return false;
        if (sourceType->kind == TypeKind::Array) {
            if (auto* id =
                    dynamic_cast<IdentifierExpr*>(
                        plan.source);
                id && mLocals.count(id->name) &&
                plan.mode !=
                    IteratorMode::Consuming) {
                state.sourceData =
                    mLocals[id->name];
            } else {
                llvm::Value* value =
                    generateExpr(plan.source);
                auto* storage =
                    createEntryBlockAlloca(
                        mCurrentFunc,
                        value->getType(),
                        name + ".iterator.source");
                mBuilder->CreateStore(
                    value, storage);
                state.sourceData = storage;
                if (state.ownsSource) {
                    auto* flagsType =
                        llvm::ArrayType::get(
                            mHelpers->boolTy(),
                            sourceType->arrayLength);
                    state.sourceDropFlags =
                        createEntryBlockAlloca(
                            mCurrentFunc,
                            flagsType,
                            name +
                                ".iterator.source.flags");
                    for (uint64_t index = 0;
                         index <
                             sourceType->arrayLength;
                         ++index) {
                        auto* flag =
                            mBuilder->CreateInBoundsGEP(
                                flagsType,
                                state.sourceDropFlags,
                                {llvm::ConstantInt::get(
                                     mHelpers->i32Ty(),
                                     0),
                                 llvm::ConstantInt::get(
                                     mHelpers->i32Ty(),
                                     index)});
                        mBuilder->CreateStore(
                            llvm::ConstantInt::getTrue(
                                *mCtx),
                            flag);
                    }
                }
            }
            initial = llvm::ConstantInt::get(
                mHelpers->i32Ty(), 0);
            state.limit = llvm::ConstantInt::get(
                mHelpers->i32Ty(),
                sourceType->arrayLength);
        } else if (sourceType->kind ==
                   TypeKind::Slice) {
            llvm::Value* slice =
                generateExpr(plan.source);
            state.sourceData =
                mBuilder->CreateExtractValue(
                    slice, {0},
                    name + ".iterator.slice.data");
            state.limit = coerceCallArgument(
                mBuilder->CreateExtractValue(
                    slice, {1},
                    name +
                        ".iterator.slice.length"),
                mHelpers->i32Ty());
            initial = llvm::ConstantInt::get(
                mHelpers->i32Ty(), 0);
        } else {
            return false;
        }
    }

    state.steps.reserve(plan.steps.size());
    for (const auto& step : plan.steps) {
        RuntimeIteratorStep runtime;
        runtime.description = step;
        runtime.value =
            generateExpr(step.argument);
        if (step.op == IteratorOp::Take) {
            runtime.value = coerceCallArgument(
                runtime.value,
                mHelpers->i32Ty());
            runtime.remaining =
                createEntryBlockAlloca(
                    mCurrentFunc,
                    mHelpers->i32Ty(),
                    name +
                        ".iterator.take.remaining");
            mBuilder->CreateStore(
                runtime.value,
                runtime.remaining);
        }
        state.steps.push_back(runtime);
    }

    state.indexStorage =
        createEntryBlockAlloca(
            mCurrentFunc,
            mHelpers->i32Ty(),
            name + ".iterator.index");
    mBuilder->CreateStore(
        initial, state.indexStorage);
    mMaterializedIterators[name] =
        std::move(state);
    return true;
}

void CodeGenerator::emitIteratorPipeline(
    const IteratorPlan& plan,
    const std::function<void(llvm::Value*)>& consume,
    const std::function<void()>& prepareTerminal) {
    llvm::Value* sourceData = nullptr;
    llvm::Value* limit = nullptr;
    llvm::Value* initial = nullptr;
    llvm::AllocaInst* indexStorage =
        nullptr;
    std::vector<RuntimeIteratorStep> steps;
    size_t materializedStepCount = 0;
    llvm::AllocaInst* materializedSourceDropFlags =
        nullptr;
    TypePtr sourceType = plan.sourceType;
    auto materialized =
        plan.materializedName.empty()
        ? mMaterializedIterators.end()
        : mMaterializedIterators.find(
              plan.materializedName);
    if (materialized !=
        mMaterializedIterators.end()) {
        sourceData =
            materialized->second.sourceData;
        limit = materialized->second.limit;
        indexStorage =
            materialized->second.indexStorage;
        steps = materialized->second.steps;
        materializedStepCount = steps.size();
        materializedSourceDropFlags =
            materialized->second.sourceDropFlags;
    } else if (plan.mode == IteratorMode::Range) {
        initial = coerceCallArgument(generateExpr(plan.rangeStart), mHelpers->i32Ty());
        limit = coerceCallArgument(generateExpr(plan.rangeEnd), mHelpers->i32Ty());
    } else {
        if (!sourceType) {
            error("iterator source has no materialized array or slice type");
            return;
        }
        if (sourceType->kind == TypeKind::Array) {
            if (auto* id = dynamic_cast<IdentifierExpr*>(plan.source);
                id && mLocals.count(id->name) &&
                plan.mode != IteratorMode::Consuming) {
                sourceData = mLocals[id->name];
            } else {
                llvm::Value* value = generateExpr(plan.source);
                auto* storage = createEntryBlockAlloca(
                    mCurrentFunc, value->getType(),
                    plan.ownedStateName.empty()
                        ? "iter.array"
                        : plan.ownedStateName);
                mBuilder->CreateStore(value, storage);
                sourceData = storage;
                if (!plan.ownedStateName.empty()) {
                    mLocals[plan.ownedStateName] =
                        storage;
                    mLocalTypes[plan.ownedStateName] =
                        sourceType;
                    auto* flagsType =
                        llvm::ArrayType::get(
                            mHelpers->boolTy(),
                            sourceType->arrayLength);
                    auto* flags =
                        createEntryBlockAlloca(
                            mCurrentFunc, flagsType,
                            plan.ownedStateName +
                                ".flags");
                    for (uint64_t index = 0;
                         index <
                             sourceType->arrayLength;
                         ++index) {
                        auto* flag =
                            mBuilder->CreateInBoundsGEP(
                                flagsType, flags,
                                {llvm::ConstantInt::get(
                                     mHelpers->i32Ty(),
                                     0),
                                 llvm::ConstantInt::get(
                                     mHelpers->i32Ty(),
                                     index)});
                        mBuilder->CreateStore(
                            llvm::ConstantInt::getTrue(
                                *mCtx),
                            flag);
                    }
                    mArrayDropFlags[
                        plan.ownedStateName] = flags;
                }
            }
            initial = llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
            limit = llvm::ConstantInt::get(
                mHelpers->i32Ty(), sourceType->arrayLength);
        } else if (sourceType->kind == TypeKind::Slice) {
            llvm::Value* slice = generateExpr(plan.source);
            sourceData = mBuilder->CreateExtractValue(slice, {0}, "iter.slice.data");
            limit = coerceCallArgument(
                mBuilder->CreateExtractValue(slice, {1}, "iter.slice.length"),
                mHelpers->i32Ty());
            initial = llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
        } else {
            error("unsupported iterator source in code generation");
            return;
        }
    }

    // Preserve source-language evaluation order: source, adapter arguments
    // from left to right, then terminal arguments.
    steps.reserve(plan.steps.size());
    for (size_t stepIndex =
             materializedStepCount;
         stepIndex < plan.steps.size();
         ++stepIndex) {
        const auto& step =
            plan.steps[stepIndex];
        RuntimeIteratorStep runtime;
        runtime.description = step;
        runtime.value = generateExpr(step.argument);
        if (step.op == IteratorOp::Take) {
            runtime.value = coerceCallArgument(runtime.value, mHelpers->i32Ty());
            runtime.remaining = createEntryBlockAlloca(
                mCurrentFunc, mHelpers->i32Ty(), "iter.take.remaining");
            mBuilder->CreateStore(runtime.value, runtime.remaining);
        }
        steps.push_back(runtime);
    }
    if (prepareTerminal) prepareTerminal();

    if (!indexStorage) {
        indexStorage = createEntryBlockAlloca(
            mCurrentFunc, mHelpers->i32Ty(),
            "iter.index");
        mBuilder->CreateStore(
            initial, indexStorage);
    }

    auto* condition = llvm::BasicBlock::Create(
        *mCtx, "iter.condition", mCurrentFunc);
    auto* body = llvm::BasicBlock::Create(*mCtx, "iter.body");
    auto* next = llvm::BasicBlock::Create(*mCtx, "iter.next");
    auto* exit = llvm::BasicBlock::Create(*mCtx, "iter.exit");
    mBuilder->CreateBr(condition);

    mBuilder->SetInsertPoint(condition);
    llvm::Value* index = mBuilder->CreateLoad(
        mHelpers->i32Ty(), indexStorage, "iter.current");
    llvm::Value* hasItem = mBuilder->CreateICmpSLT(
        index, limit, "iter.has_item");
    mBuilder->CreateCondBr(hasItem, body, exit);

    exit->insertInto(mCurrentFunc, nullptr);
    next->insertInto(mCurrentFunc, exit);
    body->insertInto(mCurrentFunc, next);
    mBuilder->SetInsertPoint(body);

    llvm::Value* item = nullptr;
    TypePtr currentItemType =
        plan.mode == IteratorMode::Range
            ? TyI32
            : (plan.mode == IteratorMode::Shared
                   ? Type::makeReference(
                         sourceType->inner)
                   : (plan.mode ==
                              IteratorMode::Mutable
                          ? Type::makeReference(
                                sourceType->inner,
                                true)
                          : sourceType->inner));
    if (plan.mode == IteratorMode::Range) {
        item = index;
    } else {
        auto* elementType = mHelpers->toLLVMType(sourceType->inner);
        llvm::Value* element = nullptr;
        if (sourceType->kind == TypeKind::Array) {
            element = mBuilder->CreateInBoundsGEP(
                mHelpers->toLLVMType(sourceType), sourceData,
                {llvm::ConstantInt::get(mHelpers->i32Ty(), 0), index},
                "iter.array.element");
        } else {
            element = mBuilder->CreateGEP(
                elementType, sourceData, index, "iter.slice.element");
        }
        item = (plan.mode == IteratorMode::Shared ||
                plan.mode == IteratorMode::Mutable)
            ? element
            : mBuilder->CreateLoad(elementType, element, "iter.item");
    }

    // A consuming source transfers ownership to the current pipeline item
    // before any adapter runs. Every rejecting/truncating edge below must
    // therefore clean that current item rather than leaving the source bit
    // initialized.
    llvm::AllocaInst* transferredSourceFlags =
        materializedSourceDropFlags;
    if (!transferredSourceFlags &&
        !plan.ownedStateName.empty()) {
        auto flags =
            mArrayDropFlags.find(
                plan.ownedStateName);
        if (flags != mArrayDropFlags.end())
            transferredSourceFlags =
                flags->second;
        else {
            error("move-only consuming array iterator has no "
                  "initialization state");
            return;
        }
    }
    if (transferredSourceFlags) {
        auto* flag = mBuilder->CreateInBoundsGEP(
            transferredSourceFlags->
                getAllocatedType(),
            transferredSourceFlags,
            {llvm::ConstantInt::get(
                 mHelpers->i32Ty(), 0),
             index},
            "iter.item.initialized");
        mBuilder->CreateStore(
            llvm::ConstantInt::getFalse(*mCtx),
            flag);
    }

    for (auto& step : steps) {
        if (step.description.op == IteratorOp::Map) {
            auto* input = mHelpers->toLLVMType(step.description.inputType);
            auto* output = mHelpers->toLLVMType(step.description.outputType);
            auto* callableType = llvm::FunctionType::get(output, {input}, false);
            item = mBuilder->CreateCall(
                callableType, step.value,
                {coerceCallArgument(item, input)}, "iter.map");
            currentItemType =
                step.description.outputType;
        } else if (step.description.op == IteratorOp::Filter) {
            auto* input = mHelpers->toLLVMType(step.description.inputType);
            auto* callableType = llvm::FunctionType::get(
                mHelpers->boolTy(), {input}, false);
            auto* accepted = mBuilder->CreateCall(
                callableType, step.value,
                {coerceCallArgument(item, input)}, "iter.filter");
            auto* acceptedBlock = llvm::BasicBlock::Create(
                *mCtx, "iter.filter.accept", mCurrentFunc);
            if (defaultUsageForType(
                    currentItemType) ==
                luna::ownership::Usage::Copy) {
                mBuilder->CreateCondBr(
                    accepted, acceptedBlock, next);
            } else {
                auto* rejectedBlock =
                    llvm::BasicBlock::Create(
                        *mCtx,
                        "iter.filter.reject",
                        mCurrentFunc);
                mBuilder->CreateCondBr(
                    accepted, acceptedBlock,
                    rejectedBlock);
                mBuilder->SetInsertPoint(
                    rejectedBlock);
                emitOwnedPayloadCleanup(
                    item, currentItemType,
                    "iter.filter.rejected");
                if (!mBuilder->GetInsertBlock()->
                        getTerminator())
                    mBuilder->CreateBr(next);
            }
            mBuilder->SetInsertPoint(acceptedBlock);
        } else if (step.description.op == IteratorOp::Take) {
            auto* remaining = mBuilder->CreateLoad(
                mHelpers->i32Ty(), step.remaining, "iter.take.count");
            auto* canTake = mBuilder->CreateICmpSGT(
                remaining,
                llvm::ConstantInt::get(mHelpers->i32Ty(), 0),
                "iter.take.more");
            auto* takeBlock = llvm::BasicBlock::Create(
                *mCtx, "iter.take.item", mCurrentFunc);
            if (defaultUsageForType(
                    currentItemType) ==
                luna::ownership::Usage::Copy) {
                mBuilder->CreateCondBr(
                    canTake, takeBlock, exit);
            } else {
                auto* exhaustedBlock =
                    llvm::BasicBlock::Create(
                        *mCtx,
                        "iter.take.exhausted",
                        mCurrentFunc);
                mBuilder->CreateCondBr(
                    canTake, takeBlock,
                    exhaustedBlock);
                mBuilder->SetInsertPoint(
                    exhaustedBlock);
                emitOwnedPayloadCleanup(
                    item, currentItemType,
                    "iter.take.rejected");
                if (!mBuilder->GetInsertBlock()->
                        getTerminator())
                    mBuilder->CreateBr(exit);
            }
            mBuilder->SetInsertPoint(takeBlock);
            mBuilder->CreateStore(
                mBuilder->CreateSub(
                    remaining,
                    llvm::ConstantInt::get(mHelpers->i32Ty(), 1),
                    "iter.take.decrement"),
                step.remaining);
        }
    }

    consume(item);
    if (!mBuilder->GetInsertBlock()->getTerminator())
        mBuilder->CreateBr(next);

    mBuilder->SetInsertPoint(next);
    index = mBuilder->CreateLoad(
        mHelpers->i32Ty(), indexStorage, "iter.previous");
    mBuilder->CreateStore(
        mBuilder->CreateAdd(
            index, llvm::ConstantInt::get(mHelpers->i32Ty(), 1),
            "iter.advance"),
        indexStorage);
    mBuilder->CreateBr(condition);
    mBuilder->SetInsertPoint(exit);
    if (!plan.materializedName.empty()) {
        emitMaterializedIteratorCleanup(
            plan.materializedName);
        mMaterializedIterators.erase(
            plan.materializedName);
    }
}

llvm::Value* CodeGenerator::generateIteratorTerminal(CallExpr* call) {
    auto* member = dynamic_cast<FieldAccessExpr*>(call->callee.get());
    IteratorPlan plan;
    if (!member || !buildIteratorPlan(member->object.get(), plan)) {
        error("invalid iterator terminal recipe");
        return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
    }
    plan.ownedStateName =
        call->iteratorRecipeStateName;
    const auto finishOwnedRecipe = [&] {
        if (plan.ownedStateName.empty()) return;
        emitCleanup(
            plan.ownedStateName,
            luna::ownership::CleanupAction::ArrayDrop);
        mArrayDropFlags.erase(plan.ownedStateName);
        mLocals.erase(plan.ownedStateName);
        mLocalTypes.erase(plan.ownedStateName);
        mLocalKnownUpperBounds.erase(
            plan.ownedStateName);
    };

    if (call->iteratorOp == IteratorOp::Fold) {
        llvm::AllocaInst* accumulator = nullptr;
        llvm::AllocaInst* accumulatorInitialized =
            nullptr;
        llvm::Value* reducer = nullptr;
        const TypePtr iteratorOutputType = resolveType(
            call->iteratorOutputType);
        const TypePtr iteratorInputType = resolveType(
            call->iteratorInputType);
        const bool ownsAccumulator =
            defaultUsageForType(iteratorOutputType) !=
            luna::ownership::Usage::Copy;
        auto* accumulatorType = mHelpers->toLLVMType(iteratorOutputType);
        auto* itemType = mHelpers->toLLVMType(iteratorInputType);
        auto* reducerType = llvm::FunctionType::get(
            accumulatorType, {accumulatorType, itemType}, false);
        emitIteratorPipeline(plan, [&](llvm::Value* item) {
            auto* current = mBuilder->CreateLoad(
                accumulatorType, accumulator, "iter.fold.current");
            if (accumulatorInitialized)
                mBuilder->CreateStore(
                    llvm::ConstantInt::getFalse(
                        *mCtx),
                    accumulatorInitialized);
            auto* reduced = mBuilder->CreateCall(
                reducerType, reducer,
                {current, coerceCallArgument(item, itemType)},
                "iter.fold.value");
            mBuilder->CreateStore(reduced, accumulator);
            if (accumulatorInitialized)
                mBuilder->CreateStore(
                    llvm::ConstantInt::getTrue(
                        *mCtx),
                    accumulatorInitialized);
        }, [&] {
            llvm::Value* initial = generateExpr(call->args[0].get());
            accumulator = createEntryBlockAlloca(
                mCurrentFunc, accumulatorType, "iter.fold.accumulator");
            mBuilder->CreateStore(
                coerceCallArgument(initial, accumulatorType), accumulator);
            if (ownsAccumulator) {
                accumulatorInitialized =
                    createEntryBlockAlloca(
                        mCurrentFunc,
                        mHelpers->boolTy(),
                        "iter.fold.accumulator.initialized");
                mBuilder->CreateStore(
                    llvm::ConstantInt::getTrue(
                        *mCtx),
                    accumulatorInitialized);
            }
            reducer = generateExpr(call->args[1].get());
        });
        finishOwnedRecipe();
        llvm::Value* result = mBuilder->CreateLoad(
            accumulatorType, accumulator, "iter.fold.result");
        if (accumulatorInitialized)
            mBuilder->CreateStore(
                llvm::ConstantInt::getFalse(*mCtx),
                accumulatorInitialized);
        return result;
    }

    if (call->iteratorOp == IteratorOp::ForEach) {
        llvm::Value* action = nullptr;
        auto* itemType = mHelpers->toLLVMType(
            resolveType(call->iteratorInputType));
        auto* actionType = llvm::FunctionType::get(
            mHelpers->voidTy(), {itemType}, false);
        emitIteratorPipeline(plan, [&](llvm::Value* item) {
            mBuilder->CreateCall(
                actionType, action, {coerceCallArgument(item, itemType)});
        }, [&] {
            action = generateExpr(call->args[0].get());
        });
        finishOwnedRecipe();
        return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
    }

    if (call->iteratorOp == IteratorOp::Count) {
        llvm::AllocaInst* count = nullptr;
        emitIteratorPipeline(plan, [&](llvm::Value* item) {
            auto* current = mBuilder->CreateLoad(
                mHelpers->i32Ty(), count, "iter.count.current");
            mBuilder->CreateStore(
                mBuilder->CreateAdd(
                    current,
                    llvm::ConstantInt::get(mHelpers->i32Ty(), 1),
                    "iter.count.next"),
                count);
            emitOwnedPayloadCleanup(
                item, plan.itemType,
                "iter.count.item");
        }, [&] {
            count = createEntryBlockAlloca(
                mCurrentFunc, mHelpers->i32Ty(), "iter.count");
            mBuilder->CreateStore(
                llvm::ConstantInt::get(mHelpers->i32Ty(), 0), count);
        });
        finishOwnedRecipe();
        return mBuilder->CreateLoad(
            mHelpers->i32Ty(), count, "iter.count.result");
    }

    if (call->iteratorOp == IteratorOp::Collect) {
        llvm::Function* begin = resolveFunction(
            call->iteratorCollectBegin);
        llvm::Function* push = resolveFunction(
            call->iteratorCollectPush);
        llvm::Function* finish = resolveFunction(
            call->iteratorCollectFinish);
        const TypePtr builderTypeWitness = resolveType(
            call->iteratorCollectBuilderType);
        const TypePtr targetTypeWitness = resolveType(
            call->iteratorCollectTargetType);
        if (!begin || !push || !finish ||
            begin->arg_size() != 0 ||
            push->arg_size() != 2 ||
            finish->arg_size() != 1 ||
            !builderTypeWitness || !targetTypeWitness) {
            error("collect has an invalid lowered FromIterator protocol");
            return llvm::PoisonValue::get(
                mHelpers->toLLVMType(resolveType(call->type)));
        }

        llvm::AllocaInst* builderStorage = nullptr;
        llvm::Type* builderType =
            mHelpers->toLLVMType(builderTypeWitness);
        emitIteratorPipeline(plan, [&](llvm::Value* item) {
            llvm::Value* builderArgument = nullptr;
            if (builderTypeWitness->kind ==
                    TypeKind::Struct ||
                builderTypeWitness->kind ==
                    TypeKind::Record) {
                builderArgument = mBuilder->CreateLoad(
                    builderType, builderStorage,
                    "iter.collect.builder.borrow");
            } else {
                builderArgument = builderStorage;
            }
            mBuilder->CreateCall(
                push,
                {coerceCallArgument(
                     builderArgument,
                     push->getFunctionType()->
                         getParamType(0)),
                 coerceCallArgument(
                     item,
                     push->getFunctionType()->
                         getParamType(1))});
        }, [&] {
            llvm::Value* builder = mBuilder->CreateCall(
                begin, {}, "iter.collect.begin");
            builderStorage = createEntryBlockAlloca(
                mCurrentFunc, builderType,
                "iter.collect.builder");
            mBuilder->CreateStore(
                coerceCallArgument(builder, builderType),
                builderStorage);
        });
        finishOwnedRecipe();
        llvm::Value* builder = mBuilder->CreateLoad(
            builderType, builderStorage,
            "iter.collect.builder.finish");
        return mBuilder->CreateCall(
            finish,
            {coerceCallArgument(
                builder,
                finish->getFunctionType()->
                    getParamType(0))},
            "iter.collect.result");
    }

    error("non-terminal iterator recipe reached value code generation");
    return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
}
