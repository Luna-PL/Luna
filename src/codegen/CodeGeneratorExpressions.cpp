#include "CodeGenerator.h"
#include "CodeGeneratorRangeAnalysis.h"
#include "../core/TypeLayout.h"

#include <algorithm>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using moon::AddrOfExpr;
using moon::ArrayLiteralExpr;
using moon::AssignExpr;
using moon::BinaryExpr;
using moon::BoolLiteralExpr;
using moon::BorrowExpr;
using moon::CallExpr;
using moon::DerefExpr;
using moon::DynamicSelectExpr;
using moon::EnvLoadExpr;
using moon::Expr;
using moon::FieldAccessExpr;
using moon::FloatLiteralExpr;
using moon::HeapAllocExpr;
using moon::IdentifierExpr;
using moon::IndexExpr;
using moon::InitAllocationExpr;
using moon::IntLiteralExpr;
using moon::LambdaExpr;
using moon::LaunchExpr;
using moon::MoveExpr;
using moon::Operator;
using moon::RecordLiteralExpr;
using moon::ResultConstructExpr;
using moon::SliceLengthExpr;
using moon::StringLiteralExpr;
using moon::TryExpr;
using moon::UnaryExpr;
using moon::UnitExpr;
using moon::VariantConstructExpr;

// ─── Expression generation ─────────────────────────────────────────

llvm::Value* CodeGenerator::generateIntLiteral(IntLiteralExpr* il) {
    return llvm::ConstantInt::get(mHelpers->i32Ty(), il->value, true);
}

llvm::Value* CodeGenerator::generateFloatLiteral(FloatLiteralExpr* fl) {
    return llvm::ConstantFP::get(mHelpers->f64Ty(), fl->value);
}

llvm::Value* CodeGenerator::generateStringLiteral(StringLiteralExpr* sl) {
    auto* gvar = mBuilder->CreateGlobalString(sl->value, "str");
    return mBuilder->CreateGEP(
        gvar->getValueType(), gvar,
        {llvm::ConstantInt::get(mHelpers->i32Ty(), 0),
         llvm::ConstantInt::get(mHelpers->i32Ty(), 0)}, "strptr");
}

llvm::Value* CodeGenerator::generateBoolLiteral(BoolLiteralExpr* bl) {
    return llvm::ConstantInt::get(mHelpers->boolTy(), bl->value ? 1 : 0);
}

llvm::Value* CodeGenerator::generateUnitLiteral(UnitExpr*) {
    // Unit has no runtime payload. Keep the legacy structured backend's
    // expression API total until canonical CFG becomes its only input.
    return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
}

llvm::Value* CodeGenerator::generateArrayLiteral(ArrayLiteralExpr* array) {
    TypePtr arrayType = Type::makeArray(
        resolveType(array->elementType), array->elements.size());
    auto* llvmArray = llvm::cast<llvm::ArrayType>(mHelpers->toLLVMType(arrayType));
    llvm::Value* result = llvm::UndefValue::get(llvmArray);
    for (size_t i = 0; i < array->elements.size(); ++i)
        result = mBuilder->CreateInsertValue(result, generateExpr(array->elements[i].get()),
                                              {static_cast<unsigned>(i)}, "array.init");
    return result;
}

llvm::Value* CodeGenerator::generateIdentifier(IdentifierExpr* id) {
    if (!id->local.empty() &&
        id->local.value < mCanonicalLocals.size() &&
        mCanonicalLocals[id->local.value]) {
        auto* alloca = mCanonicalLocals[id->local.value];
        return mBuilder->CreateLoad(
            alloca->getAllocatedType(), alloca,
            id->name.empty()
                ? "local." + std::to_string(id->local.value)
                : id->name);
    }
    auto it = mLocals.find(id->name);
    if (it != mLocals.end()) {
        auto* alloca = it->second;
        return mBuilder->CreateLoad(alloca->getAllocatedType(), alloca, id->name);
    }
    if (id->declaration.complete()) {
        if (auto* function = resolveFunction(id->declaration))
            return function;
    }
    return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
}

llvm::Value* CodeGenerator::generateDynamicSelect(DynamicSelectExpr* selection) {
    auto* opaquePointerType = llvm::cast<llvm::PointerType>(mHelpers->ptrTy());
    std::vector<llvm::Value*> filterValues;
    for (auto& argument : selection->filterArguments)
        filterValues.push_back(generateExpr(argument.get()));

    llvm::Value* selected = llvm::ConstantPointerNull::get(opaquePointerType);
    llvm::Value* matchCount = llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
    for (const auto& candidate : selection->candidates) {
        llvm::Value* matches = llvm::ConstantInt::getTrue(*mCtx);
        for (size_t index = 0; index < filterValues.size(); ++index) {
            llvm::Value* actual = filterValues[index];
            const auto& expectedValue = candidate.metadataValues[index];
            llvm::Value* equal = nullptr;
            if (auto* integer = std::get_if<int64_t>(&expectedValue)) {
                if (!actual->getType()->isIntegerTy()) {
                    error("dynamic selector integer metadata type mismatch");
                    return llvm::ConstantPointerNull::get(opaquePointerType);
                }
                auto* expected = llvm::ConstantInt::get(
                    actual->getType(), static_cast<uint64_t>(*integer), true);
                equal = mBuilder->CreateICmpEQ(actual, expected, "dynamic.meta.eq");
            } else if (auto* floating = std::get_if<double>(&expectedValue)) {
                if (!actual->getType()->isFloatingPointTy()) {
                    error("dynamic selector floating metadata type mismatch");
                    return llvm::ConstantPointerNull::get(opaquePointerType);
                }
                auto* expected = llvm::ConstantFP::get(actual->getType(), *floating);
                equal = mBuilder->CreateFCmpOEQ(actual, expected, "dynamic.meta.eq");
            } else if (auto* boolean = std::get_if<bool>(&expectedValue)) {
                if (!actual->getType()->isIntegerTy()) {
                    error("dynamic selector boolean metadata type mismatch");
                    return llvm::ConstantPointerNull::get(opaquePointerType);
                }
                auto* expected = llvm::ConstantInt::get(
                    actual->getType(), *boolean ? 1 : 0);
                equal = mBuilder->CreateICmpEQ(actual, expected, "dynamic.meta.eq");
            } else {
                const auto& string = std::get<std::string>(expectedValue);
                if (!actual->getType()->isPointerTy()) {
                    error("dynamic selector string metadata type mismatch");
                    return llvm::ConstantPointerNull::get(opaquePointerType);
                }
                auto* storage = mBuilder->CreateGlobalString(
                    string, "dynamic.meta.string");
                auto* expected = mBuilder->CreateInBoundsGEP(
                    storage->getValueType(), storage,
                    {llvm::ConstantInt::get(mHelpers->i32Ty(), 0),
                     llvm::ConstantInt::get(mHelpers->i32Ty(), 0)});
                auto compare = mModule->getOrInsertFunction(
                    "strcmp", mHelpers->i32Ty(), mHelpers->ptrTy(),
                    mHelpers->ptrTy());
                auto* compared = mBuilder->CreateCall(
                    compare, {actual, expected}, "dynamic.meta.strcmp");
                equal = mBuilder->CreateICmpEQ(
                    compared, llvm::ConstantInt::get(mHelpers->i32Ty(), 0),
                    "dynamic.meta.eq");
            }
            matches = mBuilder->CreateAnd(matches, equal, "dynamic.meta.all");
        }
        llvm::Function* function = resolveFunction(candidate.declaration);
        if (!function) {
            error("dynamic select candidate '" +
                  candidate.declaration.symbol.value +
                  "' has no generated function");
            return llvm::ConstantPointerNull::get(opaquePointerType);
        }
        selected = mBuilder->CreateSelect(matches, function, selected,
                                          "dynamic.selected");
        matchCount = mBuilder->CreateAdd(
            matchCount, mBuilder->CreateZExt(matches, mHelpers->i32Ty()),
            "dynamic.match.count");
    }

    auto* valid = mBuilder->CreateICmpEQ(
        matchCount, llvm::ConstantInt::get(mHelpers->i32Ty(), 1),
        "dynamic.select.unique");
    auto* success = llvm::BasicBlock::Create(
        *mCtx, "dynamic.select.success", mCurrentFunc);
    auto* failure = llvm::BasicBlock::Create(
        *mCtx, "dynamic.select.failure", mCurrentFunc);
    mBuilder->CreateCondBr(valid, success, failure);
    mBuilder->SetInsertPoint(failure);
    auto abort = mModule->getOrInsertFunction("abort", mHelpers->voidTy());
    mBuilder->CreateCall(abort);
    mBuilder->CreateUnreachable();
    mBuilder->SetInsertPoint(success);
    return selected;
}

llvm::Value* CodeGenerator::generateFieldAccess(FieldAccessExpr* field) {
    TypePtr objectType;
    if (auto* id = dynamic_cast<IdentifierExpr*>(field->object.get())) {
        auto it = mLocalTypes.find(id->name);
        if (it != mLocalTypes.end()) objectType = it->second;
    }
    if (!objectType && field->object)
        objectType = resolveType(field->object->type);
    const bool isReference = objectType &&
        objectType->kind == TypeKind::Reference;
    if (objectType && objectType->kind == TypeKind::Reference)
        objectType = objectType->inner;
    size_t index = fieldIndex(objectType, field->field);
    if (!objectType || index == static_cast<size_t>(-1))
        return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);

    llvm::Value* object = generateExpr(field->object.get());
    if (objectType->kind == TypeKind::Record && !isReference)
        return mBuilder->CreateExtractValue(
            object, {static_cast<unsigned>(index)}, field->field);
    const uint64_t offset =
        luna::layout::productFieldOffset(objectType, index);
    auto* bytePtr = mBuilder->CreateGEP(
        llvm::Type::getInt8Ty(*mCtx), object,
        llvm::ConstantInt::get(mHelpers->sizeTy(), offset), "fieldptr");
    auto fieldType = mHelpers->toLLVMType(objectType->fields[index].type);
    auto* typedPtr = mBuilder->CreateBitCast(
        bytePtr, llvm::PointerType::get(*mCtx, 0), "typedfieldptr");
    return mBuilder->CreateLoad(fieldType, typedPtr, field->field);
}

