#include "CodeGenerator.h"

using moon::FunctionDecl;

// ─── Function generation ───────────────────────────────────────────

void CodeGenerator::generateFunctionBody(FunctionDecl* decl) {
    const std::string internalName = decl->generatedSymbolName.empty()
        ? decl->name : decl->generatedSymbolName;
    auto functionIt = mFunctions.find(internalName);
    auto func = functionIt != mFunctions.end()
        ? functionIt->second : mModule->getFunction(internalName);
    if (!func) return;

    const TypePtr returnType = resolveType(decl->returnType);
    llvm::Type* retLLVMType = returnType
        ? mHelpers->toLLVMType(returnType)
        : mHelpers->voidTy();

    if (!decl->body || decl->isExtern) return;

    mCurrentFunc = func;
    const bool savedKernelMode = mCurrentFunctionIsKernel;
    mCurrentFunctionIsKernel = decl->isKernel;
    mLocals.clear();
    mLocalTypes.clear();
    mArrayDropFlags.clear();
    mMaterializedIterators.clear();
    mLocalKnownUpperBounds.clear();
    mSlotDefaults.clear();
    mCurrentSlotContinuation = nullptr;
    mContinuationFrames.clear();
    mContinuationFrameCounter = 0;
    mCurrentFragmentReturn = nullptr;

    auto entryBB = llvm::BasicBlock::Create(*mCtx, "entry", func);
    mBuilder->SetInsertPoint(entryBB);

    // A generated application explicitly requests the ordinary host profile.
    // The Runtime helper preserves a descriptor already supplied by an
    // embedding host, so JIT and AOT share this entry policy safely.
    if (decl->name == "main") {
        auto installApplicationHost = mModule->getOrInsertFunction(
            "rt_install_application_host_services_v1", mHelpers->i32Ty());
        mBuilder->CreateCall(installApplicationHost);
    }

    // GPU init is only needed when kernels exist. Pure-CPU programs must never
    // reference rt_gpu_* symbols so they materialise on every platform (incl.
    // Windows CI).  When kernels ARE present, the generated main performs its
    // own runtime check so AOT binaries exit clearly when the backend is
    // misconfigured, rather than crashing on a null device pointer.
    if (decl->name == "main" && mProgram && mProgram->features.kernel) {
        auto initialize = mModule->getOrInsertFunction(
            "rt_gpu_initialize", mHelpers->i32Ty());
        auto reportFailure = mModule->getOrInsertFunction(
            "rt_gpu_report_initialization_error", mHelpers->voidTy());
        auto* initialized = mBuilder->CreateCall(initialize, {}, "gpu.initialized");
        auto* readyBB = llvm::BasicBlock::Create(*mCtx, "gpu.ready", func);
        auto* failedBB = llvm::BasicBlock::Create(*mCtx, "gpu.failed", func);
        auto* succeeded = mBuilder->CreateICmpNE(
            initialized, llvm::ConstantInt::get(mHelpers->i32Ty(), 0), "gpu.ready.result");
        mBuilder->CreateCondBr(succeeded, readyBB, failedBB);

        mBuilder->SetInsertPoint(failedBB);
        mBuilder->CreateCall(reportFailure);
        if (retLLVMType == mHelpers->voidTy()) mBuilder->CreateRetVoid();
        else if (retLLVMType->isIntegerTy())
            mBuilder->CreateRet(llvm::ConstantInt::get(retLLVMType, 1));
        else
            mBuilder->CreateRet(llvm::Constant::getNullValue(retLLVMType));

        mBuilder->SetInsertPoint(readyBB);
    }

    for (size_t i = 0; i < decl->params.size(); ++i) {
        auto& p = decl->params[i];
        auto* argVal = func->getArg(i);
        auto* alloca = createEntryBlockAlloca(func, argVal->getType(), p.name);
        mBuilder->CreateStore(argVal, alloca);
        mLocals[p.name] = alloca;
        mLocalTypes[p.name] = resolveType(p.type);
    }

    generateBlock(decl->body.get(), func);

    if (retLLVMType == mHelpers->voidTy() && !mBuilder->GetInsertBlock()->getTerminator()) {
        mBuilder->CreateRetVoid();
    }

    mCurrentFunc = nullptr;
    mCurrentFunctionIsKernel = savedKernelMode;
}
