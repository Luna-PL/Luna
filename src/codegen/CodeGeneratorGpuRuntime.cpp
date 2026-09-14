#include "CodeGenerator.h"

#include <cstring>

using moon::AddrOfExpr;
using moon::BorrowExpr;
using moon::Expr;
using moon::IdentifierExpr;
using moon::LaunchExpr;
using moon::MoveExpr;
llvm::Value* CodeGenerator::generateDeviceBufferValue(Expr* expr) {
    if (auto* move = dynamic_cast<MoveExpr*>(expr))
        return generateDeviceBufferValue(move->operand.get());
    if (auto* borrow = dynamic_cast<BorrowExpr*>(expr))
        return generateDeviceBufferValue(borrow->operand.get());
    if (auto* address = dynamic_cast<AddrOfExpr*>(expr))
        return generateDeviceBufferValue(address->operand.get());
    if (auto* id = dynamic_cast<IdentifierExpr*>(expr)) {
        if (!id->local.empty() && id->local.value < mCanonicalLocals.size() &&
            mCanonicalLocals[id->local.value]) {
            auto* storage = mCanonicalLocals[id->local.value];
            const TypePtr type = id->local.value < mCanonicalLocalTypes.size()
                ? mCanonicalLocalTypes[id->local.value] : nullptr;
            if (type && type->kind == TypeKind::Reference && type->inner &&
                type->inner->kind == TypeKind::DeviceBuffer) {
                auto* reference = mBuilder->CreateLoad(
                    storage->getAllocatedType(), storage,
                    id->name + ".device.reference");
                if (mCurrentFunctionIsKernel) {
                    llvm::Value* length = id->local.value <
                            mCanonicalDeviceBufferLengths.size()
                        ? mCanonicalDeviceBufferLengths[id->local.value]
                        : nullptr;
                    if (!length) {
                        error("kernel device-buffer reference has no ABI length");
                        length = llvm::ConstantInt::get(mHelpers->sizeTy(), 0);
                    }
                    llvm::Value* buffer = llvm::UndefValue::get(
                        mHelpers->deviceBufferTy());
                    buffer = mBuilder->CreateInsertValue(
                        buffer, reference, {0}, id->name + ".device.data");
                    return mBuilder->CreateInsertValue(
                        buffer, length, {1}, id->name + ".device.buffer");
                }
                return mBuilder->CreateLoad(
                    mHelpers->deviceBufferTy(), reference,
                    id->name + ".device.buffer");
            }
            if (type && type->kind == TypeKind::DeviceBuffer)
                return mBuilder->CreateLoad(
                    storage->getAllocatedType(), storage,
                    id->name + ".device.buffer");
        }
        auto local = mLocals.find(id->name);
        if (local != mLocals.end()) {
            TypePtr type;
            auto typed = mLocalTypes.find(id->name);
            if (typed != mLocalTypes.end()) type = typed->second;
            auto* value = mBuilder->CreateLoad(
                local->second->getAllocatedType(), local->second,
                id->name + ".devicearg");
            if (type && type->kind == TypeKind::Reference && type->inner &&
                type->inner->kind == TypeKind::DeviceBuffer)
                return mBuilder->CreateLoad(
                    mHelpers->deviceBufferTy(), value,
                    id->name + ".device.buffer");
            if (type && type->kind == TypeKind::DeviceBuffer)
                return value;
        }
    }
    llvm::Value* value = generateExpr(expr);
    if (value && value->getType() == mHelpers->deviceBufferTy()) return value;
    error("device-buffer expression has no bounds-carrying ABI value");
    return llvm::PoisonValue::get(mHelpers->deviceBufferTy());
}

llvm::Value* CodeGenerator::emitDeviceBufferIndexCheck(
    llvm::Value* index, llvm::Value* length) {
    index = coerceCallArgument(index, mHelpers->i32Ty());
    length = coerceCallArgument(length, mHelpers->sizeTy());
    auto* nonnegative = mBuilder->CreateICmpSGE(
        index, llvm::ConstantInt::get(mHelpers->i32Ty(), 0),
        "device.index.nonnegative");
    auto* widened = mBuilder->CreateZExt(
        index, mHelpers->sizeTy(), "device.index.wide");
    auto* inRange = mBuilder->CreateICmpULT(
        widened, length, "device.index.inrange");
    auto* valid = mBuilder->CreateAnd(
        nonnegative, inRange, "device.index.valid");
    auto* validBlock = llvm::BasicBlock::Create(
        *mCtx, "device.index.valid", mCurrentFunc);
    auto* invalidBlock = llvm::BasicBlock::Create(
        *mCtx, "device.index.invalid", mCurrentFunc);
    mBuilder->CreateCondBr(valid, validBlock, invalidBlock);
    mBuilder->SetInsertPoint(invalidBlock);
    auto* trap = llvm::Intrinsic::getOrInsertDeclaration(
        mModule.get(), llvm::Intrinsic::trap);
    mBuilder->CreateCall(trap);
    mBuilder->CreateUnreachable();
    mBuilder->SetInsertPoint(validBlock);
    return index;
}

