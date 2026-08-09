#include "CodeGenerator.h"
#include "../core/TypeLayout.h"
#include "../runtime/RuntimeABI.h"

#include <algorithm>

void CodeGenerator::emitLunaDeallocation(llvm::Value* pointer, const TypePtr& type) {
    auto rtDealloc = mModule->getOrInsertFunction(
        "rt_dealloc", mHelpers->voidTy(), mHelpers->ptrTy(),
        mHelpers->sizeTy(), mHelpers->sizeTy());
    const uint64_t size = type ? typeSize(type) : 0;
    const uint64_t alignment = type ? typeAlignment(type) : LUNA_DEFAULT_HOST_ALIGNMENT;
    mBuilder->CreateCall(rtDealloc, {
        mBuilder->CreateBitCast(pointer, mHelpers->ptrTy()),
        llvm::ConstantInt::get(mHelpers->sizeTy(), size),
        llvm::ConstantInt::get(mHelpers->sizeTy(), alignment),
    });
}

llvm::Value* CodeGenerator::packResultPayload(
    llvm::Value* value, const TypePtr& type, const TypePtr& resultType) {
    auto* resultLLVM = llvm::dyn_cast<llvm::StructType>(
        mHelpers->toLLVMType(resultType));
    if (!resultLLVM || resultLLVM->getNumElements() != 2) {
        error("Result payload has no validated tagged-union layout");
        return llvm::PoisonValue::get(
            llvm::ArrayType::get(mHelpers->i64Ty(), 1));
    }
    auto* payloadLLVM = resultLLVM->getElementType(1);
    auto* payloadStorage = createEntryBlockAlloca(
        mCurrentFunc, payloadLLVM, "result.payload.storage");
    mBuilder->CreateStore(
        llvm::Constant::getNullValue(payloadLLVM), payloadStorage);
    if (value && type && type->kind != TypeKind::Unit &&
        type->kind != TypeKind::Never) {
        auto* sourceStorage = createEntryBlockAlloca(
            mCurrentFunc, value->getType(), "result.payload.source");
        mBuilder->CreateStore(value, sourceStorage);
        const uint64_t storedSize =
            luna::layout::valueSize(type);
        mBuilder->CreateMemCpy(
            payloadStorage, llvm::Align(8),
            sourceStorage, llvm::Align(std::max<uint64_t>(
                1, std::min<uint64_t>(
                    8, luna::layout::valueAlignment(type)))),
            storedSize);
    }
    return mBuilder->CreateLoad(
        payloadLLVM, payloadStorage, "result.payload.bits");
}

llvm::Value* CodeGenerator::unpackResultPayload(
    llvm::Value* bits, const TypePtr& type, uint64_t byteOffset) {
    if (!type || type->kind == TypeKind::Unit)
        return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
    llvm::Type* target = mHelpers->toLLVMType(type);
    auto* payloadStorage = createEntryBlockAlloca(
        mCurrentFunc, bits->getType(), "result.payload.unpack");
    mBuilder->CreateStore(bits, payloadStorage);
    llvm::Value* fieldStorage = payloadStorage;
    if (byteOffset != 0)
        fieldStorage = mBuilder->CreateGEP(
            llvm::Type::getInt8Ty(*mCtx), payloadStorage,
            llvm::ConstantInt::get(
                mHelpers->sizeTy(), byteOffset),
            "payload.field");
    return mBuilder->CreateLoad(target, fieldStorage, "result.payload");
}

