#include "CodeGenerator.h"
#include "../runtime/FragmentPluginABI.h"

using moon::BlockStmt;
using moon::FragmentCardinality;
using moon::FragmentDecl;
using moon::FragmentKind;
using moon::SlotInvokeStmt;

void CodeGenerator::generateStructuredContinuation(BlockStmt* continuation,
                                                    llvm::Function* func) {
    llvm::Type* returnType = mCurrentFunc ? mCurrentFunc->getReturnType()
                                          : mHelpers->voidTy();
    std::vector<llvm::Type*> frameFields{
        llvm::Type::getInt8Ty(*mCtx) // 0 = normal, 1 = function return
    };
    if (!returnType->isVoidTy()) frameFields.push_back(returnType);

    auto* frameType = llvm::StructType::create(
        *mCtx, frameFields,
        "luna.continuation.frame." + std::to_string(mContinuationFrameCounter++));
    auto* storage = createEntryBlockAlloca(func, frameType, "continuation.frame");
    auto* status = mBuilder->CreateStructGEP(frameType, storage, 0,
                                             "continuation.status");
    mBuilder->CreateStore(llvm::ConstantInt::get(
                              llvm::Type::getInt8Ty(*mCtx), 0), status);

    auto* continuationEntry = llvm::BasicBlock::Create(
        *mCtx, "continuation.entry", func);
    auto* normalExit = llvm::BasicBlock::Create(
        *mCtx, "continuation.normal", func);
    auto* returnDispatch = llvm::BasicBlock::Create(
        *mCtx, "continuation.return.dispatch", func);
    auto* returnValue = llvm::BasicBlock::Create(
        *mCtx, "continuation.return", func);
    auto* invalidState = llvm::BasicBlock::Create(
        *mCtx, "continuation.invalid", func);

    ContinuationFrame frame{storage, frameType, returnDispatch, returnType};
    mBuilder->CreateBr(continuationEntry);
    mBuilder->SetInsertPoint(continuationEntry);
    auto* savedFragmentReturn = mCurrentFragmentReturn;
    mCurrentFragmentReturn = nullptr;
    mContinuationFrames.push_back(frame);
    generateBlock(continuation, func);
    mContinuationFrames.pop_back();
    mCurrentFragmentReturn = savedFragmentReturn;

    if (!mBuilder->GetInsertBlock()->getTerminator()) {
        auto* normalStatus = mBuilder->CreateStructGEP(
            frameType, storage, 0, "continuation.status");
        mBuilder->CreateStore(llvm::ConstantInt::get(
                                  llvm::Type::getInt8Ty(*mCtx), 0), normalStatus);
        mBuilder->CreateBr(normalExit);
    }

    mBuilder->SetInsertPoint(returnDispatch);
    auto* dispatchStatus = mBuilder->CreateLoad(
        llvm::Type::getInt8Ty(*mCtx),
        mBuilder->CreateStructGEP(frameType, storage, 0, "continuation.status"),
        "continuation.state");
    auto* isReturn = mBuilder->CreateICmpEQ(
        dispatchStatus, llvm::ConstantInt::get(llvm::Type::getInt8Ty(*mCtx), 1),
        "continuation.is_return");
    mBuilder->CreateCondBr(isReturn, returnValue, invalidState);

    mBuilder->SetInsertPoint(returnValue);
    if (returnType->isVoidTy()) {
        mBuilder->CreateRetVoid();
    } else {
        auto* result = mBuilder->CreateLoad(
            returnType,
            mBuilder->CreateStructGEP(frameType, storage, 1,
                                      "continuation.result"),
            "continuation.return.value");
        mBuilder->CreateRet(result);
    }

    mBuilder->SetInsertPoint(invalidState);
    auto abort = mModule->getOrInsertFunction("abort", mHelpers->voidTy());
    mBuilder->CreateCall(abort);
    mBuilder->CreateUnreachable();

    mBuilder->SetInsertPoint(normalExit);
}