llvm::Value* CodeGenerator::generateSliceLength(SliceLengthExpr* length) {
    auto* slice = generateExpr(length->slice.get());
    auto* rawLength = mBuilder->CreateExtractValue(
        slice, {1}, "slice.length");
    return rawLength;
}

llvm::Value* CodeGenerator::generateIndex(IndexExpr* index) {
    auto* id = dynamic_cast<IdentifierExpr*>(index->object.get());
    llvm::AllocaInst* storage = nullptr;
    TypePtr arrayType;
    if (id && !id->local.empty() &&
        id->local.value < mCanonicalLocals.size() &&
        mCanonicalLocals[id->local.value]) {
        storage = mCanonicalLocals[id->local.value];
        arrayType = id->local.value < mCanonicalLocalTypes.size()
            ? mCanonicalLocalTypes[id->local.value] : resolveType(id->type);
    } else if (id && mLocals.count(id->name)) {
        storage = mLocals[id->name];
        arrayType = mLocalTypes[id->name];
    }
    if (!storage || !arrayType ||
        (arrayType->kind != TypeKind::Array &&
         arrayType->kind != TypeKind::Slice)) {
        error("safe array indexing requires a canonical local array binding");
        return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
    }
    if (arrayType->kind == TypeKind::Slice) {
        auto slice = mBuilder->CreateLoad(
            storage->getAllocatedType(), storage, "slice.value");
        auto* length = mBuilder->CreateExtractValue(
            slice, {1}, "slice.length");
        auto* checked = mBuilder->CreateCall(
            mModule->getOrInsertFunction(
                "rt_array_index_or_abort", mHelpers->i32Ty(),
                mHelpers->i32Ty(), mHelpers->sizeTy()),
            {coerceCallArgument(
                 generateExpr(index->index.get()), mHelpers->i32Ty()),
             length},
            "slice.index");
        auto* data = mBuilder->CreateExtractValue(
            slice, {0}, "slice.data");
        auto* ptr = mBuilder->CreateGEP(
            mHelpers->toLLVMType(arrayType->inner), data, checked,
            "slice.element");
        return mBuilder->CreateLoad(
            mHelpers->toLLVMType(arrayType->inner), ptr, "slice.load");
    }
    auto* rawIndex = coerceCallArgument(
        generateExpr(index->index.get()), mHelpers->i32Ty());
    auto* checked = mBuilder->CreateCall(
        mModule->getOrInsertFunction(
            "rt_array_index_or_abort", mHelpers->i32Ty(),
            mHelpers->i32Ty(), mHelpers->sizeTy()),
        {rawIndex, llvm::ConstantInt::get(
                  mHelpers->sizeTy(), arrayType->arrayLength)},
        "array.index");
    auto* elementPtr = mBuilder->CreateInBoundsGEP(
        storage->getAllocatedType(), storage,
        {llvm::ConstantInt::get(mHelpers->i32Ty(), 0), checked},
        "array.element");
    return mBuilder->CreateLoad(
        mHelpers->toLLVMType(arrayType->inner), elementPtr, "array.load");
}

llvm::Value* CodeGenerator::generateBinary(BinaryExpr* bin) {
    llvm::Value* lhs = generateExpr(bin->lhs.get());
    if (bin->op == Operator::LogicalAnd || bin->op == Operator::LogicalOr) {
        // `&&` and `||` are control-flow operators, not integer bitwise
        // aliases.  Lower them through a small CFG so the right-hand
        // expression is evaluated only when it is semantically needed.
        auto* originBB = mBuilder->GetInsertBlock();
        auto* rhsBB = llvm::BasicBlock::Create(*mCtx, "logic.rhs", mCurrentFunc);
        auto* mergeBB = llvm::BasicBlock::Create(*mCtx, "logic.merge", mCurrentFunc);
        if (bin->op == Operator::LogicalAnd)
            mBuilder->CreateCondBr(lhs, rhsBB, mergeBB);
        else
            mBuilder->CreateCondBr(lhs, mergeBB, rhsBB);

        mBuilder->SetInsertPoint(rhsBB);
        llvm::Value* rhs = generateExpr(bin->rhs.get());
        auto* rhsEndBB = mBuilder->GetInsertBlock();
        if (!rhsEndBB->getTerminator()) mBuilder->CreateBr(mergeBB);

        mBuilder->SetInsertPoint(mergeBB);
        auto* result = mBuilder->CreatePHI(mHelpers->boolTy(), 2,
                                           bin->op == Operator::LogicalAnd
                                               ? "and.shortcircuit"
                                               : "or.shortcircuit");
        result->addIncoming(llvm::ConstantInt::get(
            mHelpers->boolTy(), bin->op == Operator::LogicalAnd ? 0 : 1), originBB);
        result->addIncoming(rhs, rhsEndBB);
        return result;
    }
    llvm::Value* rhs = generateExpr(bin->rhs.get());
    switch (bin->op) {
        case Operator::Add:
            return lhs->getType()->isFloatingPointTy()
                ? mBuilder->CreateFAdd(lhs, rhs, "addtmp")
                : mBuilder->CreateAdd(lhs, rhs, "addtmp");
        case Operator::Subtract:
            return lhs->getType()->isFloatingPointTy()
                ? mBuilder->CreateFSub(lhs, rhs, "subtmp")
                : mBuilder->CreateSub(lhs, rhs, "subtmp");
        case Operator::Multiply:
            return lhs->getType()->isFloatingPointTy()
                ? mBuilder->CreateFMul(lhs, rhs, "multmp")
                : mBuilder->CreateMul(lhs, rhs, "multmp");
        case Operator::Divide:
            return lhs->getType()->isFloatingPointTy()
                ? mBuilder->CreateFDiv(lhs, rhs, "divtmp")
                : mBuilder->CreateSDiv(lhs, rhs, "divtmp");
        case Operator::Remainder:
            return lhs->getType()->isFloatingPointTy()
                ? mBuilder->CreateFRem(lhs, rhs, "remtmp")
                : mBuilder->CreateSRem(lhs, rhs, "remtmp");
        case Operator::BitAnd:
            return mBuilder->CreateAnd(lhs, rhs, "bitandtmp");
        case Operator::BitOr:
            return mBuilder->CreateOr(lhs, rhs, "bitortmp");
        case Operator::BitXor:
            return mBuilder->CreateXor(lhs, rhs, "bitxortmp");
        case Operator::ShiftLeft:
            return mBuilder->CreateShl(lhs, rhs, "shltmp");
        case Operator::ShiftRight:
            return mBuilder->CreateAShr(lhs, rhs, "shrtmp");
        case Operator::Equal:
            return lhs->getType()->isFloatingPointTy()
                ? mBuilder->CreateFCmpOEQ(lhs, rhs, "eqtmp")
                : mBuilder->CreateICmpEQ(lhs, rhs, "eqtmp");
        case Operator::NotEqual:
            return lhs->getType()->isFloatingPointTy()
                ? mBuilder->CreateFCmpONE(lhs, rhs, "neqtmp")
                : mBuilder->CreateICmpNE(lhs, rhs, "neqtmp");
        case Operator::Less:
            return lhs->getType()->isFloatingPointTy()
                ? mBuilder->CreateFCmpOLT(lhs, rhs, "lttmp")
                : mBuilder->CreateICmpSLT(lhs, rhs, "lttmp");
        case Operator::LessEqual:
            return lhs->getType()->isFloatingPointTy()
                ? mBuilder->CreateFCmpOLE(lhs, rhs, "letmp")
                : mBuilder->CreateICmpSLE(lhs, rhs, "letmp");
        case Operator::Greater:
            return lhs->getType()->isFloatingPointTy()
                ? mBuilder->CreateFCmpOGT(lhs, rhs, "gttmp")
                : mBuilder->CreateICmpSGT(lhs, rhs, "gttmp");
        case Operator::GreaterEqual:
            return lhs->getType()->isFloatingPointTy()
                ? mBuilder->CreateFCmpOGE(lhs, rhs, "getmp")
                : mBuilder->CreateICmpSGE(lhs, rhs, "getmp");
        default: break;
    }
    return nullptr;
}

llvm::Value* CodeGenerator::generateUnary(UnaryExpr* un) {
    llvm::Value* op = generateExpr(un->operand.get());
    switch (un->op) {
        case Operator::Negate:
            return op->getType()->isFloatingPointTy()
                ? mBuilder->CreateFNeg(op, "negtmp")
                : mBuilder->CreateNeg(op, "negtmp");
        case Operator::LogicalNot:
            return mBuilder->CreateNot(op, "nottmp");
        case Operator::BitNot:
            return mBuilder->CreateNot(op, "bitnottmp");
        case Operator::Dereference: {
            auto* ptrTy = llvm::PointerType::get(*mCtx, 0);
            auto* ptr = mBuilder->CreateBitCast(op, ptrTy);
            return mBuilder->CreateLoad(mHelpers->i32Ty(), ptr, "dereftmp");
        }
        default: break;
    }
    return nullptr;
}