void CodeGenerator::emitOwnedPayloadCleanup(
    llvm::Value* value, const TypePtr& type, const std::string& label) {
    if (!value || !type ||
        defaultUsageForType(type) == luna::ownership::Usage::Copy)
        return;
    if (type->kind == TypeKind::Array &&
        type->inner) {
        for (uint64_t index = 0;
             index < type->arrayLength; ++index)
            emitOwnedPayloadCleanup(
                mBuilder->CreateExtractValue(
                    value,
                    {static_cast<unsigned>(index)},
                    label + ".element"),
                type->inner,
                label + "." +
                    std::to_string(index));
        return;
    }
    if (type->kind == TypeKind::Record) {
        for (size_t index = 0; index < type->fields.size(); ++index)
            emitOwnedPayloadCleanup(
                mBuilder->CreateExtractValue(
                    value,
                    {static_cast<unsigned>(index)},
                    label + ".field"),
                type->fields[index].type,
                label + "." + type->fields[index].name);
        return;
    }
    if (type->kind == TypeKind::Result && type->typeArgs.size() == 2) {
        llvm::Value* isOk =
            mBuilder->CreateExtractValue(value, {0}, label + ".is_ok");
        llvm::Value* bits =
            mBuilder->CreateExtractValue(value, {1}, label + ".payload");
        auto* okBlock = llvm::BasicBlock::Create(
            *mCtx, label + ".drop_ok", mCurrentFunc);
        auto* errorBlock = llvm::BasicBlock::Create(
            *mCtx, label + ".drop_err", mCurrentFunc);
        auto* continueBlock = llvm::BasicBlock::Create(
            *mCtx, label + ".dropped", mCurrentFunc);
        mBuilder->CreateCondBr(isOk, okBlock, errorBlock);
        mBuilder->SetInsertPoint(okBlock);
        emitOwnedPayloadCleanup(
            unpackResultPayload(bits, type->typeArgs[0]),
            type->typeArgs[0], label + ".ok");
        if (!mBuilder->GetInsertBlock()->getTerminator())
            mBuilder->CreateBr(continueBlock);
        mBuilder->SetInsertPoint(errorBlock);
        emitOwnedPayloadCleanup(
            unpackResultPayload(bits, type->typeArgs[1]),
            type->typeArgs[1], label + ".err");
        if (!mBuilder->GetInsertBlock()->getTerminator())
            mBuilder->CreateBr(continueBlock);
        mBuilder->SetInsertPoint(continueBlock);
        return;
    }
    if (type->kind == TypeKind::Enum) {
        llvm::Value* tag =
            mBuilder->CreateExtractValue(value, {0}, label + ".tag");
        llvm::Value* bits =
            mBuilder->CreateExtractValue(value, {1}, label + ".payload");
        auto* continueBlock = llvm::BasicBlock::Create(
            *mCtx, label + ".dropped", mCurrentFunc);
        auto* invalidBlock = llvm::BasicBlock::Create(
            *mCtx, label + ".invalid_tag", mCurrentFunc);
        auto* dispatch = mBuilder->CreateSwitch(
            tag, invalidBlock, type->variants.size());
        for (size_t variantIndex = 0;
             variantIndex < type->variants.size(); ++variantIndex) {
            const auto& variant = type->variants[variantIndex];
            auto* variantBlock = llvm::BasicBlock::Create(
                *mCtx, label + "." + variant.name, mCurrentFunc);
            dispatch->addCase(
                llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(*mCtx), variantIndex),
                variantBlock);
            mBuilder->SetInsertPoint(variantBlock);
            for (size_t fieldIndex = 0;
                 fieldIndex < variant.fields.size(); ++fieldIndex) {
                emitOwnedPayloadCleanup(
                    unpackResultPayload(
                        bits, variant.fields[fieldIndex],
                        luna::layout::variantFieldOffset(
                            variant, fieldIndex)),
                    variant.fields[fieldIndex],
                    label + "." + variant.name + "." +
                        std::to_string(fieldIndex));
            }
            if (!mBuilder->GetInsertBlock()->getTerminator())
                mBuilder->CreateBr(continueBlock);
        }
        mBuilder->SetInsertPoint(invalidBlock);
        auto panic = mModule->getOrInsertFunction(
            "rt_panic_cstr", mHelpers->voidTy(), mHelpers->ptrTy());
        auto* message = mBuilder->CreateGlobalString(
            "invalid inline enum tag", "enum.invalid_tag.message");
        mBuilder->CreateCall(panic, {message});
        mBuilder->CreateUnreachable();
        mBuilder->SetInsertPoint(continueBlock);
        return;
    }
    if (type->kind == TypeKind::Rc || type->kind == TypeKind::Arc) {
        const bool atomic = type->kind == TypeKind::Arc;
        auto release = mModule->getOrInsertFunction(
            atomic ? "rt_arc_release" : "rt_rc_release",
            mHelpers->i32Ty(), mHelpers->ptrTy());
        auto* lastCount = mBuilder->CreateCall(
            release, {coerceCallArgument(value, mHelpers->ptrTy())},
            label + ".last");
        auto* last = mBuilder->CreateICmpNE(
            lastCount, llvm::ConstantInt::get(mHelpers->i32Ty(), 0));
        auto* finalBlock = llvm::BasicBlock::Create(
            *mCtx, label + ".final", mCurrentFunc);
        auto* continueBlock = llvm::BasicBlock::Create(
            *mCtx, label + ".released", mCurrentFunc);
        mBuilder->CreateCondBr(last, finalBlock, continueBlock);
        mBuilder->SetInsertPoint(finalBlock);
        TypePtr payload = type->inner;
        if (payload && payload->sysmeta.resource.needsDrop) {
            const std::string& symbol = payload->sysmeta.abi.dropGlueSymbol;
            auto drop = mFunctions.find(symbol);
            if (symbol.empty() || drop == mFunctions.end()) {
                error("shared Result payload has no compiler-validated drop glue");
            } else {
                mBuilder->CreateCall(drop->second, {
                    coerceCallArgument(
                        value, drop->second->getFunctionType()->getParamType(0))
                });
            }
        }
        auto deallocate = mModule->getOrInsertFunction(
            "rt_shared_dealloc", mHelpers->voidTy(), mHelpers->ptrTy());
        mBuilder->CreateCall(
            deallocate, {coerceCallArgument(value, mHelpers->ptrTy())});
        mBuilder->CreateBr(continueBlock);
        mBuilder->SetInsertPoint(continueBlock);
        return;
    }
    if (!value->getType()->isPointerTy()) {
        error("move-only Result payload '" + type->toString() +
              "' is not representable by the initial cleanup ABI");
        return;
    }
    if (type->sysmeta.resource.needsDrop) {
        const std::string& symbol = type->sysmeta.abi.dropGlueSymbol;
        auto drop = mFunctions.find(symbol);
        if (symbol.empty() || drop == mFunctions.end()) {
            error("Result payload has no compiler-validated drop glue");
        } else {
            mBuilder->CreateCall(drop->second, {
                coerceCallArgument(
                    value, drop->second->getFunctionType()->getParamType(0))
            });
        }
    }
    emitLunaDeallocation(value, type);
}