llvm::Value* CodeGenerator::generateHostRawPointer(Expr* expr) {
    // A bulk transfer accepts &raw<i32> / &mut raw<i32>.  The reference is
    // to a local that stores the foreign pointer, so load once to recover the
    // actual host-memory address rather than passing the address of its slot.
    Expr* operand = expr;
    if (auto* borrow = dynamic_cast<BorrowExpr*>(operand)) operand = borrow->operand.get();
    else if (auto* address = dynamic_cast<AddrOfExpr*>(operand)) operand = address->operand.get();
    if (auto* id = dynamic_cast<IdentifierExpr*>(operand)) {
        auto local = mLocals.find(id->name);
        if (local != mLocals.end())
            return mBuilder->CreateLoad(local->second->getAllocatedType(), local->second,
                                        id->name + ".hostptr");
    }
    return generateExpr(expr);
}

void CodeGenerator::emitGpuOperationFailureCheck(llvm::Value* operationSucceeded,
                                                  llvm::Function* func) {
    auto* succeeded = mBuilder->CreateICmpNE(
        operationSucceeded, llvm::ConstantInt::get(operationSucceeded->getType(), 0),
        "gpu.operation.ok");
    auto* failedBB = llvm::BasicBlock::Create(*mCtx, "gpu.operation.failed", func);
    auto* continuedBB = llvm::BasicBlock::Create(*mCtx, "gpu.operation.continue", func);
    mBuilder->CreateCondBr(succeeded, continuedBB, failedBB);

    mBuilder->SetInsertPoint(failedBB);
    auto report = mModule->getOrInsertFunction(
        "rt_gpu_report_operation_error_and_abort", mHelpers->voidTy());
    mBuilder->CreateCall(report);
    mBuilder->CreateUnreachable();

    mBuilder->SetInsertPoint(continuedBB);
}