llvm::Value* CodeGenerator::generateVariantConstruct(VariantConstructExpr* variant) {
    const TypePtr constructedType = resolveType(variant->constructedType);
    if (!constructedType || constructedType->kind != TypeKind::Enum) {
        error("enum construction has no validated inline ADT type");
        return llvm::PoisonValue::get(mHelpers->i32Ty());
    }
    size_t variantIndex = 0;
    const TypeVariant* selected = nullptr;
    for (; variantIndex < constructedType->variants.size();
         ++variantIndex) {
        if (constructedType->variants[variantIndex].name ==
            variant->variantName) {
            selected =
                &constructedType->variants[variantIndex];
            break;
        }
    }
    auto* enumLLVM = llvm::dyn_cast<llvm::StructType>(
        mHelpers->toLLVMType(constructedType));
    if (!selected || !enumLLVM || enumLLVM->getNumElements() != 2) {
        error("enum variant has no validated inline ADT layout");
        return llvm::PoisonValue::get(mHelpers->i32Ty());
    }
    auto* payloadLLVM = enumLLVM->getElementType(1);
    auto* payloadStorage = createEntryBlockAlloca(
        mCurrentFunc, payloadLLVM, "enum.payload.storage");
    mBuilder->CreateStore(
        llvm::Constant::getNullValue(payloadLLVM), payloadStorage);
    for (size_t fieldIndex = 0;
         fieldIndex < variant->args.size() &&
         fieldIndex < selected->fields.size(); ++fieldIndex) {
        llvm::Value* fieldValue =
            generateExpr(variant->args[fieldIndex].get());
        auto* sourceStorage = createEntryBlockAlloca(
            mCurrentFunc, fieldValue->getType(),
            "enum.payload.source");
        mBuilder->CreateStore(fieldValue, sourceStorage);
        const uint64_t offset =
            luna::layout::variantFieldOffset(*selected, fieldIndex);
        llvm::Value* destination = payloadStorage;
        if (offset != 0)
            destination = mBuilder->CreateGEP(
                llvm::Type::getInt8Ty(*mCtx), payloadStorage,
                llvm::ConstantInt::get(
                    mHelpers->sizeTy(), offset),
                "enum.payload.field");
        mBuilder->CreateMemCpy(
            destination, llvm::Align(std::max<uint64_t>(
                1, std::min<uint64_t>(
                    8, luna::layout::valueAlignment(
                           selected->fields[fieldIndex])))),
            sourceStorage, llvm::Align(std::max<uint64_t>(
                1, std::min<uint64_t>(
                    8, luna::layout::valueAlignment(
                           selected->fields[fieldIndex])))),
            luna::layout::valueSize(
                selected->fields[fieldIndex]));
    }
    llvm::Value* payload = mBuilder->CreateLoad(
        payloadLLVM, payloadStorage, "enum.payload");
    llvm::Value* result =
        llvm::UndefValue::get(enumLLVM);
    result = mBuilder->CreateInsertValue(
        result,
        llvm::ConstantInt::get(
            mHelpers->i32Ty(), variantIndex),
        {0}, "enum.tag");
    return mBuilder->CreateInsertValue(
        result, payload, {1}, "enum.value");
}

llvm::Value* CodeGenerator::generateResultConstruct(ResultConstructExpr* result) {
    const TypePtr resultType = resolveType(result->type);
    if (!resultType || resultType->kind != TypeKind::Result ||
        resultType->typeArgs.size() != 2 || !result->payload) {
        error("Result construction has no validated payload contract");
        return llvm::PoisonValue::get(mHelpers->i32Ty());
    }
    const TypePtr payloadType =
        resultType->typeArgs[result->isOk ? 0 : 1];
    llvm::Value* payload = generateExpr(result->payload.get());
    llvm::Value* bits = packResultPayload(
        payload, payloadType, resultType);
    llvm::Value* value = llvm::UndefValue::get(
        mHelpers->toLLVMType(resultType));
    value = mBuilder->CreateInsertValue(
        value,
        llvm::ConstantInt::get(
            mHelpers->boolTy(), result->isOk ? 1 : 0),
        {0}, result->isOk ? "ok.tag" : "err.tag");
    return mBuilder->CreateInsertValue(
        value, bits, {1}, result->isOk ? "ok.value" : "err.value");
}

llvm::Value* CodeGenerator::generateRecordLiteral(RecordLiteralExpr* record) {
    const TypePtr recordType = resolveType(record->type);
    if (!recordType ||
        (recordType->kind != TypeKind::Record &&
         recordType->kind != TypeKind::Struct)) {
        error("record literal has no validated product type");
        return llvm::PoisonValue::get(mHelpers->i32Ty());
    }
    if (recordType->kind == TypeKind::Struct) {
        auto rtAlloc = mModule->getOrInsertFunction(
            "rt_alloc", mHelpers->ptrTy(),
            mHelpers->sizeTy(), mHelpers->sizeTy());
        llvm::Value* pointer = mBuilder->CreateCall(
            rtAlloc,
            {llvm::ConstantInt::get(
                 mHelpers->sizeTy(), typeSize(recordType)),
             llvm::ConstantInt::get(
                 mHelpers->sizeTy(), typeAlignment(recordType))},
            "struct.literal");
        for (auto& field : record->fields) {
            // Evaluate in source order, but place by declaration field.
            llvm::Value* fieldValue = generateExpr(field.value.get());
            const size_t index = fieldIndex(recordType, field.name);
            if (index == static_cast<size_t>(-1)) {
                error("named struct literal field is absent from its type");
                continue;
            }
            const uint64_t offset =
                luna::layout::productFieldOffset(recordType, index);
            llvm::Value* fieldPointer = pointer;
            if (offset != 0)
                fieldPointer = mBuilder->CreateGEP(
                    llvm::Type::getInt8Ty(*mCtx), pointer,
                    llvm::ConstantInt::get(
                        mHelpers->sizeTy(), offset),
                    "struct.literal.field");
            fieldValue = coerceCallArgument(
                fieldValue,
                mHelpers->toLLVMType(recordType->fields[index].type));
            mBuilder->CreateStore(fieldValue, fieldPointer);
        }
        return pointer;
    }
    auto* recordLLVM = llvm::dyn_cast<llvm::StructType>(
        mHelpers->toLLVMType(recordType));
    if (!recordLLVM) {
        error("record literal did not lower to an inline aggregate");
        return llvm::PoisonValue::get(mHelpers->i32Ty());
    }
    llvm::Value* result = llvm::UndefValue::get(recordLLVM);
    for (auto& field : record->fields) {
        // Generate in source order, then place the value at its canonical
        // name-sorted index.
        llvm::Value* fieldValue = generateExpr(field.value.get());
        const size_t index = fieldIndex(recordType, field.name);
        if (index == static_cast<size_t>(-1)) {
            error("record literal field is absent from its canonical type");
            continue;
        }
        fieldValue = coerceCallArgument(
            fieldValue, recordLLVM->getElementType(index));
        result = mBuilder->CreateInsertValue(
            result, fieldValue,
            {static_cast<unsigned>(index)}, "record.field");
    }
    return result;
}

llvm::Value* CodeGenerator::generateInitAllocation(InitAllocationExpr* initialized) {
    if (initialized->allocation.empty() ||
        initialized->allocation.value >= mCanonicalLocals.size() ||
        !mCanonicalLocals[initialized->allocation.value]) {
        error("canonical allocation initialization has no storage");
        return llvm::PoisonValue::get(mHelpers->ptrTy());
    }
    auto pointer = mBuilder->CreateLoad(
        mCanonicalLocals[initialized->allocation.value]->getAllocatedType(),
        mCanonicalLocals[initialized->allocation.value],
        "canonical.allocation.value");
    auto allocatedType = resolveType(initialized->allocatedType);
    if (!allocatedType) {
        error("canonical allocation initialization has no materialized type");
        return llvm::PoisonValue::get(mHelpers->ptrTy());
    }
    for (const auto& element : initialized->elements) {
        auto value = generateExpr(element.value.get());
        llvm::Value* destination = pointer;
        TypePtr fieldType = allocatedType;
        if (allocatedType->kind == TypeKind::Struct ||
            allocatedType->kind == TypeKind::Record) {
            if (element.index >= allocatedType->fields.size()) {
                error("canonical allocation initializer field is outside its type");
                continue;
            }
            fieldType = allocatedType->fields[element.index].type;
            uint64_t offset = 0;
            for (size_t fieldIndex = 0; fieldIndex < element.index; ++fieldIndex) {
                offset = luna::layout::alignTo(
                    offset, luna::layout::valueAlignment(
                        allocatedType->fields[fieldIndex].type));
                offset += luna::layout::valueSize(
                    allocatedType->fields[fieldIndex].type);
            }
            offset = luna::layout::alignTo(
                offset, luna::layout::valueAlignment(
                    allocatedType->fields[element.index].type));
            if (offset != 0)
                destination = mBuilder->CreateGEP(
                    llvm::Type::getInt8Ty(*mCtx), pointer,
                    llvm::ConstantInt::get(mHelpers->sizeTy(), offset),
                    "canonical.allocation.field");
        } else if (element.index != 0) {
            error("canonical scalar allocation initializer has a nonzero index");
            continue;
        }
        mBuilder->CreateStore(
            coerceCallArgument(value, mHelpers->toLLVMType(fieldType)),
            destination);
    }
    return pointer;
}

llvm::Value* CodeGenerator::generateHeapAlloc(HeapAllocExpr* ha) {
    const TypePtr allocatedType = resolveType(ha->allocatedType);
    uint64_t sz = typeSize(allocatedType);
    auto* sizeVal = llvm::ConstantInt::get(mHelpers->sizeTy(), sz);
    auto* alignmentVal = llvm::ConstantInt::get(
        mHelpers->sizeTy(), typeAlignment(allocatedType));
    auto rtAlloc = mModule->getOrInsertFunction(
        "rt_alloc", mHelpers->ptrTy(),
        mHelpers->sizeTy(), mHelpers->sizeTy());
    llvm::Value* ptr = mBuilder->CreateCall(
        rtAlloc, {sizeVal, alignmentVal}, "heapalloc");

    // Initialize: store constructor args at the malloc'd pointer
    if (auto* initCall = dynamic_cast<CallExpr*>(ha->initializer.get())) {
        uint64_t offset = 0;
        for (size_t i = 0; i < initCall->args.size(); ++i) {
            auto& arg = initCall->args[i];
            llvm::Value* argVal = generateExpr(arg.get());
            if (allocatedType &&
                allocatedType->kind == TypeKind::Struct)
                offset = luna::layout::productFieldOffset(
                    allocatedType, i);
            else if (i > 0 && allocatedType &&
                     allocatedType->kind == TypeKind::Record) {
                offset = 0;
                for (size_t j = 0;
                     j < i && j < allocatedType->fields.size(); ++j)
                    offset += luna::layout::valueSize(
                        allocatedType->fields[j].type);
            }
            auto* basePtr = ptr;
            if (offset != 0)
                basePtr = mBuilder->CreateGEP(
                    llvm::Type::getInt8Ty(*mCtx), ptr,
                    llvm::ConstantInt::get(mHelpers->sizeTy(), offset), "fieldinit");
            auto* typedPtr = mBuilder->CreateBitCast(
                basePtr, llvm::PointerType::get(*mCtx, 0), "typedptr");
            mBuilder->CreateStore(argVal, typedPtr);
        }
    }
    return ptr;
}