void CodeGenerator::generateSlotInvoke(SlotInvokeStmt* slot, llvm::Function* func) {
    for (auto it = mDynamicApplyScopes.rbegin(); it != mDynamicApplyScopes.rend(); ++it) {
        auto dynamic = it->find(slot->name);
        if (dynamic != it->end()) {
            generateDynamicFragmentDispatch(dynamic->second, slot, func);
            return;
        }
    }
    FragmentDecl* fragment = nullptr;
    for (auto it = mApplyScopes.rbegin(); it != mApplyScopes.rend(); ++it) {
        auto applied = it->find(slot->name);
        if (applied != it->end()) {
            fragment = applied->second;
            break;
        }
    }
    if (!fragment && !slot->defaultFragment.empty()) {
        const std::string& key = slot->resolvedDefaultFragmentName.empty()
            ? slot->defaultFragment : slot->resolvedDefaultFragmentName;
        auto found = mFragments.find(key);
        if (found != mFragments.end()) fragment = found->second;
    }
    if (!fragment) {
        auto fallback = mSlotDefaults.find(slot->name);
        if (fallback != mSlotDefaults.end()) fragment = fallback->second;
    }
    if (!fragment) {
        generateBlock(slot->continuation.get(), func);
        return;
    }
    generateFragmentInline(fragment, slot, func);
}

void CodeGenerator::generateDynamicFragmentDispatch(
    const std::vector<FragmentDecl*>& candidates, SlotInvokeStmt* slot, llvm::Function* func) {
    if (candidates.empty()) {
        error("dynamic slot '" + slot->name + "' has no fragment candidates");
        return;
    }

    auto select = mModule->getOrInsertFunction(
        "rt_dynamic_fragment_select", mHelpers->ptrTy(), mHelpers->ptrTy(), mHelpers->ptrTy());
    auto matches = mModule->getOrInsertFunction(
        "rt_dynamic_fragment_matches", mHelpers->i32Ty(), mHelpers->ptrTy(), mHelpers->ptrTy());
    auto* slotName = mBuilder->CreateGlobalString(slot->name, "dynamic.slot");
    const std::string fallbackName = candidates.front()->name;
    auto* fallback = mBuilder->CreateGlobalString(fallbackName, "dynamic.fallback");
    auto* selected = mBuilder->CreateCall(select, {slotName, fallback}, "dynamic.fragment");

    auto* merge = llvm::BasicBlock::Create(*mCtx, "dynamic.apply.merge");
    bool reachesMerge = false;
    for (size_t index = 0; index < candidates.size(); ++index) {
        auto* candidateBlock = llvm::BasicBlock::Create(*mCtx, "dynamic.apply.candidate", func);
        auto* nextBlock = index + 1 < candidates.size()
            ? llvm::BasicBlock::Create(*mCtx, "dynamic.apply.next", func)
            : llvm::BasicBlock::Create(*mCtx, "dynamic.apply.unknown", func);
        auto* candidateName = mBuilder->CreateGlobalString(candidates[index]->name, "dynamic.candidate");
        auto* isCandidate = mBuilder->CreateICmpNE(
            mBuilder->CreateCall(matches, {selected, candidateName}, "dynamic.matches"),
            llvm::ConstantInt::get(mHelpers->i32Ty(), 0), "dynamic.selected");
        mBuilder->CreateCondBr(isCandidate, candidateBlock, nextBlock);

        mBuilder->SetInsertPoint(candidateBlock);
        generateFragmentInline(candidates[index], slot, func);
        if (!mBuilder->GetInsertBlock()->getTerminator()) {
            mBuilder->CreateBr(merge);
            reachesMerge = true;
        }

        mBuilder->SetInsertPoint(nextBlock);
    }

    // A name not present in the statically linked candidate set may still be
    // supplied by an external v1 plugin.  The plugin path is intentionally a
    // separate ABI: it receives only explicit argument addresses and returns
    // continue/abort.  It never receives the generated stack continuation.
    auto* externalCandidate = llvm::BasicBlock::Create(*mCtx, "dynamic.apply.external", func);
    auto* externalFailure = llvm::BasicBlock::Create(*mCtx, "dynamic.apply.external.failure", func);
    auto externalInvocation = generateExternalFragmentInvocation(slot, func, selected);
    auto invoke = mModule->getOrInsertFunction(
        "rt_fragment_plugin_invoke", mHelpers->i32Ty(),
        mHelpers->ptrTy(), mHelpers->ptrTy(), mHelpers->ptrTy(), mHelpers->ptrTy());
    auto* pluginAction = mBuilder->CreateCall(invoke, externalInvocation, "external.fragment.action");
    auto* continueBlock = llvm::BasicBlock::Create(*mCtx, "dynamic.apply.external.continue", func);
    auto* abortBlock = llvm::BasicBlock::Create(*mCtx, "dynamic.apply.external.abort", func);
    auto* isContinue = mBuilder->CreateICmpEQ(
        pluginAction, llvm::ConstantInt::get(mHelpers->i32Ty(), LUNA_FRAGMENT_PLUGIN_CONTINUE),
        "external.fragment.continue");
    mBuilder->CreateCondBr(isContinue, continueBlock, externalCandidate);

    mBuilder->SetInsertPoint(externalCandidate);
    auto* isAbort = mBuilder->CreateICmpEQ(
        pluginAction, llvm::ConstantInt::get(mHelpers->i32Ty(), LUNA_FRAGMENT_PLUGIN_ABORT),
        "external.fragment.abort");
    mBuilder->CreateCondBr(isAbort, abortBlock, externalFailure);
    mBuilder->SetInsertPoint(continueBlock);
    generateBlock(slot->continuation.get(), func);
    if (!mBuilder->GetInsertBlock()->getTerminator()) {
        mBuilder->CreateBr(merge);
        reachesMerge = true;
    }
    mBuilder->SetInsertPoint(abortBlock);
    mBuilder->CreateBr(merge);
    reachesMerge = true;
    mBuilder->SetInsertPoint(externalFailure);
    auto reportPluginFailure = mModule->getOrInsertFunction(
        "rt_fragment_plugin_report_error_and_abort", mHelpers->voidTy());
    mBuilder->CreateCall(reportPluginFailure);
    mBuilder->CreateUnreachable();
    if (reachesMerge) {
        merge->insertInto(func, nullptr);
        mBuilder->SetInsertPoint(merge);
    } else {
        delete merge;
    }
}

