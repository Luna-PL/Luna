#include "CodeGenerator.h"
#include "../core/TypeLayout.h"
#include "../core/TypeRelations.h"
#include "../runtime/RuntimeABI.h"

#include <algorithm>

llvm::Function* CodeGenerator::getOrCreateDropCallback(
    const TypePtr& type) {
    const std::string typeId = luna::types::typeId(type).value;
    auto existing = mDropCallbacks.find(typeId);
    if (existing != mDropCallbacks.end()) return existing->second;

    const std::string symbol = "__luna_drop_callback_" +
        std::to_string(luna::identity::stableIdentityHash(typeId));
    auto* callbackType = llvm::FunctionType::get(
        mHelpers->voidTy(), {mHelpers->ptrTy()}, false);
    auto* callback = llvm::Function::Create(
        callbackType, llvm::Function::InternalLinkage,
        symbol, mModule.get());
    mDropCallbacks[typeId] = callback;

    const auto savedInsertionPoint = mBuilder->saveIP();
    auto* savedFunction = mCurrentFunc;
    const bool savedKernel = mCurrentFunctionIsKernel;
    auto* entry = llvm::BasicBlock::Create(*mCtx, "entry", callback);
    mBuilder->SetInsertPoint(entry);
    mCurrentFunc = callback;
    mCurrentFunctionIsKernel = false;

    if (type && type->kind != TypeKind::Unit &&
        type->kind != TypeKind::Never) {
        llvm::Value* stored = mBuilder->CreateLoad(
            mHelpers->toLLVMType(type), callback->getArg(0),
            "stored.value");
        emitOwnedPayloadCleanup(stored, type, "erased.value");
    }
    if (!mBuilder->GetInsertBlock()->getTerminator())
        mBuilder->CreateRetVoid();

    mCurrentFunc = savedFunction;
    mCurrentFunctionIsKernel = savedKernel;
    mBuilder->restoreIP(savedInsertionPoint);
    return callback;
}

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

void CodeGenerator::emitResourceContentsCleanup(
    llvm::Value* value, const TypePtr& type, const std::string& label) {
    if (!value || !type) return;

    // Source Drop finalizes the value in place. Compiler-derived recursive
    // cleanup then destroys every still-owned field exactly once.
    if (type->sysmeta.resource.needsDrop) {
        const auto* frozenType = mProgram
            ? mProgram->findType(luna::types::typeId(type)) : nullptr;
        llvm::Function* drop = frozenType
            ? resolveFunction(frozenType->dropGlue) : nullptr;
        if (!drop) {
            error("resource '" + label +
                  "' has no compiler-validated Drop implementation");
            return;
        }
        llvm::Value* address = value;
        llvm::AllocaInst* storage = nullptr;
        if (!value->getType()->isPointerTy()) {
            storage = createEntryBlockAlloca(
                mCurrentFunc, value->getType(), label + ".drop.storage");
            mBuilder->CreateStore(value, storage);
            address = storage;
        }
        mBuilder->CreateCall(drop, {
            coerceCallArgument(
                address,
                drop->getFunctionType()->getParamType(0))
        });
        if (storage)
            value = mBuilder->CreateLoad(
                storage->getAllocatedType(), storage,
                label + ".drop.value");
    }

    if (type->kind == TypeKind::Struct) {
        if (!value->getType()->isPointerTy()) {
            error("named product '" + label +
                  "' has no pointer storage for recursive cleanup");
            return;
        }
        for (size_t index = 0; index < type->fields.size(); ++index) {
            const auto& field = type->fields[index];
            const uint64_t offset =
                luna::layout::productFieldOffset(type, index);
            llvm::Value* fieldPointer = value;
            if (offset != 0)
                fieldPointer = mBuilder->CreateGEP(
                    llvm::Type::getInt8Ty(*mCtx), value,
                    llvm::ConstantInt::get(
                        mHelpers->sizeTy(), offset),
                    label + ".field.pointer");
            llvm::Value* fieldValue = mBuilder->CreateLoad(
                mHelpers->toLLVMType(field.type), fieldPointer,
                label + "." + field.name);
            emitOwnedPayloadCleanup(
                fieldValue, field.type,
                label + "." + field.name);
        }
        return;
    }
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
}