llvm::Value* CodeGenerator::generateCall(CallExpr* call) {
        const TypePtr intrinsicType = resolveType(call->intrinsicType);
        if (call->iteratorOp == IteratorOp::Fold ||
            call->iteratorOp == IteratorOp::ForEach ||
            call->iteratorOp == IteratorOp::Count ||
            call->iteratorOp == IteratorOp::Collect)
            return generateIteratorTerminal(call);
        if (call->iteratorOp != IteratorOp::None) {
            error("iterator adapters are ephemeral and must end in `for`, "
                  "`fold`, `for_each`, `count`, or `collect`");
            return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
        }
        if (auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get());
            calleeId && calleeId->name == "pointer_cast" &&
            call->args.size() == 1) {
            return coerceCallArgument(
                generateExpr(call->args.front().get()), mHelpers->ptrTy());
        }
        if (auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get());
            calleeId && calleeId->name == "drop_callback" &&
            call->args.empty() && !call->typeArgs.empty()) {
            return getOrCreateDropCallback(resolveType(call->typeArgs.front()));
        }
        if (auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get());
            calleeId && (calleeId->name == "Ok" || calleeId->name == "Err") &&
            call->args.size() == 1 && intrinsicType &&
            intrinsicType->kind == TypeKind::Result) {
            const bool isOk = calleeId->name == "Ok";
            TypePtr payloadType = intrinsicType->typeArgs[
                isOk ? 0 : 1];
            llvm::Value* payload = generateExpr(call->args.front().get());
            llvm::Value* bits = packResultPayload(
                payload, payloadType, intrinsicType);
            llvm::Value* result = llvm::UndefValue::get(
                mHelpers->toLLVMType(intrinsicType));
            result = mBuilder->CreateInsertValue(
                result, llvm::ConstantInt::get(mHelpers->boolTy(), isOk ? 1 : 0),
                {0}, isOk ? "ok.tag" : "err.tag");
            return mBuilder->CreateInsertValue(
                result, bits, {1}, isOk ? "ok.value" : "err.value");
        }
        if (auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get());
            calleeId && (calleeId->name == "is_ok" ||
                         calleeId->name == "is_err") &&
            call->args.size() == 1) {
            llvm::Value* result = generateExpr(call->args.front().get());
            llvm::Value* isOk =
                mBuilder->CreateExtractValue(result, {0}, "result.is_ok");
            return calleeId->name == "is_ok"
                ? isOk : mBuilder->CreateNot(isOk, "result.is_err");
        }
        if (auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get());
            calleeId && (calleeId->name == "unwrap" ||
                         calleeId->name == "unwrap_err") &&
            call->args.size() == 1 && intrinsicType &&
            intrinsicType->kind == TypeKind::Result) {
            llvm::Value* result = generateExpr(call->args.front().get());
            llvm::Value* isOk =
                mBuilder->CreateExtractValue(result, {0}, "result.tag");
            const bool wantOk = calleeId->name == "unwrap";
            llvm::Value* valid = wantOk
                ? isOk : mBuilder->CreateNot(isOk, "result.want_err");
            auto* success = llvm::BasicBlock::Create(
                *mCtx, "result.unwrap", mCurrentFunc);
            auto* failure = llvm::BasicBlock::Create(
                *mCtx, "result.unwrap.panic", mCurrentFunc);
            mBuilder->CreateCondBr(valid, success, failure);
            mBuilder->SetInsertPoint(failure);
            auto panic = mModule->getOrInsertFunction(
                "rt_panic_cstr", mHelpers->voidTy(), mHelpers->ptrTy());
            auto* message = mBuilder->CreateGlobalString(
                wantOk ? "called unwrap on Err" :
                         "called unwrap_err on Ok",
                "result.unwrap.message");
            mBuilder->CreateCall(panic, {message});
            mBuilder->CreateUnreachable();
            mBuilder->SetInsertPoint(success);
            llvm::Value* bits =
                mBuilder->CreateExtractValue(result, {1}, "result.payload");
            return unpackResultPayload(
                bits, intrinsicType->typeArgs[wantOk ? 0 : 1]);
        }
        if (auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get());
            calleeId && calleeId->name == "panic" &&
            call->args.size() == 1) {
            llvm::Value* message = generateExpr(call->args.front().get());
            auto panic = mModule->getOrInsertFunction(
                "rt_panic_cstr", mHelpers->voidTy(), mHelpers->ptrTy());
            mBuilder->CreateCall(panic, {
                coerceCallArgument(message, mHelpers->ptrTy())
            });
            mBuilder->CreateUnreachable();
            return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
        }
        if (auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get()); calleeId && calleeId->name == "slice" && call->args.size() == 3) {
            auto* source = generateExpr(call->args[0].get());
            auto* start = coerceCallArgument(generateExpr(call->args[1].get()), mHelpers->i32Ty());
            auto* end = coerceCallArgument(generateExpr(call->args[2].get()), mHelpers->i32Ty());
            // Semantic analysis guarantees a borrowed local array. Its extent is recovered from that binding.
            uint64_t length = 0;
            if (auto* b = dynamic_cast<BorrowExpr*>(call->args[0].get())) if (auto* id = dynamic_cast<IdentifierExpr*>(b->operand.get())) length = mLocalTypes[id->name]->arrayLength;
            auto* checkedStart = mBuilder->CreateCall(mModule->getOrInsertFunction("rt_array_index_or_abort", mHelpers->i32Ty(), mHelpers->i32Ty(), mHelpers->sizeTy()), {start, llvm::ConstantInt::get(mHelpers->sizeTy(), length + 1)}, "slice.start");
            auto* checkedEnd = mBuilder->CreateCall(mModule->getOrInsertFunction("rt_array_index_or_abort", mHelpers->i32Ty(), mHelpers->i32Ty(), mHelpers->sizeTy()), {end, llvm::ConstantInt::get(mHelpers->sizeTy(), length + 1)}, "slice.end");
            auto* valid = mBuilder->CreateICmpSLE(checkedStart, checkedEnd, "slice.order");
            auto* ok = llvm::BasicBlock::Create(*mCtx, "slice.ok", mCurrentFunc); auto* bad = llvm::BasicBlock::Create(*mCtx, "slice.bad", mCurrentFunc);
            mBuilder->CreateCondBr(valid, ok, bad); mBuilder->SetInsertPoint(bad); mBuilder->CreateCall(mModule->getOrInsertFunction("abort", mHelpers->voidTy())); mBuilder->CreateUnreachable(); mBuilder->SetInsertPoint(ok);
            llvm::Value* data = source;
            if (auto* b = dynamic_cast<BorrowExpr*>(call->args[0].get())) {
                if (auto* id = dynamic_cast<IdentifierExpr*>(b->operand.get()); id && mLocals.count(id->name))
                    data = mBuilder->CreateInBoundsGEP(mLocals[id->name]->getAllocatedType(), mLocals[id->name],
                        {llvm::ConstantInt::get(mHelpers->i32Ty(), 0), checkedStart}, "slice.data");
            }
            auto* sliceTy = llvm::StructType::get(*mCtx, {mHelpers->ptrTy(), mHelpers->sizeTy()}); llvm::Value* value = llvm::UndefValue::get(sliceTy);
            value = mBuilder->CreateInsertValue(value, data, {0}); return mBuilder->CreateInsertValue(value, mBuilder->CreateSExtOrTrunc(mBuilder->CreateSub(checkedEnd, checkedStart), mHelpers->sizeTy()), {1});
        }
        if (call->compileTimeValue) {
            if (auto* integer = std::get_if<int64_t>(&*call->compileTimeValue))
                return llvm::ConstantInt::get(mHelpers->i32Ty(), *integer, true);
            if (auto* floating = std::get_if<double>(&*call->compileTimeValue))
                return llvm::ConstantFP::get(mHelpers->f64Ty(), *floating);
            if (auto* boolean = std::get_if<bool>(&*call->compileTimeValue))
                return llvm::ConstantInt::get(mHelpers->boolTy(), *boolean ? 1 : 0);
            if (auto* string = std::get_if<std::string>(&*call->compileTimeValue)) {
                auto* global = mBuilder->CreateGlobalString(*string, "ctstr");
                return mBuilder->CreateGEP(
                    global->getValueType(), global,
                    {llvm::ConstantInt::get(mHelpers->i32Ty(), 0),
                     llvm::ConstantInt::get(mHelpers->i32Ty(), 0)}, "ctstrptr");
            }
        }
        // Device built-ins use the runtime boundary on the host. Kernel bodies
        // retain direct element operations so that the same LLVM function can
        // be cloned to PTX; the CPU simulator invokes that host form directly.
        if (auto* calleeId = dynamic_cast<IdentifierExpr*>(
                call->callee.get())) {
            if (calleeId->name == "gpu_alloc_i32" && call->args.size() == 1) {
                auto alloc = mModule->getOrInsertFunction(
                    "rt_gpu_alloc_i32", mHelpers->ptrTy(), mHelpers->sizeTy());
                auto* count = coerceCallArgument(generateExpr(call->args[0].get()),
                                                  mHelpers->sizeTy());
                return mBuilder->CreateCall(alloc, {count}, "devicealloc");
            }
            if (calleeId->name == "gpu_free" && call->args.size() == 1) {
                auto free = mModule->getOrInsertFunction(
                    "rt_gpu_free", mHelpers->voidTy(), mHelpers->ptrTy());
                auto* buffer = generateDeviceBufferPointer(call->args[0].get());
                mBuilder->CreateCall(free, {buffer});
                return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
            }
            if (calleeId->name == "gpu_load_i32" && call->args.size() == 2) {
                auto* buffer = generateDeviceBufferPointer(call->args[0].get());
                auto* index = coerceCallArgument(generateExpr(call->args[1].get()),
                                                  mHelpers->i32Ty());
                if (mCurrentFunctionIsKernel) {
                    auto* element = mBuilder->CreateGEP(mHelpers->i32Ty(), buffer, index,
                                                        "deviceelem");
                    return mBuilder->CreateLoad(mHelpers->i32Ty(), element, "deviceload");
                }
                auto load = mModule->getOrInsertFunction(
                    "rt_gpu_load_i32", mHelpers->i32Ty(), mHelpers->ptrTy(), mHelpers->i32Ty());
                return mBuilder->CreateCall(load, {buffer, index}, "deviceload");
            }
            if (calleeId->name == "gpu_store_i32" && call->args.size() == 3) {
                auto* buffer = generateDeviceBufferPointer(call->args[0].get());
                auto* index = coerceCallArgument(generateExpr(call->args[1].get()),
                                                  mHelpers->i32Ty());
                auto* value = coerceCallArgument(generateExpr(call->args[2].get()),
                                                  mHelpers->i32Ty());
                if (mCurrentFunctionIsKernel) {
                    auto* element = mBuilder->CreateGEP(mHelpers->i32Ty(), buffer, index,
                                                        "deviceelem");
                    mBuilder->CreateStore(value, element);
                } else {
                    auto store = mModule->getOrInsertFunction(
                        "rt_gpu_store_i32", mHelpers->voidTy(), mHelpers->ptrTy(),
                        mHelpers->i32Ty(), mHelpers->i32Ty());
                    mBuilder->CreateCall(store, {buffer, index, value});
                }
                return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
            }
            if (calleeId->name == "gpu_copy_from_host_i32" && call->args.size() == 3) {
                auto* destination = generateDeviceBufferPointer(call->args[0].get());
                auto* source = generateHostRawPointer(call->args[1].get());
                auto* count = coerceCallArgument(generateExpr(call->args[2].get()),
                                                  mHelpers->i32Ty());
                auto copy = mModule->getOrInsertFunction(
                    "rt_gpu_copy_from_host_i32", mHelpers->i32Ty(), mHelpers->ptrTy(),
                    mHelpers->ptrTy(), mHelpers->i32Ty());
                auto* copied = mBuilder->CreateCall(copy, {destination, source, count}, "gpu.uploaded");
                emitGpuOperationFailureCheck(copied, mCurrentFunc);
                return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
            }
            if (calleeId->name == "gpu_copy_to_host_i32" && call->args.size() == 3) {
                auto* destination = generateHostRawPointer(call->args[0].get());
                auto* source = generateDeviceBufferPointer(call->args[1].get());
                auto* count = coerceCallArgument(generateExpr(call->args[2].get()),
                                                  mHelpers->i32Ty());
                auto copy = mModule->getOrInsertFunction(
                    "rt_gpu_copy_to_host_i32", mHelpers->i32Ty(), mHelpers->ptrTy(),
                    mHelpers->ptrTy(), mHelpers->i32Ty());
                auto* copied = mBuilder->CreateCall(copy, {destination, source, count}, "gpu.downloaded");
                emitGpuOperationFailureCheck(copied, mCurrentFunc);
                return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
            }
        }

        // Language-level print has a fixed Luna runtime ABI. In particular,
        // do not make JIT objects resolve a platform variadic printf: MinGW's
        // wrapper and UCRT export can otherwise use different buffering paths.
        if (auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get())) {
            if (calleeId->name == "print" && !call->args.empty()) {
                for (auto& arg : call->args) {
                    llvm::Value* argVal = generateExpr(arg.get());
                    if (argVal->getType()->isIntegerTy(32)) {
                        auto print = mModule->getOrInsertFunction(
                            "rt_print_i32", mHelpers->voidTy(), mHelpers->i32Ty());
                        mBuilder->CreateCall(print, {argVal});
                    } else {
                        auto print = mModule->getOrInsertFunction(
                            "rt_print_cstr", mHelpers->voidTy(), mHelpers->ptrTy());
                        mBuilder->CreateCall(print, {argVal});
                    }
                }
                return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
            }
        }

        // Runtime fragment dispatch intrinsics emitted by the canonical CFG
        // builder for dynamic single-shot interceptor apply. These have no
        // MoonIR declaration row; resolve them directly to their runtime ABI.
        if (auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get())) {
            if (calleeId->name == "rt_dynamic_fragment_select" &&
                call->args.size() == 2) {
                auto fn = mModule->getOrInsertFunction(
                    "rt_dynamic_fragment_select",
                    mHelpers->ptrTy(), mHelpers->ptrTy(), mHelpers->ptrTy());
                std::vector<llvm::Value*> args;
                for (auto& arg : call->args)
                    args.push_back(coerceCallArgument(
                        generateExpr(arg.get()), mHelpers->ptrTy()));
                return mBuilder->CreateCall(fn, args, "dynamic.fragment.select");
            }
            if (calleeId->name == "rt_dynamic_fragment_matches" &&
                call->args.size() == 2) {
                auto fn = mModule->getOrInsertFunction(
                    "rt_dynamic_fragment_matches",
                    mHelpers->i32Ty(), mHelpers->ptrTy(), mHelpers->ptrTy());
                std::vector<llvm::Value*> args;
                for (auto& arg : call->args)
                    args.push_back(coerceCallArgument(
                        generateExpr(arg.get()), mHelpers->ptrTy()));
                return mBuilder->CreateCall(fn, args, "dynamic.fragment.matches");
            }
            if (calleeId->name == "rt_dynamic_fragment_report_unknown_and_abort") {
                auto fn = mModule->getOrInsertFunction(
                    "rt_dynamic_fragment_report_unknown_and_abort",
                    mHelpers->voidTy(), mHelpers->ptrTy(), mHelpers->ptrTy());
                std::vector<llvm::Value*> args;
                for (auto& arg : call->args)
                    args.push_back(coerceCallArgument(
                        generateExpr(arg.get()), mHelpers->ptrTy()));
                mBuilder->CreateCall(fn, args);
                mBuilder->CreateUnreachable();
                return llvm::PoisonValue::get(mHelpers->i32Ty());
            }
        }

        // Global calls are resolved only through the verified declaration
        // table. Source names never act as a backend lookup fallback.
        if (dynamic_cast<IdentifierExpr*>(call->callee.get())) {
            llvm::Function* callee = call->calleeRef.complete()
                ? resolveFunction(call->calleeRef) : nullptr;
            if (callee) {
                
                std::vector<llvm::Value*> args;
                for (size_t i = 0; i < call->args.size(); ++i) {
                    auto* value = generateExpr(call->args[i].get());
                    if (i < callee->getFunctionType()->getNumParams())
                        value = coerceCallArgument(
                            value, callee->getFunctionType()->getParamType(i));
                    args.push_back(value);
                }
                
                auto* emitted = mBuilder->CreateCall(
                    callee, args,
                    callee->getReturnType()->isVoidTy() ? "" : "calltmp");
                const TypePtr callType = resolveType(call->type);
                if (callType && callType->kind == TypeKind::Never) {
                    mBuilder->CreateUnreachable();
                    return llvm::PoisonValue::get(mHelpers->i32Ty());
                }
                return emitted;
            }

            
        }
        // Every non-direct callable uses its resolved MoonIR function type.
        // This covers closures, statically selected bindings, and dynamic
        // selector results without baking an i32-only ABI into LLVM lowering.
        TypePtr callableType = call->callee
            ? resolveType(call->callee->type) : nullptr;
        if (auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get())) {
            auto localType = mLocalTypes.find(calleeId->name);
            if ((!callableType ||
                 (callableType->kind != TypeKind::Function &&
                  callableType->kind != TypeKind::Closure)) &&
                localType != mLocalTypes.end())
                callableType = localType->second;
            if (call->calleeRef.empty() &&
                mLocals.find(calleeId->name) == mLocals.end()) {
                error("call target '" + calleeId->name +
                      "' has neither a local value nor a verified DeclarationRef");
                return llvm::PoisonValue::get(mHelpers->i32Ty());
            }
        }
        if (callableType && callableType->kind == TypeKind::Function) {
            llvm::Value* functionPointer = generateExpr(call->callee.get());
            std::vector<llvm::Type*> parameterTypes;
            std::vector<llvm::Value*> arguments;
            for (size_t index = 0; index < callableType->paramTypes.size(); ++index)
                parameterTypes.push_back(mHelpers->toLLVMType(callableType->paramTypes[index]));
            for (size_t index = 0; index < call->args.size(); ++index) {
                llvm::Value* argument = generateExpr(call->args[index].get());
                if (index < parameterTypes.size())
                    argument = coerceCallArgument(argument, parameterTypes[index]);
                arguments.push_back(argument);
            }
            auto* returnType = mHelpers->toLLVMType(callableType->returnType);
            auto* functionType = llvm::FunctionType::get(
                returnType, parameterTypes, false);
            return mBuilder->CreateCall(
                functionType, functionPointer, arguments,
                returnType->isVoidTy() ? "" : "indirect.call");
        }
        if (callableType && callableType->kind == TypeKind::Closure) {
            llvm::Value* closureValue = generateExpr(call->callee.get());
            llvm::Type* closureType = mHelpers->toLLVMType(callableType);
            auto* closureStorage = createEntryBlockAlloca(
                mCurrentFunc, closureType, "closure.call");
            mBuilder->CreateStore(closureValue, closureStorage);
            llvm::Value* codePointer = mBuilder->CreateLoad(
                mHelpers->ptrTy(),
                mBuilder->CreateStructGEP(
                    closureType, closureStorage, 0),
                "closure.call.code");
            llvm::Value* environmentPointer = closureStorage;
            std::vector<llvm::Type*> parameterTypes;
            std::vector<llvm::Value*> arguments;
            parameterTypes.push_back(mHelpers->ptrTy());
            arguments.push_back(environmentPointer);
            for (size_t index = 0; index < callableType->paramTypes.size(); ++index)
                parameterTypes.push_back(mHelpers->toLLVMType(callableType->paramTypes[index]));
            for (size_t index = 0; index < call->args.size(); ++index) {
                llvm::Value* argument = generateExpr(call->args[index].get());
                if (index + 1 < parameterTypes.size())
                    argument = coerceCallArgument(argument, parameterTypes[index + 1]);
                arguments.push_back(argument);
            }
            auto* returnType = mHelpers->toLLVMType(callableType->returnType);
            auto* functionType = llvm::FunctionType::get(
                returnType, parameterTypes, false);
            return mBuilder->CreateCall(
                functionType, codePointer, arguments,
                returnType->isVoidTy() ? "" : "closure.call");
        }
        return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
}