static std::string externalFragmentContract(
    const SlotInvokeStmt* slot, moon::TypeMaterializer& types) {
    std::string contract = "luna.slot." + slot->name + ".";
    contract += slot->acceptedKind == FragmentKind::Interceptor ? "interceptor" : "context";
    contract += slot->acceptedCardinality == FragmentCardinality::Many ? ".many" : ".once";
    if (const TypePtr structuralType = types.materialize(slot->structuralType)) {
        for (const auto& parameter : structuralType->paramTypes)
            contract += "." + (parameter ? parameter->toString() : "unknown");
    }
    return contract + ".v1";
}

std::array<llvm::Value*, 4> CodeGenerator::generateExternalFragmentInvocation(
    SlotInvokeStmt* slot, llvm::Function* func, llvm::Value* selected) {
    auto* slotName = mBuilder->CreateGlobalString(slot->name, "external.slot");
    auto* contract = mBuilder->CreateGlobalString(
        externalFragmentContract(slot, *mTypeMaterializer),
        "external.contract");
    auto* invocationType = llvm::StructType::get(
        *mCtx, {mHelpers->i32Ty(), mHelpers->ptrTy(), mHelpers->sizeTy()});
    auto* invocationStorage = createEntryBlockAlloca(func, invocationType, "external.invocation");

    llvm::Value* argumentArray = llvm::ConstantPointerNull::get(
        llvm::cast<llvm::PointerType>(mHelpers->ptrTy()));
    if (!slot->args.empty()) {
        auto* arrayType = llvm::ArrayType::get(mHelpers->ptrTy(), slot->args.size());
        auto* arrayStorage = createEntryBlockAlloca(func, arrayType, "external.args");
        for (size_t i = 0; i < slot->args.size(); ++i) {
            auto* value = generateExpr(slot->args[i].get());
            auto* valueStorage = createEntryBlockAlloca(func, value->getType(), "external.arg");
            mBuilder->CreateStore(value, valueStorage);
            auto* element = mBuilder->CreateInBoundsGEP(
                arrayType, arrayStorage,
                {llvm::ConstantInt::get(mHelpers->i32Ty(), 0),
                 llvm::ConstantInt::get(mHelpers->i32Ty(), i)}, "external.arg.ptr");
            mBuilder->CreateStore(valueStorage, element);
        }
        argumentArray = mBuilder->CreateInBoundsGEP(
            arrayType, arrayStorage,
            {llvm::ConstantInt::get(mHelpers->i32Ty(), 0),
             llvm::ConstantInt::get(mHelpers->i32Ty(), 0)}, "external.args.ptr");
    }
    mBuilder->CreateStore(llvm::ConstantInt::get(mHelpers->i32Ty(), LUNA_FRAGMENT_PLUGIN_ABI_V1),
                          mBuilder->CreateStructGEP(invocationType, invocationStorage, 0));
    mBuilder->CreateStore(argumentArray,
                          mBuilder->CreateStructGEP(invocationType, invocationStorage, 1));
    mBuilder->CreateStore(llvm::ConstantInt::get(mHelpers->sizeTy(), slot->args.size()),
                          mBuilder->CreateStructGEP(invocationType, invocationStorage, 2));

    // Return the four C-call arguments; the selected string is installed by
    // generateDynamicFragmentDispatch immediately before the call.
    return {slotName, selected, contract, invocationStorage};
}