void CodeGenerator::emitOwnedPayloadCleanup(
    llvm::Value* value, const TypePtr& type, const std::string& label) {
    if (!value || !type || !typeRequiresCleanup(type)) return;

    if (type->kind == TypeKind::DeviceBuffer) {
        auto release = mModule->getOrInsertFunction(
            "rt_gpu_free", mHelpers->voidTy(), mHelpers->ptrTy());
        mBuilder->CreateCall(
            release, {coerceCallArgument(value, mHelpers->ptrTy())});
        return;
    }
    if (type->kind == TypeKind::Array ||
        type->kind == TypeKind::Record ||
        type->kind == TypeKind::Result ||
        type->kind == TypeKind::Enum) {
        emitResourceContentsCleanup(value, type, label);
        return;
    }
    if (!value->getType()->isPointerTy()) {
        error("owned resource '" + type->toString() +
              "' is not representable by the cleanup ABI");
        return;
    }
    emitResourceContentsCleanup(value, type, label);
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
        emitOwnedPayloadCleanup(pointer, type, place + ".result");
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
    if (action == luna::ownership::CleanupAction::DeviceRelease) {
        emitOwnedPayloadCleanup(pointer, type, place + ".device");
        return;
    }
    if (action == luna::ownership::CleanupAction::Drop) {
        emitOwnedPayloadCleanup(pointer, type, place + ".resource");
        return;
    }
    if (action == luna::ownership::CleanupAction::None) {
        error("cleanup for '" + place + "' has no Resource action");
        return;
    }
    if (!pointer->getType()->isPointerTy()) return;
    emitLunaDeallocation(pointer, type);
}

