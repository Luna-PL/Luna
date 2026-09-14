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

llvm::Value* CodeGenerator::generateAssign(AssignExpr* as) {
        llvm::Value* rhs = generateExpr(as->rhs.get());
        const bool isUnsigned = isUnsignedIntegerType(
            resolveType(as->lhs->type));
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
                            : (isUnsigned
                                ? mBuilder->CreateUDiv(lhs, rhs, "deref.div")
                                : mBuilder->CreateSDiv(lhs, rhs, "deref.div")); break;
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
            llvm::AllocaInst* objectStorage = nullptr;
            if (auto* id =
                    dynamic_cast<IdentifierExpr*>(
                        field->object.get())) {
                auto type = mLocalTypes.find(id->name);
                if (type != mLocalTypes.end())
                    objectType = type->second;
                auto local = mLocals.find(id->name);
                if (local != mLocals.end())
                    objectStorage = local->second;
                // Canonical CFG path: locals are indexed by LocalId.
                if (!objectType && !id->local.empty() &&
                    id->local.value < mCanonicalLocalTypes.size())
                    objectType = mCanonicalLocalTypes[id->local.value];
                if (!objectStorage && !id->local.empty() &&
                    id->local.value < mCanonicalLocals.size())
                    objectStorage = mCanonicalLocals[id->local.value];
            }
            if (objectType &&
                objectType->kind == TypeKind::Reference)
                objectType = objectType->inner;
            const size_t index =
                fieldIndex(objectType, field->field);
            if (objectType && objectStorage &&
                index != static_cast<size_t>(-1)) {
                if (objectType->kind == TypeKind::Record) {
                    auto* pointer = mBuilder->CreateStructGEP(
                        objectStorage->getAllocatedType(), objectStorage,
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
                                    : (isUnsigned
                                        ? mBuilder->CreateUDiv(previous, result)
                                        : mBuilder->CreateSDiv(previous, result)); break;
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
                                : (isUnsigned
                                    ? mBuilder->CreateUDiv(
                                          previous, result,
                                          "field.assign.div")
                                    : mBuilder->CreateSDiv(
                                          previous, result,
                                          "field.assign.div"));
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
                                : (isUnsigned
                                    ? mBuilder->CreateUDiv(lhs, result, "diveqtmp")
                                    : mBuilder->CreateSDiv(lhs, result, "diveqtmp")); break;
                        case Operator::RemainderAssign:
                            result = lhs->getType()->isFloatingPointTy()
                                ? mBuilder->CreateFRem(lhs, result, "remeqtmp")
                                : (isUnsigned
                                    ? mBuilder->CreateURem(lhs, result, "remeqtmp")
                                    : mBuilder->CreateSRem(lhs, result, "remeqtmp")); break;
                        case Operator::BitAndAssign: result = mBuilder->CreateAnd(lhs, result, "andeqtmp"); break;
                        case Operator::BitOrAssign: result = mBuilder->CreateOr(lhs, result, "oreqtmp"); break;
                        case Operator::BitXorAssign: result = mBuilder->CreateXor(lhs, result, "oreqtmp"); break;
                        case Operator::ShiftLeftAssign: result = mBuilder->CreateShl(lhs, result, "shleqtmp"); break;
                        case Operator::ShiftRightAssign:
                            result = isUnsigned
                                ? mBuilder->CreateLShr(lhs, result, "shreqtmp")
                                : mBuilder->CreateAShr(lhs, result, "shreqtmp");
                            break;
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
            // Canonical CFG path: locals are indexed by LocalId, not name.
            if (!id->local.empty() &&
                id->local.value < mCanonicalLocals.size() &&
                mCanonicalLocals[id->local.value]) {
                auto* alloca = mCanonicalLocals[id->local.value];
                auto localType = id->local.value < mCanonicalLocalTypes.size()
                    ? mCanonicalLocalTypes[id->local.value] : nullptr;
                if (localType &&
                    (localType->kind == TypeKind::Struct ||
                     localType->kind == TypeKind::Record))
                    return mBuilder->CreateLoad(
                        alloca->getAllocatedType(), alloca,
                        id->name.empty()
                            ? "local." + std::to_string(id->local.value) +
                              ".borrowed_object"
                            : id->name + ".borrowed_object");
                // A pointer-typed local (&i32, raw<T>, device_buffer<T>)
                // already stores a pointer. Borrowing it returns the stored
                // pointer, not the alloca address, so the callee receives
                // the element address directly rather than a pointer-to-
                // pointer.
                if (localType &&
                    (localType->kind == TypeKind::Reference ||
                     localType->kind == TypeKind::RawPointer))
                    return mBuilder->CreateLoad(
                        alloca->getAllocatedType(), alloca,
                        id->name.empty()
                            ? "local." + std::to_string(id->local.value) +
                              ".ref"
                            : id->name + ".ref");
                return alloca;
            }
        }
        // A BorrowExpr wrapping an IndexExpr borrows a specific array
        // element. Return the element pointer (GEP) rather than loading
        // the value, so the borrowed reference is the address of the
        // element inside the source array.
        if (auto* idx = dynamic_cast<IndexExpr*>(bw->operand.get())) {
            auto* id = dynamic_cast<IdentifierExpr*>(idx->object.get());
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
                    generateExpr(idx->index.get()), mHelpers->i32Ty());
                llvm::Value* checked = rawIndex;
                if (!luna::codegen::isProvablySafeArrayIndex(
                        idx->index.get(), arrayType->arrayLength,
                        mLocalKnownUpperBounds)) {
                    checked = emitCheckedArrayIndex(
                        rawIndex,
                        llvm::ConstantInt::get(
                            mHelpers->sizeTy(), arrayType->arrayLength),
                        "borrow.array.index");
                }
                return mBuilder->CreateInBoundsGEP(
                    storage->getAllocatedType(), storage,
                    {llvm::ConstantInt::get(mHelpers->i32Ty(), 0), checked},
                    "borrow.array.element");
            }
            // For a slice source ({ ptr, i64 }), load the slice value,
            // extract its data pointer and length, bounds-check the index,
            // and return the element pointer without loading the element.
            if (storage && arrayType && arrayType->kind == TypeKind::Slice) {
                auto* sliceValue = mBuilder->CreateLoad(
                    storage->getAllocatedType(), storage, "borrow.slice.value");
                auto* length = mBuilder->CreateExtractValue(
                    sliceValue, {1}, "borrow.slice.length");
                auto* rawIndex = coerceCallArgument(
                    generateExpr(idx->index.get()), mHelpers->i32Ty());
                auto* checked = emitCheckedArrayIndex(
                    rawIndex, length, "borrow.slice.index");
                auto* data = mBuilder->CreateExtractValue(
                    sliceValue, {0}, "borrow.slice.data");
                return mBuilder->CreateGEP(
                    mHelpers->toLLVMType(arrayType->inner), data, checked,
                    "borrow.slice.element");
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
            // Canonical CFG path: locals are indexed by LocalId, not name.
            if (!id->local.empty() &&
                id->local.value < mCanonicalLocals.size() &&
                mCanonicalLocals[id->local.value])
                return mCanonicalLocals[id->local.value];
        }
        return generateExpr(ad->operand.get());
}

llvm::Value* CodeGenerator::generateLambda(LambdaExpr* le) {
        if (!le->controlFlow || le->body) {
            error("lambda reached code generation without an exclusive canonical CFG body");
            return llvm::PoisonValue::get(mHelpers->ptrTy());
        }
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
        auto savedCanonicalLocals = std::move(mCanonicalLocals);
        auto savedCanonicalLocalTypes = std::move(mCanonicalLocalTypes);
        auto savedIP = mBuilder->saveIP();
        mLocals.clear();
        mLocalTypes.clear();
        mArrayDropFlags.clear();
        mMaterializedIterators.clear();
        mLocalKnownUpperBounds.clear();
        mCurrentFunc = func;

        auto entryBB = llvm::BasicBlock::Create(*mCtx, "entry", func);
        mBuilder->SetInsertPoint(entryBB);

        generateControlFlowBody(*le->controlFlow, func, entryBB);
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
        const TypePtr closureType = resolveType(closure->type);
        llvm::Type* closureLLVMType = mHelpers->toLLVMType(closureType);
        if (!le->controlFlow || le->body) {
            error("closure lambda reached code generation without an exclusive canonical CFG body");
            return llvm::PoisonValue::get(closureLLVMType);
        }
        static int lambdaCount = 0;
        std::string lambdaName = "__lambda_" + std::to_string(lambdaCount++);
        if (!le->identitySuffix.empty()) lambdaName += "__" + le->identitySuffix;

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
        auto savedCanonicalLocals = std::move(mCanonicalLocals);
        auto savedCanonicalLocalTypes = std::move(mCanonicalLocalTypes);
        auto savedIP = mBuilder->saveIP();
        mLocals.clear();
        mLocalTypes.clear();
        mArrayDropFlags.clear();
        mMaterializedIterators.clear();
        mLocalKnownUpperBounds.clear();
        mCurrentFunc = func;

        auto entryBB = llvm::BasicBlock::Create(*mCtx, "entry", func);
        mBuilder->SetInsertPoint(entryBB);

        generateControlFlowBody(*le->controlFlow, func, entryBB);
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
