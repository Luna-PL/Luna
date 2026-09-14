#include "CodeGenerator.h"
#include "../core/TypeLayout.h"
#include "../core/TypeRelations.h"
#include "../runtime/RuntimeABI.h"

#include <algorithm>

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
    // String/cstr literals are immutable global constants and own no heap
    // allocation. Until the standard library introduces owned text, their
    // cleanup is a no-op (see emitOwnedPayloadCleanup).
    if (type && (type->kind == TypeKind::String ||
                 type->kind == TypeKind::CStr))
        return;
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
    // String and cstr values currently point at immutable globals and own no
    // allocation. Keep canonical CFG cleanup aligned with emitCleanup() and
    // emitOwnedPayloadCleanup(); an Allocation record must still release the
    // separately allocated storage for `new string`/`new cstr`.
    if (cleanup.kind == moon::CleanupKind::Value &&
        (type->kind == TypeKind::String || type->kind == TypeKind::CStr))
        return;
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