void CodeGenerator::emitCanonicalCleanup(
    const moon::CleanupRecord& cleanup) {
    if (cleanup.place.root.empty() ||
        cleanup.place.root.value >= mCanonicalLocals.size() ||
        !mCanonicalLocals[cleanup.place.root.value]) {
        error("canonical cleanup references no LLVM local storage");
        return;
    }

    TypePtr type = resolveType(cleanup.type);
    if (!type) {
        error("canonical cleanup has no materialized type");
        return;
    }
    const auto root = cleanup.place.root.value;
    const std::string label =
        "local." + std::to_string(root) + ".cleanup";

    // A CleanupRecord names storage, not merely a value.  Walk the frozen
    // PlaceRef so projected fields/elements and dereferenced owning pointers
    // are cleaned in place instead of accidentally dropping the aggregate
    // root.  This is also the address form needed by guarded array cleanup.
    llvm::Value* storage = mCanonicalLocals[root];
    TypePtr currentType = root < mCanonicalLocalTypes.size()
        ? mCanonicalLocalTypes[root] : nullptr;
    if (!currentType) currentType = type;
    for (const auto& projection : cleanup.place.projections) {
        if (!currentType) {
            error("canonical cleanup projection has no source type");
            return;
        }
        switch (projection.kind) {
            case moon::ProjectionKind::Field: {
                if (projection.index >= currentType->fields.size()) {
                    error("canonical cleanup field projection is outside its type");
                    return;
                }
                if (currentType->kind == TypeKind::Struct) {
                    storage = mBuilder->CreateLoad(
                        mHelpers->ptrTy(), storage, label + ".object");
                } else if (currentType->kind != TypeKind::Record) {
                    error("canonical cleanup field projection has no product source");
                    return;
                }
                uint64_t offset = 0;
                for (size_t index = 0; index < projection.index; ++index) {
                    offset = luna::layout::alignTo(
                        offset, luna::layout::valueAlignment(
                            currentType->fields[index].type));
                    offset += luna::layout::valueSize(
                        currentType->fields[index].type);
                }
                offset = luna::layout::alignTo(
                    offset, luna::layout::valueAlignment(
                        currentType->fields[projection.index].type));
                if (offset != 0)
                    storage = mBuilder->CreateGEP(
                        llvm::Type::getInt8Ty(*mCtx), storage,
                        llvm::ConstantInt::get(mHelpers->sizeTy(), offset),
                        label + ".field");
                currentType = currentType->fields[projection.index].type;
                break;
            }
            case moon::ProjectionKind::ConstantIndex:
            case moon::ProjectionKind::DynamicIndex: {
                if (currentType->kind != TypeKind::Array &&
                    currentType->kind != TypeKind::Slice) {
                    error("canonical cleanup index projection has no array source");
                    return;
                }
                if (projection.kind == moon::ProjectionKind::ConstantIndex &&
                    currentType->kind == TypeKind::Array &&
                    projection.index >= currentType->arrayLength) {
                    error("canonical cleanup constant index is outside its array");
                    return;
                }
                if (currentType->kind == TypeKind::Slice) {
                    auto slice = mBuilder->CreateLoad(
                        mHelpers->toLLVMType(currentType), storage,
                        label + ".slice");
                    auto data = mBuilder->CreateExtractValue(
                        slice, {0}, label + ".data");
                    llvm::Value* index = nullptr;
                    if (projection.kind == moon::ProjectionKind::ConstantIndex) {
                        index = llvm::ConstantInt::get(
                            mHelpers->i32Ty(), projection.index);
                    } else if (!projection.dynamicIndex.empty() &&
                               projection.dynamicIndex.value < mCanonicalLocals.size() &&
                               mCanonicalLocals[projection.dynamicIndex.value]) {
                        index = mBuilder->CreateLoad(
                            mCanonicalLocals[projection.dynamicIndex.value]->getAllocatedType(),
                            mCanonicalLocals[projection.dynamicIndex.value],
                            label + ".index");
                    } else {
                        error("canonical cleanup dynamic index has no LLVM local");
                        return;
                    }
                    index = coerceCallArgument(index, mHelpers->i32Ty());
                    storage = mBuilder->CreateInBoundsGEP(
                        mHelpers->toLLVMType(currentType->inner), data, index,
                        label + ".element");
                } else if (projection.kind == moon::ProjectionKind::ConstantIndex) {
                    const auto stride = luna::layout::valueSize(
                        currentType->inner);
                    const auto offset = stride * projection.index;
                    if (offset != 0)
                        storage = mBuilder->CreateGEP(
                            llvm::Type::getInt8Ty(*mCtx), storage,
                            llvm::ConstantInt::get(mHelpers->sizeTy(), offset),
                            label + ".element");
                } else if (!projection.dynamicIndex.empty() &&
                           projection.dynamicIndex.value < mCanonicalLocals.size() &&
                           mCanonicalLocals[projection.dynamicIndex.value]) {
                    llvm::Value* index = mBuilder->CreateLoad(
                        mCanonicalLocals[projection.dynamicIndex.value]->getAllocatedType(),
                        mCanonicalLocals[projection.dynamicIndex.value],
                        label + ".index");
                    index = coerceCallArgument(index, mHelpers->sizeTy());
                    auto stride = llvm::ConstantInt::get(
                        mHelpers->sizeTy(), luna::layout::valueSize(
                            currentType->inner));
                    auto offset = mBuilder->CreateMul(index, stride, label + ".offset");
                    storage = mBuilder->CreateGEP(
                        llvm::Type::getInt8Ty(*mCtx), storage, offset,
                        label + ".element");
                } else {
                    error("canonical cleanup dynamic index has no LLVM local");
                    return;
                }
                currentType = currentType->inner;
                break;
            }
            case moon::ProjectionKind::Dereference:
                if (currentType->kind != TypeKind::Reference &&
                    currentType->kind != TypeKind::RawPointer) {
                    error("canonical cleanup dereference projection has no pointer source");
                    return;
                }
                storage = mBuilder->CreateLoad(
                    mHelpers->ptrTy(), storage, label + ".deref");
                currentType = currentType->inner;
                break;
        }
    }

    auto emitValueCleanup = [this, &cleanup, type, &storage, label]() {
        llvm::Type* storageType = cleanup.kind == moon::CleanupKind::Allocation
            ? mHelpers->ptrTy() : mHelpers->toLLVMType(type);
        llvm::Value* value = mBuilder->CreateLoad(
            storageType, storage, label);
        switch (cleanup.action) {
            case luna::ownership::CleanupAction::ResultDrop:
                if (type->kind != TypeKind::Result) {
                    error("canonical Result cleanup has a non-Result type");
                    return;
                }
                emitOwnedPayloadCleanup(value, type, label + ".result");
                return;
            case luna::ownership::CleanupAction::EnumDrop:
                if (type->kind != TypeKind::Enum) {
                    error("canonical enum cleanup has a non-enum type");
                    return;
                }
                emitOwnedPayloadCleanup(value, type, label + ".enum");
                return;
            case luna::ownership::CleanupAction::ArrayDrop:
                if (type->kind != TypeKind::Array) {
                    error("canonical array cleanup has a non-array type");
                    return;
                }
                emitOwnedPayloadCleanup(value, type, label + ".array");
                return;
            case luna::ownership::CleanupAction::RecordDrop:
                if (type->kind != TypeKind::Record) {
                    error("canonical record cleanup has a non-record type");
                    return;
                }
                emitOwnedPayloadCleanup(value, type, label + ".record");
                return;
            case luna::ownership::CleanupAction::DeviceRelease:
                emitOwnedPayloadCleanup(value, type, label + ".device");
                return;
            case luna::ownership::CleanupAction::Drop:
                emitOwnedPayloadCleanup(value, type, label + ".resource");
                return;
            case luna::ownership::CleanupAction::Deallocate:
                if (!value->getType()->isPointerTy()) {
                    error("canonical deallocation cleanup has no pointer value");
                    return;
                }
                emitLunaDeallocation(value, type);
                return;
            case luna::ownership::CleanupAction::None:
                error("canonical cleanup has no Resource action");
                return;
        }
    };

    if (cleanup.kind == moon::CleanupKind::Allocation) {
        if (!cleanup.place.projections.empty() || cleanup.guard) {
            error("canonical allocation cleanup is projected or guarded");
            return;
        }
        emitValueCleanup();
        return;
    }

    if (cleanup.guard) {
        const auto cursor = cleanup.guard->nextUnread;
        if (cursor.empty() || cursor.value >= mCanonicalLocals.size() ||
            !mCanonicalLocals[cursor.value]) {
            error("guarded canonical cleanup has no LLVM cursor storage");
            return;
        }
        auto cursorValue = mBuilder->CreateLoad(
            mCanonicalLocals[cursor.value]->getAllocatedType(),
            mCanonicalLocals[cursor.value], label + ".cursor");
        auto threshold = llvm::ConstantInt::get(
            cursorValue->getType(), cleanup.guard->elementIndex);
        auto shouldCleanup = mBuilder->CreateICmpULE(
            cursorValue, threshold, label + ".guard");
        auto* cleanupBlock = llvm::BasicBlock::Create(
            *mCtx, label + ".guarded", mCurrentFunc);
        auto* afterBlock = llvm::BasicBlock::Create(
            *mCtx, label + ".after", mCurrentFunc);
        mBuilder->CreateCondBr(shouldCleanup, cleanupBlock, afterBlock);
        mBuilder->SetInsertPoint(cleanupBlock);
        emitValueCleanup();
        if (!mBuilder->GetInsertBlock()->getTerminator())
            mBuilder->CreateBr(afterBlock);
        mBuilder->SetInsertPoint(afterBlock);
        return;
    }

    emitValueCleanup();
}