llvm::Value* CodeGenerator::generateTry(TryExpr* propagation) {
        llvm::Value* result = generateExpr(propagation->operand.get());
        llvm::Value* isOk =
            mBuilder->CreateExtractValue(result, {0}, "try.is_ok");
        auto* success = llvm::BasicBlock::Create(
            *mCtx, "try.success", mCurrentFunc);
        auto* failure = llvm::BasicBlock::Create(
            *mCtx, "try.error", mCurrentFunc);
        mBuilder->CreateCondBr(isOk, success, failure);
        mBuilder->SetInsertPoint(failure);
        llvm::Value* errorBits =
            mBuilder->CreateExtractValue(result, {1}, "try.error.bits");
        llvm::Value* errorValue =
            unpackResultPayload(
                errorBits, resolveType(propagation->errorType));
        if (!propagation->errorConversion.empty()) {
            llvm::Function* conversion = resolveFunction(
                propagation->errorConversion);
            if (!conversion) {
                error("error propagation references unknown From conversion '" +
                      propagation->errorConversion.symbol.value + "'");
            } else {
                errorValue = mBuilder->CreateCall(
                    conversion,
                    {coerceCallArgument(
                        errorValue,
                        conversion->getFunctionType()->getParamType(0))},
                    "try.converted_error");
            }
        }
        TypePtr propagatedResult = resolveType(
            propagation->propagatedResultType.empty()
                ? propagation->resultType
                : propagation->propagatedResultType);
        TypePtr propagatedError = resolveType(
            propagation->propagatedErrorType.empty()
                ? propagation->errorType
                : propagation->propagatedErrorType);
        llvm::Value* propagatedBits = packResultPayload(
            errorValue, propagatedError, propagatedResult);
        llvm::Value* propagatedValue = llvm::UndefValue::get(
            mHelpers->toLLVMType(propagatedResult));
        propagatedValue = mBuilder->CreateInsertValue(
            propagatedValue,
            llvm::ConstantInt::get(mHelpers->boolTy(), 0),
            {0}, "try.error.tag");
        propagatedValue = mBuilder->CreateInsertValue(
            propagatedValue, propagatedBits, {1}, "try.error.value");
        for (const auto& cleanup : propagation->cleanups)
            emitCleanup(cleanup.place, cleanup.action);
        mBuilder->CreateRet(propagatedValue);
        mBuilder->SetInsertPoint(success);
        llvm::Value* bits =
            mBuilder->CreateExtractValue(result, {1}, "try.value");
        return unpackResultPayload(bits, resolveType(propagation->valueType));
}