void CodeGenerator::emitMaterializedIteratorCleanup(
    const std::string& name) {
    auto materialized =
        mMaterializedIterators.find(name);
    if (materialized ==
        mMaterializedIterators.end() ||
        !materialized->second.ownsSource)
        return;
    auto& state = materialized->second;
    TypePtr sourceType = state.plan.sourceType;
    if (!sourceType ||
        sourceType->kind != TypeKind::Array ||
        !sourceType->inner ||
        !state.sourceData ||
        !state.sourceDropFlags) {
        error("owning materialized iterator '" +
              name +
              "' has no validated source drop state");
        return;
    }
    auto* arrayType =
        mHelpers->toLLVMType(sourceType);
    auto* elementType =
        mHelpers->toLLVMType(sourceType->inner);
    for (uint64_t index = 0;
         index < sourceType->arrayLength;
         ++index) {
        auto* flagPointer =
            mBuilder->CreateInBoundsGEP(
                state.sourceDropFlags->
                    getAllocatedType(),
                state.sourceDropFlags,
                {llvm::ConstantInt::get(
                     mHelpers->i32Ty(), 0),
                 llvm::ConstantInt::get(
                     mHelpers->i32Ty(), index)},
                name + ".iterator.source.flag");
        llvm::Value* initialized =
            mBuilder->CreateLoad(
                mHelpers->boolTy(),
                flagPointer,
                name +
                    ".iterator.source.initialized");
        auto* dropBlock =
            llvm::BasicBlock::Create(
                *mCtx,
                name + ".iterator.source.drop." +
                    std::to_string(index),
                mCurrentFunc);
        auto* continueBlock =
            llvm::BasicBlock::Create(
                *mCtx,
                name + ".iterator.source.after." +
                    std::to_string(index),
                mCurrentFunc);
        mBuilder->CreateCondBr(
            initialized, dropBlock,
            continueBlock);
        mBuilder->SetInsertPoint(dropBlock);
        auto* elementPointer =
            mBuilder->CreateInBoundsGEP(
                arrayType,
                state.sourceData,
                {llvm::ConstantInt::get(
                     mHelpers->i32Ty(), 0),
                 llvm::ConstantInt::get(
                     mHelpers->i32Ty(), index)},
                name +
                    ".iterator.source.element");
        llvm::Value* element =
            mBuilder->CreateLoad(
                elementType,
                elementPointer,
                name +
                    ".iterator.source.value");
        mBuilder->CreateStore(
            llvm::ConstantInt::getFalse(*mCtx),
            flagPointer);
        emitOwnedPayloadCleanup(
            element, sourceType->inner,
            name + ".iterator.source." +
                std::to_string(index));
        if (!mBuilder->GetInsertBlock()->
                getTerminator())
            mBuilder->CreateBr(
                continueBlock);
        mBuilder->SetInsertPoint(
            continueBlock);
    }
}

