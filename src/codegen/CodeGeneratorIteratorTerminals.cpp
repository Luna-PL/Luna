#include "CodeGenerator.h"

using moon::CallExpr;
using moon::Expr;
using moon::FieldAccessExpr;
using moon::IdentifierExpr;

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
        TypePtr reducerType = nullptr;
        const TypePtr iteratorOutputType = resolveType(
            call->iteratorOutputType);
        const TypePtr iteratorInputType = resolveType(
            call->iteratorInputType);
        const bool ownsAccumulator =
            defaultUsageForType(iteratorOutputType) !=
            luna::ownership::Usage::Copy;
        auto* accumulatorType = mHelpers->toLLVMType(iteratorOutputType);
        auto* itemType = mHelpers->toLLVMType(iteratorInputType);
        emitIteratorPipeline(plan, [&](llvm::Value* item) {
            auto* current = mBuilder->CreateLoad(
                accumulatorType, accumulator, "iter.fold.current");
            if (accumulatorInitialized)
                mBuilder->CreateStore(
                    llvm::ConstantInt::getFalse(
                        *mCtx),
                    accumulatorInitialized);
            auto* reduced = emitCallableInvocation(
                reducer, reducerType,
                {current, coerceCallArgument(item, itemType)},
                accumulatorType, "iter.fold.value");
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
            reducerType = resolveType(call->args[1]->type);
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
        TypePtr actionType = nullptr;
        auto* itemType = mHelpers->toLLVMType(
            resolveType(call->iteratorInputType));
        emitIteratorPipeline(plan, [&](llvm::Value* item) {
            emitCallableInvocation(
                action, actionType, {coerceCallArgument(item, itemType)},
                mHelpers->voidTy(), "iter.for_each");
        }, [&] {
            action = generateExpr(call->args[0].get());
            actionType = resolveType(call->args[0]->type);
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

llvm::Value* CodeGenerator::emitCallableInvocation(
    llvm::Value* callable, const TypePtr& callableType,
    llvm::ArrayRef<llvm::Value*> arguments, llvm::Type* returnType,
    const std::string& name) {
    if (callableType && callableType->kind == TypeKind::Closure) {
        llvm::Type* closureType = mHelpers->toLLVMType(callableType);
        auto* closureStorage = createEntryBlockAlloca(
            mCurrentFunc, closureType, name + ".closure");
        mBuilder->CreateStore(callable, closureStorage);
        llvm::Value* codePointer = mBuilder->CreateLoad(
            mHelpers->ptrTy(),
            mBuilder->CreateStructGEP(
                closureType, closureStorage, 0),
            name + ".code");
        llvm::Value* environmentPointer = closureStorage;
        std::vector<llvm::Type*> parameterTypes;
        std::vector<llvm::Value*> callArguments;
        parameterTypes.push_back(mHelpers->ptrTy());
        callArguments.push_back(environmentPointer);
        for (size_t index = 0;
             index < arguments.size(); ++index) {
            const TypePtr parameterType =
                index < callableType->paramTypes.size()
                    ? callableType->paramTypes[index]
                    : nullptr;
            llvm::Type* targetType = parameterType
                ? mHelpers->toLLVMType(parameterType)
                : (index + 1 < parameterTypes.size()
                       ? parameterTypes[index + 1]
                       : nullptr);
            parameterTypes.push_back(
                targetType ? targetType
                           : arguments[index]->getType());
            callArguments.push_back(
                targetType ? coerceCallArgument(
                                 arguments[index], targetType)
                           : arguments[index]);
        }
        auto* functionType = llvm::FunctionType::get(
            returnType, parameterTypes, false);
        return mBuilder->CreateCall(
            functionType, codePointer, callArguments,
            returnType->isVoidTy() ? "" : name);
    }
    std::vector<llvm::Type*> parameterTypes;
    std::vector<llvm::Value*> callArguments;
    for (auto* argument : arguments) {
        parameterTypes.push_back(argument->getType());
        callArguments.push_back(argument);
    }
    auto* functionType = llvm::FunctionType::get(
        returnType, parameterTypes, false);
    return mBuilder->CreateCall(
        functionType, callable, callArguments,
        returnType->isVoidTy() ? "" : name);
}