llvm::Value* CodeGenerator::generateAssign(AssignExpr* as) {
        llvm::Value* rhs = generateExpr(as->rhs.get());
        moon::Expr* dereferencedOperand = nullptr;
        if (auto* dereference = dynamic_cast<DerefExpr*>(as->lhs.get()))
            dereferencedOperand = dereference->operand.get();
        if (auto* unary = dynamic_cast<UnaryExpr*>(as->lhs.get());
            unary && unary->op == Operator::Dereference)
            dereferencedOperand = unary->operand.get();
        if (dereferencedOperand) {
            llvm::Value* pointer = generateExpr(dereferencedOperand);
            auto* valueType = rhs->getType();
            llvm::Value* result = rhs;
            if (as->op != Operator::Assign) {
                llvm::Value* lhs = mBuilder->CreateLoad(
                    valueType, pointer, "deref.old");
                switch (as->op) {
                    case Operator::AddAssign:
                        result = valueType->isFloatingPointTy()
                            ? mBuilder->CreateFAdd(lhs, rhs, "deref.add")
                            : mBuilder->CreateAdd(lhs, rhs, "deref.add"); break;
                    case Operator::SubtractAssign:
                        result = valueType->isFloatingPointTy()
                            ? mBuilder->CreateFSub(lhs, rhs, "deref.sub")
                            : mBuilder->CreateSub(lhs, rhs, "deref.sub"); break;
                    case Operator::MultiplyAssign:
                        result = valueType->isFloatingPointTy()
                            ? mBuilder->CreateFMul(lhs, rhs, "deref.mul")
                            : mBuilder->CreateMul(lhs, rhs, "deref.mul"); break;
                    case Operator::DivideAssign:
                        result = valueType->isFloatingPointTy()
                            ? mBuilder->CreateFDiv(lhs, rhs, "deref.div")
                            : mBuilder->CreateSDiv(lhs, rhs, "deref.div"); break;
                    default: break;
                }
            }
            mBuilder->CreateStore(result, pointer);
            return result;
        }
        if (auto* index = dynamic_cast<IndexExpr*>(as->lhs.get())) {
            auto* id = dynamic_cast<IdentifierExpr*>(index->object.get());
            llvm::AllocaInst* storage = nullptr;
            TypePtr arrayType;
            if (id && !id->local.empty() &&
                id->local.value < mCanonicalLocals.size() &&
                mCanonicalLocals[id->local.value]) {
                storage = mCanonicalLocals[id->local.value];
                arrayType = id->local.value < mCanonicalLocalTypes.size()
                    ? mCanonicalLocalTypes[id->local.value] : resolveType(id->type);
            } else if (id && mLocals.count(id->name)) {
                storage = mLocals[id->name];
                arrayType = mLocalTypes[id->name];
            }
            if (storage && arrayType && arrayType->kind == TypeKind::Array) {
                auto* rawIndex = coerceCallArgument(
                    generateExpr(index->index.get()), mHelpers->i32Ty());
                auto* checked = mBuilder->CreateCall(
                    mModule->getOrInsertFunction(
                        "rt_array_index_or_abort", mHelpers->i32Ty(),
                        mHelpers->i32Ty(), mHelpers->sizeTy()),
                    {rawIndex, llvm::ConstantInt::get(
                              mHelpers->sizeTy(), arrayType->arrayLength)},
                    "array.index");
                auto* elementPtr = mBuilder->CreateInBoundsGEP(
                    storage->getAllocatedType(), storage,
                    {llvm::ConstantInt::get(mHelpers->i32Ty(), 0), checked},
                    "array.element");
                mBuilder->CreateStore(
                    coerceCallArgument(rhs,
                        mHelpers->toLLVMType(arrayType->inner)), elementPtr);
                return rhs;
            }
        }
        if (auto* field =
                dynamic_cast<FieldAccessExpr*>(
                    as->lhs.get())) {
            TypePtr objectType;
            if (auto* id =
                    dynamic_cast<IdentifierExpr*>(
                        field->object.get())) {
                auto type = mLocalTypes.find(id->name);
                if (type != mLocalTypes.end())
                    objectType = type->second;
            }
            if (objectType &&
                objectType->kind == TypeKind::Reference)
                objectType = objectType->inner;
            const size_t index =
                fieldIndex(objectType, field->field);
            if (objectType &&
                index != static_cast<size_t>(-1)) {
                if (objectType->kind == TypeKind::Record) {
                    auto* objectId = dynamic_cast<IdentifierExpr*>(
                        field->object.get());
                    auto local = objectId
                        ? mLocals.find(objectId->name) : mLocals.end();
                    if (local == mLocals.end()) {
                        error("record field assignment requires a local record binding");
                        return rhs;
                    }
                    auto* pointer = mBuilder->CreateStructGEP(
                        local->second->getAllocatedType(), local->second,
                        static_cast<unsigned>(index), "record.field.assign.ptr");
                    auto* valueType = mHelpers->toLLVMType(
                        objectType->fields[index].type);
                    llvm::Value* result = coerceCallArgument(rhs, valueType);
                    if (as->op != Operator::Assign) {
                        llvm::Value* previous = mBuilder->CreateLoad(
                            valueType, pointer, "record.field.old");
                        switch (as->op) {
                            case Operator::AddAssign:
                                result = valueType->isFloatingPointTy()
                                    ? mBuilder->CreateFAdd(previous, result)
                                    : mBuilder->CreateAdd(previous, result); break;
                            case Operator::SubtractAssign:
                                result = valueType->isFloatingPointTy()
                                    ? mBuilder->CreateFSub(previous, result)
                                    : mBuilder->CreateSub(previous, result); break;
                            case Operator::MultiplyAssign:
                                result = valueType->isFloatingPointTy()
                                    ? mBuilder->CreateFMul(previous, result)
                                    : mBuilder->CreateMul(previous, result); break;
                            case Operator::DivideAssign:
                                result = valueType->isFloatingPointTy()
                                    ? mBuilder->CreateFDiv(previous, result)
                                    : mBuilder->CreateSDiv(previous, result); break;
                            default: break;
                        }
                    }
                    mBuilder->CreateStore(result, pointer);
                    return result;
                }
                llvm::Value* object =
                    generateExpr(field->object.get());
                const uint64_t offset =
                    luna::layout::productFieldOffset(objectType, index);
                llvm::Value* pointer =
                    mBuilder->CreateGEP(
                        llvm::Type::getInt8Ty(*mCtx),
                        object,
                        llvm::ConstantInt::get(
                            mHelpers->sizeTy(), offset),
                        "field.assign.ptr");
                auto* valueType =
                    mHelpers->toLLVMType(
                        objectType->fields[index].type);
                llvm::Value* result =
                    coerceCallArgument(rhs, valueType);
                if (as->op != Operator::Assign) {
                    llvm::Value* previous =
                        mBuilder->CreateLoad(
                            valueType, pointer,
                            "field.assign.old");
                    switch (as->op) {
                        case Operator::AddAssign:
                            result = valueType->isFloatingPointTy()
                                ? mBuilder->CreateFAdd(
                                      previous, result,
                                      "field.assign.add")
                                : mBuilder->CreateAdd(
                                      previous, result,
                                      "field.assign.add");
                            break;
                        case Operator::SubtractAssign:
                            result = valueType->isFloatingPointTy()
                                ? mBuilder->CreateFSub(
                                      previous, result,
                                      "field.assign.sub")
                                : mBuilder->CreateSub(
                                      previous, result,
                                      "field.assign.sub");
                            break;
                        case Operator::MultiplyAssign:
                            result = valueType->isFloatingPointTy()
                                ? mBuilder->CreateFMul(
                                      previous, result,
                                      "field.assign.mul")
                                : mBuilder->CreateMul(
                                      previous, result,
                                      "field.assign.mul");
                            break;
                        case Operator::DivideAssign:
                            result = valueType->isFloatingPointTy()
                                ? mBuilder->CreateFDiv(
                                      previous, result,
                                      "field.assign.div")
                                : mBuilder->CreateSDiv(
                                      previous, result,
                                      "field.assign.div");
                            break;
                        default:
                            break;
                    }
                }
                mBuilder->CreateStore(result, pointer);
                return result;
            }
        }
        if (auto* id = dynamic_cast<IdentifierExpr*>(as->lhs.get())) {
            llvm::AllocaInst* storage = nullptr;
            if (!id->local.empty() &&
                id->local.value < mCanonicalLocals.size())
                storage = mCanonicalLocals[id->local.value];
            if (!storage) {
                auto it = mLocals.find(id->name);
                if (it != mLocals.end()) storage = it->second;
            }
            if (storage) {
                llvm::Value* result = coerceCallArgument(
                    rhs, storage->getAllocatedType());
                if (as->op != Operator::Assign) {
                    llvm::Value* lhs = mBuilder->CreateLoad(
                        storage->getAllocatedType(), storage, id->name + ".old");
                    switch (as->op) {
                        case Operator::AddAssign:
                            result = lhs->getType()->isFloatingPointTy()
                                ? mBuilder->CreateFAdd(lhs, result, "addeqtmp")
                                : mBuilder->CreateAdd(lhs, result, "addeqtmp"); break;
                        case Operator::SubtractAssign:
                            result = lhs->getType()->isFloatingPointTy()
                                ? mBuilder->CreateFSub(lhs, result, "subeqtmp")
                                : mBuilder->CreateSub(lhs, result, "subeqtmp"); break;
                        case Operator::MultiplyAssign:
                            result = lhs->getType()->isFloatingPointTy()
                                ? mBuilder->CreateFMul(lhs, result, "muleqtmp")
                                : mBuilder->CreateMul(lhs, result, "muleqtmp"); break;
                        case Operator::DivideAssign:
                            result = lhs->getType()->isFloatingPointTy()
                                ? mBuilder->CreateFDiv(lhs, result, "diveqtmp")
                                : mBuilder->CreateSDiv(lhs, result, "diveqtmp"); break;
                        case Operator::RemainderAssign:
                            result = lhs->getType()->isFloatingPointTy()
                                ? mBuilder->CreateFRem(lhs, result, "remeqtmp")
                                : mBuilder->CreateSRem(lhs, result, "remeqtmp"); break;
                        case Operator::BitAndAssign: result = mBuilder->CreateAnd(lhs, result, "andeqtmp"); break;
                        case Operator::BitOrAssign: result = mBuilder->CreateOr(lhs, result, "oreqtmp"); break;
                        case Operator::BitXorAssign: result = mBuilder->CreateXor(lhs, result, "oreqtmp"); break;
                        case Operator::ShiftLeftAssign: result = mBuilder->CreateShl(lhs, result, "shleqtmp"); break;
                        case Operator::ShiftRightAssign: result = mBuilder->CreateAShr(lhs, result, "shreqtmp"); break;
                        default: break;
                    }
                }
                mBuilder->CreateStore(result, storage);
                if (id->name.size()) mLocalKnownUpperBounds.erase(id->name);
                return result;
            }
        }
        return rhs;
}