void CodeGenerator::generateFragmentInline(FragmentDecl* fragment, SlotInvokeStmt* slot,
                                            llvm::Function* func) {
    auto savedLocals = mLocals;
    auto savedTypes = mLocalTypes;
    auto savedUpperBounds = mLocalKnownUpperBounds;
    auto* savedContinuation = mCurrentSlotContinuation;
    auto* savedFragmentExit = mCurrentFragmentExit;
    auto* savedFragmentReturn = mCurrentFragmentReturn;
    auto* fragmentExit = llvm::BasicBlock::Create(*mCtx, "fragment.exit", func);

    for (size_t i = 0; i < fragment->params.size(); ++i) {
        llvm::Value* value = nullptr;
        TypePtr type;
        if (i < slot->args.size()) {
            value = generateExpr(slot->args[i].get());
            type = resolveType(fragment->params[i].type);
        } else if (i < slot->resolvedParamNames.size()) {
            auto outer = mLocals.find(slot->resolvedParamNames[i]);
            if (outer != mLocals.end()) {
                value = mBuilder->CreateLoad(outer->second->getAllocatedType(), outer->second,
                                             fragment->params[i].name + ".slotarg");
                auto outerType = mLocalTypes.find(slot->resolvedParamNames[i]);
                if (outerType != mLocalTypes.end()) type = outerType->second;
            }
        }
        if (!value) {
            error("cannot materialize argument " + std::to_string(i + 1) +
                  " for fragment '" + fragment->name + "'");
            mLocals = std::move(savedLocals);
            mLocalTypes = std::move(savedTypes);
            mLocalKnownUpperBounds = std::move(savedUpperBounds);
            return;
        }
        auto* alloca = createEntryBlockAlloca(func, value->getType(),
                                              "__fragment_" + fragment->params[i].name);
        mBuilder->CreateStore(value, alloca);
        mLocals[fragment->params[i].name] = alloca;
        mLocalTypes[fragment->params[i].name] = type;
    }

    mCurrentSlotContinuation = slot->continuation.get();
    mCurrentFragmentExit = fragmentExit;
    mCurrentFragmentReturn = fragmentExit;
    generateBlock(fragment->body.get(), func);
    if (!mBuilder->GetInsertBlock()->getTerminator() &&
        fragment->kind == FragmentKind::Interceptor)
        generateBlock(slot->continuation.get(), func);
    if (!mBuilder->GetInsertBlock()->getTerminator()) mBuilder->CreateBr(fragmentExit);
    mBuilder->SetInsertPoint(fragmentExit);
    mCurrentSlotContinuation = savedContinuation;
    mCurrentFragmentExit = savedFragmentExit;
    mCurrentFragmentReturn = savedFragmentReturn;
    mLocals = std::move(savedLocals);
    mLocalTypes = std::move(savedTypes);
    mLocalKnownUpperBounds = std::move(savedUpperBounds);
}
