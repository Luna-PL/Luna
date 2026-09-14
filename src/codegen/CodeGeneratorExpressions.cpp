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
    const TypePtr type = resolveType(il->type);
    llvm::Type* llvmType = type && isIntegerType(type)
        ? mHelpers->toLLVMType(type) : mHelpers->i32Ty();
    return llvm::ConstantInt::get(
        llvmType, static_cast<uint64_t>(il->value),
        !isUnsignedIntegerType(type));
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

llvm::Value* CodeGenerator::emitCheckedArrayIndex(
    llvm::Value* index, llvm::Value* length, const std::string& label) {
    index = coerceCallArgument(index, mHelpers->i32Ty());
    length = coerceCallArgument(length, mHelpers->sizeTy());

    auto* nonNegative = mBuilder->CreateICmpSGE(
        index, llvm::ConstantInt::get(mHelpers->i32Ty(), 0),
        label + ".nonnegative");
    auto* widened = mBuilder->CreateZExtOrTrunc(
        index, mHelpers->sizeTy(), label + ".wide");
    auto* belowLength = mBuilder->CreateICmpULT(
        widened, length, label + ".below_length");
    auto* valid = mBuilder->CreateAnd(
        nonNegative, belowLength, label + ".valid");
    auto* success = llvm::BasicBlock::Create(
        *mCtx, label + ".ok", mCurrentFunc);
    auto* failure = llvm::BasicBlock::Create(
        *mCtx, label + ".fail", mCurrentFunc);
    mBuilder->CreateCondBr(valid, success, failure);

    mBuilder->SetInsertPoint(failure);
    auto check = mModule->getOrInsertFunction(
        "rt_array_index_or_abort", mHelpers->i32Ty(),
        mHelpers->i32Ty(), mHelpers->sizeTy());
    mBuilder->CreateCall(check, {index, length});
    mBuilder->CreateUnreachable();

    mBuilder->SetInsertPoint(success);
    return index;
}

llvm::Value* CodeGenerator::generateArrayLiteral(ArrayLiteralExpr* array) {
    TypePtr arrayType = Type::makeArray(
        resolveType(array->elementType), array->elements.size());
    auto* llvmArray = llvm::cast<llvm::ArrayType>(mHelpers->toLLVMType(arrayType));
    std::vector<llvm::Value*> elements;
    elements.reserve(array->elements.size());
    std::vector<llvm::Constant*> constants;
    constants.reserve(array->elements.size());
    bool allConstant = true;
    for (const auto& element : array->elements) {
        llvm::Value* value = generateExpr(element.get());
        elements.push_back(value);
        if (auto* constant = llvm::dyn_cast<llvm::Constant>(value))
            constants.push_back(constant);
        else
            allConstant = false;
    }
    if (allConstant)
        return llvm::ConstantArray::get(llvmArray, constants);

    llvm::Value* result = llvm::UndefValue::get(llvmArray);
    for (size_t i = 0; i < elements.size(); ++i)
        result = mBuilder->CreateInsertValue(result, elements[i],
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
        auto* checked = emitCheckedArrayIndex(
            generateExpr(index->index.get()), length, "slice.index");
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
    llvm::Value* checked = rawIndex;
    if (!luna::codegen::isProvablySafeArrayIndex(
            index->index.get(), arrayType->arrayLength,
            mLocalKnownUpperBounds)) {
        checked = emitCheckedArrayIndex(
            rawIndex,
            llvm::ConstantInt::get(
                mHelpers->sizeTy(), arrayType->arrayLength),
            "array.index");
    }
    auto* elementPtr = mBuilder->CreateInBoundsGEP(
        storage->getAllocatedType(), storage,
        {llvm::ConstantInt::get(mHelpers->i32Ty(), 0), checked},
        "array.element");
    return mBuilder->CreateLoad(
        mHelpers->toLLVMType(arrayType->inner), elementPtr, "array.load");
}

llvm::Value* CodeGenerator::generateBinary(BinaryExpr* bin) {
    llvm::Value* lhs = generateExpr(bin->lhs.get());
    const TypePtr operandType = resolveType(bin->lhs->type);
    const bool isUnsigned = isUnsignedIntegerType(operandType);
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
    if ((bin->op == Operator::ShiftLeft ||
         bin->op == Operator::ShiftRight) &&
        lhs->getType()->isIntegerTy() && rhs->getType()->isIntegerTy() &&
        lhs->getType() != rhs->getType()) {
        rhs = mBuilder->CreateZExtOrTrunc(
            rhs, lhs->getType(), "shift.count");
    }
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
                : (isUnsigned
                    ? mBuilder->CreateUDiv(lhs, rhs, "divtmp")
                    : mBuilder->CreateSDiv(lhs, rhs, "divtmp"));
        case Operator::Remainder:
            return lhs->getType()->isFloatingPointTy()
                ? mBuilder->CreateFRem(lhs, rhs, "remtmp")
                : (isUnsigned
                    ? mBuilder->CreateURem(lhs, rhs, "remtmp")
                    : mBuilder->CreateSRem(lhs, rhs, "remtmp"));
        case Operator::BitAnd:
            return mBuilder->CreateAnd(lhs, rhs, "bitandtmp");
        case Operator::BitOr:
            return mBuilder->CreateOr(lhs, rhs, "bitortmp");
        case Operator::BitXor:
            return mBuilder->CreateXor(lhs, rhs, "bitxortmp");
        case Operator::ShiftLeft:
            return mBuilder->CreateShl(lhs, rhs, "shltmp");
        case Operator::ShiftRight:
            return isUnsigned
                ? mBuilder->CreateLShr(lhs, rhs, "shrtmp")
                : mBuilder->CreateAShr(lhs, rhs, "shrtmp");
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
                : (isUnsigned
                    ? mBuilder->CreateICmpULT(lhs, rhs, "lttmp")
                    : mBuilder->CreateICmpSLT(lhs, rhs, "lttmp"));
        case Operator::LessEqual:
            return lhs->getType()->isFloatingPointTy()
                ? mBuilder->CreateFCmpOLE(lhs, rhs, "letmp")
                : (isUnsigned
                    ? mBuilder->CreateICmpULE(lhs, rhs, "letmp")
                    : mBuilder->CreateICmpSLE(lhs, rhs, "letmp"));
        case Operator::Greater:
            return lhs->getType()->isFloatingPointTy()
                ? mBuilder->CreateFCmpOGT(lhs, rhs, "gttmp")
                : (isUnsigned
                    ? mBuilder->CreateICmpUGT(lhs, rhs, "gttmp")
                    : mBuilder->CreateICmpSGT(lhs, rhs, "gttmp"));
        case Operator::GreaterEqual:
            return lhs->getType()->isFloatingPointTy()
                ? mBuilder->CreateFCmpOGE(lhs, rhs, "getmp")
                : (isUnsigned
                    ? mBuilder->CreateICmpUGE(lhs, rhs, "getmp")
                    : mBuilder->CreateICmpSGE(lhs, rhs, "getmp"));
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

llvm::Value* CodeGenerator::generateExpr(Expr* expr) {
    if (!expr) {
        error("codegen was asked to lower a null expression");
        return llvm::PoisonValue::get(mHelpers->i32Ty());
    }
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