llvm::Value* CodeGenerator::generateMove(MoveExpr* mv) {
        llvm::Value* value = generateExpr(mv->operand.get());
        if (!mv->nextUnread.empty() &&
            mv->nextUnread.value < mCanonicalLocals.size() &&
            mCanonicalLocals[mv->nextUnread.value]) {
            auto* cursorStorage = mCanonicalLocals[mv->nextUnread.value];
            auto* cursor = mBuilder->CreateLoad(
                cursorStorage->getAllocatedType(), cursorStorage,
                "guarded.cursor");
            auto* next = mBuilder->CreateAdd(
                cursor, llvm::ConstantInt::get(cursor->getType(), 1),
                "guarded.cursor.next");
            mBuilder->CreateStore(next, cursorStorage);
        }
        return value;
}

llvm::Value* CodeGenerator::generateBorrow(BorrowExpr* bw) {
        if (auto* id = dynamic_cast<IdentifierExpr*>(bw->operand.get())) {
            auto it = mLocals.find(id->name);
            if (it != mLocals.end()) {
                auto type = mLocalTypes.find(id->name);
                if (type != mLocalTypes.end() && type->second &&
                    (type->second->kind == TypeKind::Struct ||
                     type->second->kind == TypeKind::Record))
                    return mBuilder->CreateLoad(
                        it->second->getAllocatedType(), it->second,
                        id->name + ".borrowed_object");
                return it->second;
            }
        }
        return generateExpr(bw->operand.get());
}

llvm::Value* CodeGenerator::generateDeref(DerefExpr* dr) {
        llvm::Value* op = generateExpr(dr->operand.get());
        auto* ptr = mBuilder->CreateBitCast(op, llvm::PointerType::get(*mCtx, 0));
        const TypePtr dereferencedType = resolveType(dr->type);
        llvm::Type* valueType = dereferencedType
            ? mHelpers->toLLVMType(dereferencedType)
            : mHelpers->i32Ty();
        return mBuilder->CreateLoad(valueType, ptr, "deref");
}

llvm::Value* CodeGenerator::generateAddrOf(AddrOfExpr* ad) {
        if (auto* id = dynamic_cast<IdentifierExpr*>(ad->operand.get())) {
            auto it = mLocals.find(id->name);
            if (it != mLocals.end()) return it->second;
        }
        return generateExpr(ad->operand.get());
}

llvm::Value* CodeGenerator::generateLambda(LambdaExpr* le) {
        // Generate a hidden function for the lambda body
        static int lambdaCount = 0;
        std::string lambdaName = "__lambda_" + std::to_string(lambdaCount++);
        if (!le->identitySuffix.empty()) lambdaName += "__" + le->identitySuffix;

        // Build LLVM function for lambda
        std::vector<llvm::Type*> paramTypes;
        for (auto& p : le->params)
            paramTypes.push_back(mHelpers->toLLVMType(resolveType(p.type)));
        const TypePtr lambdaReturnType = resolveType(le->returnType);
        llvm::Type* retTy = lambdaReturnType
            ? mHelpers->toLLVMType(lambdaReturnType) : mHelpers->i32Ty();
        auto funcTy = llvm::FunctionType::get(retTy, paramTypes, false);
        auto func = llvm::Function::Create(
            funcTy, llvm::Function::InternalLinkage, lambdaName, mModule.get());

        // Save caller state (including insert point)
        auto savedFunc = mCurrentFunc;
        auto savedLocals = std::move(mLocals);
        auto savedLocalTypes = std::move(mLocalTypes);
        auto savedArrayDropFlags =
            std::move(mArrayDropFlags);
        auto savedMaterializedIterators =
            std::move(mMaterializedIterators);
        auto savedUpperBounds = std::move(mLocalKnownUpperBounds);
        auto savedContinuationFrames = std::move(mContinuationFrames);
        const unsigned savedContinuationFrameCounter = mContinuationFrameCounter;
        auto savedCanonicalLocals = std::move(mCanonicalLocals);
        auto savedCanonicalLocalTypes = std::move(mCanonicalLocalTypes);
        auto savedIP = mBuilder->saveIP();
        mLocals.clear();
        mLocalTypes.clear();
        mArrayDropFlags.clear();
        mMaterializedIterators.clear();
        mLocalKnownUpperBounds.clear();
        mContinuationFrames.clear();
        mContinuationFrameCounter = 0;
        mCurrentFunc = func;

        auto entryBB = llvm::BasicBlock::Create(*mCtx, "entry", func);
        mBuilder->SetInsertPoint(entryBB);

        if (le->controlFlow) {
            generateControlFlowBody(*le->controlFlow, func, entryBB);
        } else {
            size_t idx = 0;
            for (auto& arg : func->args()) {
                arg.setName(le->params[idx].name);
                auto* alloca = createEntryBlockAlloca(
                    func, arg.getType(), le->params[idx].name);
                mBuilder->CreateStore(&arg, alloca);
                mLocals[le->params[idx].name] = alloca;
                mLocalTypes[le->params[idx].name] =
                    resolveType(le->params[idx].type);
                idx++;
            }
            if (le->body) generateBlock(le->body.get(), func);
        }
        if (!mBuilder->GetInsertBlock()->getTerminator()) {
            if (retTy->isVoidTy()) mBuilder->CreateRetVoid();
            else mBuilder->CreateRet(llvm::Constant::getNullValue(retTy));
        }

        // Restore state (including insert point)
        mCurrentFunc = savedFunc;
        mLocals = std::move(savedLocals);
        mLocalTypes = std::move(savedLocalTypes);
        mArrayDropFlags =
            std::move(savedArrayDropFlags);
        mMaterializedIterators =
            std::move(savedMaterializedIterators);
        mLocalKnownUpperBounds = std::move(savedUpperBounds);
        mContinuationFrames = std::move(savedContinuationFrames);
        mContinuationFrameCounter = savedContinuationFrameCounter;
        mCanonicalLocals = std::move(savedCanonicalLocals);
        mCanonicalLocalTypes = std::move(savedCanonicalLocalTypes);
        mBuilder->restoreIP(savedIP);
        return func;
}

llvm::Value* CodeGenerator::generateEnvLoad(EnvLoadExpr* envLoad) {
        if (envLoad->envLocal.empty() ||
            envLoad->envLocal.value >= mCanonicalLocals.size() ||
            !mCanonicalLocals[envLoad->envLocal.value]) {
            error("environment load has no canonical local storage");
            return llvm::PoisonValue::get(mHelpers->i32Ty());
        }
        const TypePtr closureType =
            mCanonicalLocalTypes[envLoad->envLocal.value];
        if (!closureType || closureType->kind != TypeKind::Closure ||
            envLoad->fieldIndex >= closureType->capturedFields.size()) {
            error("environment load is not bound to a valid closure field");
            return llvm::PoisonValue::get(mHelpers->i32Ty());
        }
        const TypePtr fieldType =
            closureType->capturedFields[envLoad->fieldIndex].type;
        auto* closureLLVMType = mHelpers->toLLVMType(closureType);
        auto* fieldPointer = mBuilder->CreateStructGEP(
            closureLLVMType,
            mCanonicalLocals[envLoad->envLocal.value],
            envLoad->fieldIndex + 1);
        return mBuilder->CreateLoad(
            mHelpers->toLLVMType(fieldType), fieldPointer, "env.load");
}