llvm::Value* CodeGenerator::generateLaunch(LaunchExpr* launch) {
    const auto* kernelDeclaration = resolveDeclaration(
        launch->kernelRef);
    const std::string symbol = kernelDeclaration
        ? kernelDeclaration->linkageName : std::string{};
    llvm::Function* callee = resolveFunction(launch->kernelRef);
    if (!callee || !mCurrentFunc) {
        error("cannot lower launch of unknown kernel '" + launch->kernelName + "'");
        return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
    }
    const TypePtr kernelType = kernelDeclaration
        ? resolveType(kernelDeclaration->type) : nullptr;
    const auto isDeviceBufferArgument = [&](size_t argumentIndex) {
        const size_t sourceParameterIndex = argumentIndex + 1;
        if (!kernelType || kernelType->kind != TypeKind::Function ||
            sourceParameterIndex >= kernelType->paramTypes.size())
            return false;
        const TypePtr& type = kernelType->paramTypes[sourceParameterIndex];
        return type && type->kind == TypeKind::Reference && type->inner &&
            type->inner->kind == TypeKind::DeviceBuffer;
    };

    auto* counter = createEntryBlockAlloca(mCurrentFunc, mHelpers->i32Ty(), "launch.index");
    auto* threads = coerceCallArgument(generateExpr(launch->threads.get()), mHelpers->i32Ty());
    mBuilder->CreateStore(llvm::ConstantInt::get(mHelpers->i32Ty(), 0), counter);

    // Both CUDA's Driver API and HIP's Module API receive an array of
    // addresses, not an array of values. A source device-buffer argument is
    // expanded to independent data-pointer and length slots; ordinary scalar
    // launch values are materialized into entry-block slots here.
    std::vector<llvm::Value*> driverParameters;
    driverParameters.push_back(counter);
    size_t llvmParameterIndex = 1;
    for (size_t i = 0; i < launch->args.size(); ++i) {
        if (isDeviceBufferArgument(i)) {
            auto* buffer = generateDeviceBufferValue(launch->args[i].get());
            auto* data = mBuilder->CreateExtractValue(
                buffer, {0}, "launch.device.data");
            auto* length = mBuilder->CreateExtractValue(
                buffer, {1}, "launch.device.length");
            auto* dataSlot = createEntryBlockAlloca(
                mCurrentFunc, mHelpers->ptrTy(), "launch.device.data.slot");
            auto* lengthSlot = createEntryBlockAlloca(
                mCurrentFunc, mHelpers->sizeTy(), "launch.device.length.slot");
            mBuilder->CreateStore(data, dataSlot);
            mBuilder->CreateStore(length, lengthSlot);
            driverParameters.push_back(dataSlot);
            driverParameters.push_back(lengthSlot);
            llvmParameterIndex += 2;
            continue;
        }
        llvm::Value* value = generateExpr(launch->args[i].get());
        llvm::Type* parameterType = llvmParameterIndex <
                callee->getFunctionType()->getNumParams()
            ? callee->getFunctionType()->getParamType(llvmParameterIndex)
            : value->getType();
        if (parameterType->isPointerTy() && value->getType()->isPointerTy()) {
            driverParameters.push_back(value);
        } else {
            value = coerceCallArgument(value, parameterType);
            auto* slot = createEntryBlockAlloca(mCurrentFunc, value->getType(), "launch.scalar");
            mBuilder->CreateStore(value, slot);
            driverParameters.push_back(slot);
        }
        ++llvmParameterIndex;
    }
    auto* parameterArrayType = llvm::ArrayType::get(mHelpers->ptrTy(), driverParameters.size());
    auto* parameterArray = createEntryBlockAlloca(mCurrentFunc, parameterArrayType, "launch.params");
    for (size_t i = 0; i < driverParameters.size(); ++i) {
        auto* destination = mBuilder->CreateInBoundsGEP(
            parameterArrayType, parameterArray,
            {llvm::ConstantInt::get(mHelpers->i32Ty(), 0),
             llvm::ConstantInt::get(mHelpers->i32Ty(), i)}, "launch.param");
        mBuilder->CreateStore(driverParameters[i], destination);
    }
    auto* parameterStart = mBuilder->CreateInBoundsGEP(
        parameterArrayType, parameterArray,
        {llvm::ConstantInt::get(mHelpers->i32Ty(), 0),
         llvm::ConstantInt::get(mHelpers->i32Ty(), 0)}, "launch.paramstart");

    const auto ptx = mKernelPTX.find(symbol);
    const std::string ptxSource = ptx == mKernelPTX.end() ? "" : ptx->second;
    auto* ptxValue = mBuilder->CreateGlobalString(ptxSource, "kernel.ptx");
    const auto hsaco = mKernelHSACO.find(symbol);
    llvm::Value* hsacoValue = nullptr;
    llvm::Value* hsacoSize = nullptr;
    if (hsaco == mKernelHSACO.end() || hsaco->second.empty()) {
        hsacoValue = mBuilder->CreateGlobalString("", "kernel.hsaco.empty");
        hsacoSize = llvm::ConstantInt::get(mHelpers->i64Ty(), 0);
    } else {
        const llvm::StringRef hsacoSource(hsaco->second.data(), hsaco->second.size());
        auto* hsacoData = llvm::ConstantDataArray::getString(*mCtx, hsacoSource, false);
        auto* hsacoGlobal = new llvm::GlobalVariable(
            *mModule, hsacoData->getType(), true, llvm::GlobalValue::PrivateLinkage,
            hsacoData, "kernel.hsaco");
        hsacoValue = mBuilder->CreateInBoundsGEP(
            hsacoData->getType(), hsacoGlobal,
            {llvm::ConstantInt::get(mHelpers->i32Ty(), 0),
             llvm::ConstantInt::get(mHelpers->i32Ty(), 0)}, "kernel.hsaco.data");
        hsacoSize = llvm::ConstantInt::get(mHelpers->i64Ty(), hsaco->second.size());
    }
    auto* kernelName = mBuilder->CreateGlobalString(symbol, "kernel.name");
    auto cudaBackend = mModule->getOrInsertFunction(
        "rt_gpu_backend_is_cuda", mHelpers->i32Ty());
    auto* useCuda = mBuilder->CreateICmpNE(
        mBuilder->CreateCall(cudaBackend, {}, "gpu.backend"),
        llvm::ConstantInt::get(mHelpers->i32Ty(), 0), "gpu.iscuda");
    auto rocmBackend = mModule->getOrInsertFunction(
        "rt_gpu_backend_is_rocm", mHelpers->i32Ty());
    auto* useRocm = mBuilder->CreateICmpNE(
        mBuilder->CreateCall(rocmBackend, {}, "gpu.backend"),
        llvm::ConstantInt::get(mHelpers->i32Ty(), 0), "gpu.isrocm");

    auto* cudaBB = llvm::BasicBlock::Create(*mCtx, "launch.cuda", mCurrentFunc);
    auto* backendBB = llvm::BasicBlock::Create(*mCtx, "launch.backend", mCurrentFunc);
    auto* rocmBB = llvm::BasicBlock::Create(*mCtx, "launch.rocm", mCurrentFunc);
    auto* condBB = llvm::BasicBlock::Create(*mCtx, "launch.sim.cond", mCurrentFunc);
    auto* bodyBB = llvm::BasicBlock::Create(*mCtx, "launch.sim.body", mCurrentFunc);
    auto* exitBB = llvm::BasicBlock::Create(*mCtx, "launch.sim.exit", mCurrentFunc);
    auto* mergeBB = llvm::BasicBlock::Create(*mCtx, "launch.merge", mCurrentFunc);
    mBuilder->CreateCondBr(useCuda, cudaBB, backendBB);

    mBuilder->SetInsertPoint(cudaBB);
    auto cudaLaunch = mModule->getOrInsertFunction(
        "rt_gpu_launch_ptx", mHelpers->i32Ty(), mHelpers->ptrTy(), mHelpers->ptrTy(),
        mHelpers->i32Ty(), mHelpers->ptrTy());
    auto* cudaEvent = mBuilder->CreateCall(cudaLaunch,
        {ptxValue, kernelName, threads, parameterStart}, "cuda.event");
    mBuilder->CreateBr(mergeBB);

    mBuilder->SetInsertPoint(backendBB);
    mBuilder->CreateCondBr(useRocm, rocmBB, condBB);

    mBuilder->SetInsertPoint(rocmBB);
    auto rocmLaunch = mModule->getOrInsertFunction(
        "rt_gpu_launch_hsaco", mHelpers->i32Ty(), mHelpers->ptrTy(),
        mHelpers->i64Ty(), mHelpers->ptrTy(), mHelpers->i32Ty(), mHelpers->ptrTy());
    auto* rocmEvent = mBuilder->CreateCall(rocmLaunch,
        {hsacoValue, hsacoSize, kernelName, threads, parameterStart}, "rocm.event");
    mBuilder->CreateBr(mergeBB);

    mBuilder->SetInsertPoint(condBB);
    auto* index = mBuilder->CreateLoad(mHelpers->i32Ty(), counter, "launch.index.value");
    auto* active = mBuilder->CreateICmpSLT(index, threads, "launch.active");
    mBuilder->CreateCondBr(active, bodyBB, exitBB);

    mBuilder->SetInsertPoint(bodyBB);
    std::vector<llvm::Value*> args;
    args.push_back(index);
    llvmParameterIndex = 1;
    for (size_t i = 0; i < launch->args.size(); ++i) {
        if (isDeviceBufferArgument(i)) {
            auto* buffer = generateDeviceBufferValue(launch->args[i].get());
            auto* data = mBuilder->CreateExtractValue(
                buffer, {0}, "sim.device.data");
            auto* length = mBuilder->CreateExtractValue(
                buffer, {1}, "sim.device.length");
            if (llvmParameterIndex < callee->getFunctionType()->getNumParams())
                data = coerceCallArgument(
                    data, callee->getFunctionType()->getParamType(
                              llvmParameterIndex));
            args.push_back(data);
            ++llvmParameterIndex;
            if (llvmParameterIndex < callee->getFunctionType()->getNumParams())
                length = coerceCallArgument(
                    length, callee->getFunctionType()->getParamType(
                                llvmParameterIndex));
            args.push_back(length);
            ++llvmParameterIndex;
            continue;
        }
        llvm::Value* value = generateExpr(launch->args[i].get());
        if (llvmParameterIndex < callee->getFunctionType()->getNumParams())
            value = coerceCallArgument(
                value, callee->getFunctionType()->getParamType(
                           llvmParameterIndex));
        args.push_back(value);
        ++llvmParameterIndex;
    }
    mBuilder->CreateCall(callee, args);
    auto* next = mBuilder->CreateAdd(index, llvm::ConstantInt::get(mHelpers->i32Ty(), 1),
                                     "launch.next");
    mBuilder->CreateStore(next, counter);
    mBuilder->CreateBr(condBB);

    mBuilder->SetInsertPoint(exitBB);
    mBuilder->CreateBr(mergeBB);

    mBuilder->SetInsertPoint(mergeBB);
    auto* event = mBuilder->CreatePHI(mHelpers->i32Ty(), 3, "launch.event");
    event->addIncoming(cudaEvent, cudaBB);
    event->addIncoming(rocmEvent, rocmBB);
    // Event value 1 denotes a completed simulator dispatch. `await` remains
    // explicit in source and becomes event synchronization in either vendor
    // branch above.
    event->addIncoming(llvm::ConstantInt::get(mHelpers->i32Ty(), 1), exitBB);
    return event;
}