void CodeGenerator::emitCleanup(
    const std::string& place, luna::ownership::CleanupAction action) {
    if (mMaterializedIterators.count(place)) {
        emitMaterializedIteratorCleanup(place);
        return;
    }
    auto local = mLocals.find(place);
    if (local == mLocals.end()) {
        error("cleanup references unknown local '" + place + "'");
        return;
    }
    llvm::Value* pointer = mBuilder->CreateLoad(
        local->second->getAllocatedType(), local->second, place + ".cleanup");
    TypePtr type;
    auto typed = mLocalTypes.find(place);
    if (typed != mLocalTypes.end()) type = typed->second;
    if (action == luna::ownership::CleanupAction::ResultDrop) {
        if (!type || type->kind != TypeKind::Result ||
            type->typeArgs.size() != 2) {
            error("Result cleanup for '" + place +
                  "' has no validated Result type");
            return;
        }
        llvm::Value* isOk =
            mBuilder->CreateExtractValue(pointer, {0}, place + ".is_ok");
        llvm::Value* bits =
            mBuilder->CreateExtractValue(pointer, {1}, place + ".payload");
        auto* okBlock = llvm::BasicBlock::Create(
            *mCtx, place + ".drop_ok", mCurrentFunc);
        auto* errorBlock = llvm::BasicBlock::Create(
            *mCtx, place + ".drop_err", mCurrentFunc);
        auto* continueBlock = llvm::BasicBlock::Create(
            *mCtx, place + ".dropped", mCurrentFunc);
        mBuilder->CreateCondBr(isOk, okBlock, errorBlock);
        mBuilder->SetInsertPoint(okBlock);
        emitOwnedPayloadCleanup(
            unpackResultPayload(bits, type->typeArgs[0]),
            type->typeArgs[0], place + ".ok");
        if (!mBuilder->GetInsertBlock()->getTerminator())
            mBuilder->CreateBr(continueBlock);
        mBuilder->SetInsertPoint(errorBlock);
        emitOwnedPayloadCleanup(
            unpackResultPayload(bits, type->typeArgs[1]),
            type->typeArgs[1], place + ".err");
        if (!mBuilder->GetInsertBlock()->getTerminator())
            mBuilder->CreateBr(continueBlock);
        mBuilder->SetInsertPoint(continueBlock);
        return;
    }
    if (action == luna::ownership::CleanupAction::EnumDrop) {
        if (!type || type->kind != TypeKind::Enum) {
            error("enum cleanup for '" + place +
                  "' has no validated enum type");
            return;
        }
        emitOwnedPayloadCleanup(
            pointer, type, place + ".enum");
        return;
    }
    if (action == luna::ownership::CleanupAction::ArrayDrop) {
        if (!type || type->kind != TypeKind::Array ||
            !type->inner) {
            error("array cleanup for '" + place +
                  "' has no validated array type");
            return;
        }
        auto flags = mArrayDropFlags.find(place);
        for (uint64_t index = 0;
             index < type->arrayLength; ++index) {
            llvm::Value* element =
                mBuilder->CreateExtractValue(
                    pointer,
                    {static_cast<unsigned>(index)},
                    place + ".element");
            if (flags == mArrayDropFlags.end()) {
                emitOwnedPayloadCleanup(
                    element, type->inner,
                    place + "." +
                        std::to_string(index));
                continue;
            }
            auto* dropBlock =
                llvm::BasicBlock::Create(
                    *mCtx,
                    place + ".drop." +
                        std::to_string(index),
                    mCurrentFunc);
            auto* continueBlock =
                llvm::BasicBlock::Create(
                    *mCtx,
                    place + ".after." +
                        std::to_string(index),
                    mCurrentFunc);
            auto* flagPointer =
                mBuilder->CreateInBoundsGEP(
                    flags->second->getAllocatedType(),
                    flags->second,
                    {llvm::ConstantInt::get(
                         mHelpers->i32Ty(), 0),
                     llvm::ConstantInt::get(
                         mHelpers->i32Ty(), index)},
                    place + ".flag");
            llvm::Value* initialized =
                mBuilder->CreateLoad(
                    mHelpers->boolTy(), flagPointer,
                    place + ".initialized");
            mBuilder->CreateCondBr(
                initialized, dropBlock,
                continueBlock);
            mBuilder->SetInsertPoint(dropBlock);
            emitOwnedPayloadCleanup(
                element, type->inner,
                place + "." +
                    std::to_string(index));
            if (!mBuilder->GetInsertBlock()->
                    getTerminator())
                mBuilder->CreateBr(continueBlock);
            mBuilder->SetInsertPoint(continueBlock);
        }
        return;
    }
    if (action == luna::ownership::CleanupAction::RecordDrop) {
        if (!type || type->kind != TypeKind::Record) {
            error("record cleanup for '" + place +
                  "' has no validated record type");
            return;
        }
        emitOwnedPayloadCleanup(pointer, type, place + ".record");
        return;
    }
    if (!pointer->getType()->isPointerTy()) return;
    if (action == luna::ownership::CleanupAction::RcRelease ||
        action == luna::ownership::CleanupAction::ArcRelease) {
        const bool atomic =
            action == luna::ownership::CleanupAction::ArcRelease;
        auto release = mModule->getOrInsertFunction(
            atomic ? "rt_arc_release" : "rt_rc_release",
            mHelpers->i32Ty(), mHelpers->ptrTy());
        auto* isLastCount = mBuilder->CreateCall(
            release, {mBuilder->CreateBitCast(pointer, mHelpers->ptrTy())},
            atomic ? "arc.last" : "rc.last");
        auto* isLast = mBuilder->CreateICmpNE(
            isLastCount, llvm::ConstantInt::get(mHelpers->i32Ty(), 0));
        auto* finalBlock = llvm::BasicBlock::Create(
            *mCtx, atomic ? "arc.final" : "rc.final", mCurrentFunc);
        auto* continueBlock = llvm::BasicBlock::Create(
            *mCtx, atomic ? "arc.released" : "rc.released", mCurrentFunc);
        mBuilder->CreateCondBr(isLast, finalBlock, continueBlock);
        mBuilder->SetInsertPoint(finalBlock);

        TypePtr payload = type &&
            (type->kind == TypeKind::Rc || type->kind == TypeKind::Arc)
            ? type->inner : nullptr;
        if (payload && payload->sysmeta.resource.needsDrop) {
            const std::string& symbol = payload->sysmeta.abi.dropGlueSymbol;
            auto drop = mFunctions.find(symbol);
            if (symbol.empty() || drop == mFunctions.end()) {
                error("final shared cleanup for '" + place +
                      "' has no compiler-validated payload drop glue");
            } else {
                mBuilder->CreateCall(drop->second, {
                    coerceCallArgument(pointer,
                        drop->second->getFunctionType()->getParamType(0))
                });
            }
        }
        auto deallocate = mModule->getOrInsertFunction(
            "rt_shared_dealloc", mHelpers->voidTy(), mHelpers->ptrTy());
        mBuilder->CreateCall(deallocate, {
            mBuilder->CreateBitCast(pointer, mHelpers->ptrTy())
        });
        mBuilder->CreateBr(continueBlock);
        mBuilder->SetInsertPoint(continueBlock);
        return;
    }
    if (action == luna::ownership::CleanupAction::Drop) {
        const std::string symbol = type
            ? type->sysmeta.abi.dropGlueSymbol : std::string{};
        auto drop = mFunctions.find(symbol);
        if (symbol.empty() || drop == mFunctions.end()) {
            error("drop cleanup for '" + place +
                  "' has no compiler-validated drop glue");
            return;
        }
        mBuilder->CreateCall(drop->second, {
            coerceCallArgument(pointer,
                drop->second->getFunctionType()->getParamType(0))
        });
    }
    emitLunaDeallocation(pointer, type);
}