llvm::Value* CodeGenerator::generateMakeClosure(moon::MakeClosureExpr* closure) {
        auto* le = closure->lambda.get();
        static int lambdaCount = 0;
        std::string lambdaName = "__lambda_" + std::to_string(lambdaCount++);
        if (!le->identitySuffix.empty()) lambdaName += "__" + le->identitySuffix;

        const TypePtr closureType = resolveType(closure->type);
        std::vector<llvm::Type*> paramTypes;
        paramTypes.push_back(mHelpers->ptrTy());
        for (auto& p : le->params)
            paramTypes.push_back(mHelpers->toLLVMType(resolveType(p.type)));
        const TypePtr lambdaReturnType = resolveType(le->returnType);
        llvm::Type* retTy = lambdaReturnType
            ? mHelpers->toLLVMType(lambdaReturnType) : mHelpers->i32Ty();
        auto funcTy = llvm::FunctionType::get(retTy, paramTypes, false);
        auto func = llvm::Function::Create(
            funcTy, llvm::Function::InternalLinkage, lambdaName, mModule.get());

        auto savedFunc = mCurrentFunc;
        auto savedLocals = std::move(mLocals);
        auto savedLocalTypes = std::move(mLocalTypes);
        auto savedArrayDropFlags =
            std::move(mArrayDropFlags);
        auto savedMaterializedIterators =
            std::move(mMaterializedIterators);
        auto savedUpperBounds = std::move(mLocalKnownUpperBounds);
        auto savedContinuationFrames = std::move(mContinuationFrames);
        const unsigned savedContinuationFrameCounter = mContinuationFrameCounter;
        auto savedCanonicalLocals = std::move(mCanonicalLocals);
        auto savedCanonicalLocalTypes = std::move(mCanonicalLocalTypes);
        auto savedIP = mBuilder->saveIP();
        mLocals.clear();
        mLocalTypes.clear();
        mArrayDropFlags.clear();
        mMaterializedIterators.clear();
        mLocalKnownUpperBounds.clear();
        mContinuationFrames.clear();
        mContinuationFrameCounter = 0;
        mCurrentFunc = func;

        auto entryBB = llvm::BasicBlock::Create(*mCtx, "entry", func);
        mBuilder->SetInsertPoint(entryBB);

        llvm::Value* environmentPointer = nullptr;
        size_t idx = 0;
        for (auto& arg : func->args()) {
            if (idx == 0) {
                environmentPointer = &arg;
                ++idx;
                continue;
            }
            const size_t paramIndex = idx - 1;
            arg.setName(le->params[paramIndex].name);
            auto* alloca = createEntryBlockAlloca(
                func, arg.getType(), le->params[paramIndex].name);
            mBuilder->CreateStore(&arg, alloca);
            mLocals[le->params[paramIndex].name] = alloca;
            mLocalTypes[le->params[paramIndex].name] =
                resolveType(le->params[paramIndex].type);
            ++idx;
        }

        llvm::Type* closureLLVMType = mHelpers->toLLVMType(closureType);
        if (le->controlFlow) {
            generateControlFlowBody(*le->controlFlow, func, entryBB);
        } else {
            for (size_t fieldIndex = 0;
                 fieldIndex < closure->capturedValues.size(); ++fieldIndex) {
                auto* reference = dynamic_cast<IdentifierExpr*>(
                    closure->capturedValues[fieldIndex].get());
                if (!reference) continue;
                TypePtr fieldType;
                if (closureType &&
                    fieldIndex < closureType->capturedFields.size())
                    fieldType = closureType->capturedFields[fieldIndex].type;
                if (!fieldType) fieldType = resolveType(reference->type);
                auto* fieldPointer = mBuilder->CreateStructGEP(
                    closureLLVMType, environmentPointer, fieldIndex + 1);
                auto* loaded = mBuilder->CreateLoad(
                    mHelpers->toLLVMType(fieldType), fieldPointer,
                    reference->name);
                auto* alloca = createEntryBlockAlloca(
                    func, mHelpers->toLLVMType(fieldType), reference->name);
                mBuilder->CreateStore(loaded, alloca);
                mLocals[reference->name] = alloca;
                mLocalTypes[reference->name] = fieldType;
            }
            if (le->body) generateBlock(le->body.get(), func);
        }
        if (!mBuilder->GetInsertBlock()->getTerminator()) {
            if (retTy->isVoidTy()) mBuilder->CreateRetVoid();
            else mBuilder->CreateRet(llvm::Constant::getNullValue(retTy));
        }

        mCurrentFunc = savedFunc;
        mLocals = std::move(savedLocals);
        mLocalTypes = std::move(savedLocalTypes);
        mArrayDropFlags =
            std::move(savedArrayDropFlags);
        mMaterializedIterators =
            std::move(savedMaterializedIterators);
        mLocalKnownUpperBounds = std::move(savedUpperBounds);
        mContinuationFrames = std::move(savedContinuationFrames);
        mContinuationFrameCounter = savedContinuationFrameCounter;
        mCanonicalLocals = std::move(savedCanonicalLocals);
        mCanonicalLocalTypes = std::move(savedCanonicalLocalTypes);
        mBuilder->restoreIP(savedIP);

        auto* closureStorage = mBuilder->CreateAlloca(closureLLVMType);
        mBuilder->CreateStore(
            func, mBuilder->CreateStructGEP(
                closureLLVMType, closureStorage, 0));
        for (size_t fieldIndex = 0;
             fieldIndex < closure->capturedValues.size(); ++fieldIndex) {
            llvm::Value* value =
                generateExpr(closure->capturedValues[fieldIndex].get());
            TypePtr fieldType;
            if (closureType &&
                fieldIndex < closureType->capturedFields.size())
                fieldType = closureType->capturedFields[fieldIndex].type;
            auto* fieldPointer = mBuilder->CreateStructGEP(
                closureLLVMType, closureStorage, fieldIndex + 1);
            if (fieldType)
                value = coerceCallArgument(
                    value, mHelpers->toLLVMType(fieldType));
            mBuilder->CreateStore(value, fieldPointer);
        }
        return mBuilder->CreateLoad(closureLLVMType, closureStorage, "closure.value");
}

llvm::Value* CodeGenerator::generateExpr(Expr* expr) {
    if (auto* il = dynamic_cast<IntLiteralExpr*>(expr))
        return generateIntLiteral(il);
    if (auto* fl = dynamic_cast<FloatLiteralExpr*>(expr))
        return generateFloatLiteral(fl);
    if (auto* sl = dynamic_cast<StringLiteralExpr*>(expr))
        return generateStringLiteral(sl);
    if (auto* bl = dynamic_cast<BoolLiteralExpr*>(expr))
        return generateBoolLiteral(bl);
    if (auto* unit = dynamic_cast<UnitExpr*>(expr))
        return generateUnitLiteral(unit);
    if (auto* id = dynamic_cast<IdentifierExpr*>(expr))
        return generateIdentifier(id);
    if (auto* selection = dynamic_cast<DynamicSelectExpr*>(expr))
        return generateDynamicSelect(selection);
    if (auto* bin = dynamic_cast<BinaryExpr*>(expr))
        if (auto* value = generateBinary(bin)) return value;
    if (auto* un = dynamic_cast<UnaryExpr*>(expr))
        if (auto* value = generateUnary(un)) return value;
    if (auto* variant = dynamic_cast<VariantConstructExpr*>(expr))
        return generateVariantConstruct(variant);
    if (auto* result = dynamic_cast<ResultConstructExpr*>(expr))
        return generateResultConstruct(result);
    if (auto* record = dynamic_cast<RecordLiteralExpr*>(expr))
        return generateRecordLiteral(record);
    if (auto* field = dynamic_cast<FieldAccessExpr*>(expr))
        return generateFieldAccess(field);
    if (auto* array = dynamic_cast<ArrayLiteralExpr*>(expr))
        return generateArrayLiteral(array);
    if (auto* length = dynamic_cast<SliceLengthExpr*>(expr))
        return generateSliceLength(length);
    if (auto* index = dynamic_cast<IndexExpr*>(expr))
        return generateIndex(index);
    if (auto* launch = dynamic_cast<LaunchExpr*>(expr)) return generateLaunch(launch);
    if (auto* call = dynamic_cast<CallExpr*>(expr))
        return generateCall(call);
    if (auto* initialized = dynamic_cast<InitAllocationExpr*>(expr))
        return generateInitAllocation(initialized);
    if (auto* ha = dynamic_cast<HeapAllocExpr*>(expr))
        return generateHeapAlloc(ha);
    if (auto* propagation = dynamic_cast<TryExpr*>(expr))
        return generateTry(propagation);
    if (auto* as = dynamic_cast<AssignExpr*>(expr))
        return generateAssign(as);
    if (auto* mv = dynamic_cast<MoveExpr*>(expr))
        return generateMove(mv);
    if (auto* bw = dynamic_cast<BorrowExpr*>(expr))
        return generateBorrow(bw);
    if (auto* dr = dynamic_cast<DerefExpr*>(expr))
        return generateDeref(dr);
    if (auto* ad = dynamic_cast<AddrOfExpr*>(expr))
        return generateAddrOf(ad);
    if (auto* le = dynamic_cast<LambdaExpr*>(expr))
        return generateLambda(le);
    if (auto* envLoad = dynamic_cast<EnvLoadExpr*>(expr))
        return generateEnvLoad(envLoad);
    if (auto* closure = dynamic_cast<moon::MakeClosureExpr*>(expr))
        return generateMakeClosure(closure);

    error("codegen has no LLVM lowering for this expression kind");
    return llvm::PoisonValue::get(mHelpers->i32Ty());
}
