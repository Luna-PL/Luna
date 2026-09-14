#include "../core/TypeLayout.h"
#include "CodeGenerator.h"
#include "CodeGeneratorRangeAnalysis.h"

#include <algorithm>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <llvm/IR/Intrinsics.h>

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

llvm::Value* CodeGenerator::generateCall(CallExpr* call) {
    const TypePtr intrinsicType = resolveType(call->intrinsicType);
    if (call->iteratorOp == IteratorOp::Fold || call->iteratorOp == IteratorOp::ForEach ||
        call->iteratorOp == IteratorOp::Count || call->iteratorOp == IteratorOp::Collect)
        return generateIteratorTerminal(call);
    if (call->iteratorOp != IteratorOp::None) {
        error("iterator adapters are ephemeral and must end in `for`, "
              "`fold`, `for_each`, `count`, or `collect`");
        return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
    }
    if (auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get());
        calleeId && calleeId->name == "popcount_u32" && call->args.size() == 1) {
        llvm::Value* value = generateExpr(call->args.front().get());
        auto* intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
            mModule.get(), llvm::Intrinsic::ctpop, {mHelpers->i32Ty()});
        return mBuilder->CreateCall(intrinsic, {value}, "popcount");
    }
    if (auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get());
        calleeId && calleeId->name == "pointer_cast" && call->args.size() == 1) {
        return coerceCallArgument(generateExpr(call->args.front().get()), mHelpers->ptrTy());
    }
    if (auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get());
        calleeId && calleeId->name == "drop_callback" && call->args.empty() &&
        !call->typeArgs.empty()) {
        return getOrCreateDropCallback(resolveType(call->typeArgs.front()));
    }
    if (auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get());
        calleeId && (calleeId->name == "Ok" || calleeId->name == "Err") && call->args.size() == 1 &&
        intrinsicType && intrinsicType->kind == TypeKind::Result) {
        const bool isOk = calleeId->name == "Ok";
        TypePtr payloadType = intrinsicType->typeArgs[isOk ? 0 : 1];
        llvm::Value* payload = generateExpr(call->args.front().get());
        llvm::Value* bits = packResultPayload(payload, payloadType, intrinsicType);
        llvm::Value* result = llvm::UndefValue::get(mHelpers->toLLVMType(intrinsicType));
        result = mBuilder->CreateInsertValue(
            result, llvm::ConstantInt::get(mHelpers->boolTy(), isOk ? 1 : 0), {0},
            isOk ? "ok.tag" : "err.tag");
        return mBuilder->CreateInsertValue(result, bits, {1}, isOk ? "ok.value" : "err.value");
    }
    if (auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get());
        calleeId && (calleeId->name == "is_ok" || calleeId->name == "is_err") &&
        call->args.size() == 1) {
        llvm::Value* result = generateExpr(call->args.front().get());
        llvm::Value* isOk = mBuilder->CreateExtractValue(result, {0}, "result.is_ok");
        return calleeId->name == "is_ok" ? isOk : mBuilder->CreateNot(isOk, "result.is_err");
    }
    if (auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get());
        calleeId && (calleeId->name == "unwrap" || calleeId->name == "unwrap_err") &&
        call->args.size() == 1 && intrinsicType && intrinsicType->kind == TypeKind::Result) {
        llvm::Value* result = generateExpr(call->args.front().get());
        llvm::Value* isOk = mBuilder->CreateExtractValue(result, {0}, "result.tag");
        const bool wantOk = calleeId->name == "unwrap";
        llvm::Value* valid = wantOk ? isOk : mBuilder->CreateNot(isOk, "result.want_err");
        auto* success = llvm::BasicBlock::Create(*mCtx, "result.unwrap", mCurrentFunc);
        auto* failure = llvm::BasicBlock::Create(*mCtx, "result.unwrap.panic", mCurrentFunc);
        mBuilder->CreateCondBr(valid, success, failure);
        mBuilder->SetInsertPoint(failure);
        auto panic =
            mModule->getOrInsertFunction("rt_panic_cstr", mHelpers->voidTy(), mHelpers->ptrTy());
        auto* message = mBuilder->CreateGlobalString(
            wantOk ? "called unwrap on Err" : "called unwrap_err on Ok", "result.unwrap.message");
        mBuilder->CreateCall(panic, {message});
        mBuilder->CreateUnreachable();
        mBuilder->SetInsertPoint(success);
        llvm::Value* bits = mBuilder->CreateExtractValue(result, {1}, "result.payload");
        return unpackResultPayload(bits, intrinsicType->typeArgs[wantOk ? 0 : 1]);
    }
    if (auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get());
        calleeId && calleeId->name == "panic" && call->args.size() == 1) {
        llvm::Value* message = generateExpr(call->args.front().get());
        auto panic =
            mModule->getOrInsertFunction("rt_panic_cstr", mHelpers->voidTy(), mHelpers->ptrTy());
        mBuilder->CreateCall(panic, {coerceCallArgument(message, mHelpers->ptrTy())});
        mBuilder->CreateUnreachable();
        return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
    }
    if (auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get());
        calleeId && calleeId->name == "slice" && call->args.size() == 3) {
        auto* source = generateExpr(call->args[0].get());
        auto* start = coerceCallArgument(generateExpr(call->args[1].get()), mHelpers->i32Ty());
        auto* end = coerceCallArgument(generateExpr(call->args[2].get()), mHelpers->i32Ty());
        // Semantic analysis guarantees a borrowed local array. Its extent is
        // recovered from that binding. The structured path uses the name map
        // (mLocalTypes/mLocals); the canonical CFG path uses LocalId-indexed
        // tables (mCanonicalLocalTypes/mCanonicalLocals).
        uint64_t length = 0;
        llvm::AllocaInst* sourceAlloca = nullptr;
        if (auto* b = dynamic_cast<BorrowExpr*>(call->args[0].get())) {
            if (auto* id = dynamic_cast<IdentifierExpr*>(b->operand.get())) {
                auto structuredType = mLocalTypes.find(id->name);
                if (structuredType != mLocalTypes.end() && structuredType->second)
                    length = structuredType->second->arrayLength;
                else if (!id->local.empty() && id->local.value < mCanonicalLocalTypes.size() &&
                         mCanonicalLocalTypes[id->local.value])
                    length = mCanonicalLocalTypes[id->local.value]->arrayLength;
                auto structuredLocal = mLocals.find(id->name);
                if (structuredLocal != mLocals.end())
                    sourceAlloca = structuredLocal->second;
                else if (!id->local.empty() && id->local.value < mCanonicalLocals.size())
                    sourceAlloca = mCanonicalLocals[id->local.value];
            }
        }
        auto* checkedStart = mBuilder->CreateCall(
            mModule->getOrInsertFunction("rt_array_index_or_abort", mHelpers->i32Ty(),
                                         mHelpers->i32Ty(), mHelpers->sizeTy()),
            {start, llvm::ConstantInt::get(mHelpers->sizeTy(), length + 1)}, "slice.start");
        auto* checkedEnd = mBuilder->CreateCall(
            mModule->getOrInsertFunction("rt_array_index_or_abort", mHelpers->i32Ty(),
                                         mHelpers->i32Ty(), mHelpers->sizeTy()),
            {end, llvm::ConstantInt::get(mHelpers->sizeTy(), length + 1)}, "slice.end");
        auto* valid = mBuilder->CreateICmpSLE(checkedStart, checkedEnd, "slice.order");
        auto* ok = llvm::BasicBlock::Create(*mCtx, "slice.ok", mCurrentFunc);
        auto* bad = llvm::BasicBlock::Create(*mCtx, "slice.bad", mCurrentFunc);
        mBuilder->CreateCondBr(valid, ok, bad);
        mBuilder->SetInsertPoint(bad);
        mBuilder->CreateCall(mModule->getOrInsertFunction("abort", mHelpers->voidTy()));
        mBuilder->CreateUnreachable();
        mBuilder->SetInsertPoint(ok);
        llvm::Value* data = source;
        if (sourceAlloca)
            data = mBuilder->CreateInBoundsGEP(
                sourceAlloca->getAllocatedType(), sourceAlloca,
                {llvm::ConstantInt::get(mHelpers->i32Ty(), 0), checkedStart}, "slice.data");
        auto* sliceTy = llvm::StructType::get(*mCtx, {mHelpers->ptrTy(), mHelpers->sizeTy()});
        llvm::Value* value = llvm::UndefValue::get(sliceTy);
        value = mBuilder->CreateInsertValue(value, data, {0});
        return mBuilder->CreateInsertValue(
            value,
            mBuilder->CreateSExtOrTrunc(mBuilder->CreateSub(checkedEnd, checkedStart),
                                        mHelpers->sizeTy()),
            {1});
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
            return mBuilder->CreateGEP(global->getValueType(), global,
                                       {llvm::ConstantInt::get(mHelpers->i32Ty(), 0),
                                        llvm::ConstantInt::get(mHelpers->i32Ty(), 0)},
                                       "ctstrptr");
        }
    }
    // Device built-ins use the runtime boundary on the host. Kernel bodies
    // retain direct element operations so that the same LLVM function can
    // be cloned to PTX; the CPU simulator invokes that host form directly.
    if (auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get())) {
        if (calleeId->name == "gpu_alloc_i32" && call->args.size() == 1) {
            auto alloc = mModule->getOrInsertFunction("rt_gpu_alloc_i32", mHelpers->voidTy(),
                                                      mHelpers->sizeTy(), mHelpers->ptrTy());
            auto* count = coerceCallArgument(generateExpr(call->args[0].get()), mHelpers->sizeTy());
            auto* output = createEntryBlockAlloca(mCurrentFunc, mHelpers->deviceBufferTy(),
                                                  "devicealloc.output");
            mBuilder->CreateCall(alloc, {count, output});
            return mBuilder->CreateLoad(mHelpers->deviceBufferTy(), output, "devicealloc");
        }
        if (calleeId->name == "gpu_free" && call->args.size() == 1) {
            auto free = mModule->getOrInsertFunction("rt_gpu_free", mHelpers->voidTy(),
                                                     mHelpers->ptrTy(), mHelpers->sizeTy());
            auto* buffer = generateDeviceBufferValue(call->args[0].get());
            mBuilder->CreateCall(free,
                                 {mBuilder->CreateExtractValue(buffer, {0}, "device.data"),
                                  mBuilder->CreateExtractValue(buffer, {1}, "device.length")});
            return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
        }
        if (calleeId->name == "gpu_load_i32" && call->args.size() == 2) {
            auto* bufferValue = generateDeviceBufferValue(call->args[0].get());
            auto* buffer = mBuilder->CreateExtractValue(bufferValue, {0}, "device.data");
            auto* index = coerceCallArgument(generateExpr(call->args[1].get()), mHelpers->i32Ty());
            if (mCurrentFunctionIsKernel) {
                auto* length = mBuilder->CreateExtractValue(bufferValue, {1}, "device.length");
                index = emitDeviceBufferIndexCheck(index, length);
                auto* element = mBuilder->CreateGEP(mHelpers->i32Ty(), buffer, index, "deviceelem");
                return mBuilder->CreateLoad(mHelpers->i32Ty(), element, "deviceload");
            }
            auto load = mModule->getOrInsertFunction("rt_gpu_load_i32", mHelpers->i32Ty(),
                                                     mHelpers->ptrTy(), mHelpers->sizeTy(),
                                                     mHelpers->i32Ty());
            auto* length = mBuilder->CreateExtractValue(bufferValue, {1}, "device.length");
            return mBuilder->CreateCall(load, {buffer, length, index}, "deviceload");
        }
        if (calleeId->name == "gpu_store_i32" && call->args.size() == 3) {
            auto* bufferValue = generateDeviceBufferValue(call->args[0].get());
            auto* buffer = mBuilder->CreateExtractValue(bufferValue, {0}, "device.data");
            auto* index = coerceCallArgument(generateExpr(call->args[1].get()), mHelpers->i32Ty());
            auto* value = coerceCallArgument(generateExpr(call->args[2].get()), mHelpers->i32Ty());
            if (mCurrentFunctionIsKernel) {
                auto* length = mBuilder->CreateExtractValue(bufferValue, {1}, "device.length");
                index = emitDeviceBufferIndexCheck(index, length);
                auto* element = mBuilder->CreateGEP(mHelpers->i32Ty(), buffer, index, "deviceelem");
                mBuilder->CreateStore(value, element);
            } else {
                auto store = mModule->getOrInsertFunction("rt_gpu_store_i32", mHelpers->voidTy(),
                                                          mHelpers->ptrTy(), mHelpers->sizeTy(),
                                                          mHelpers->i32Ty(), mHelpers->i32Ty());
                auto* length = mBuilder->CreateExtractValue(bufferValue, {1}, "device.length");
                mBuilder->CreateCall(store, {buffer, length, index, value});
            }
            return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
        }
        if (calleeId->name == "gpu_copy_from_host_i32" && call->args.size() == 3) {
            auto* destination = generateDeviceBufferValue(call->args[0].get());
            auto* source = generateHostRawPointer(call->args[1].get());
            auto* count = coerceCallArgument(generateExpr(call->args[2].get()), mHelpers->i32Ty());
            auto copy = mModule->getOrInsertFunction("rt_gpu_copy_from_host_i32", mHelpers->i32Ty(),
                                                     mHelpers->ptrTy(), mHelpers->sizeTy(),
                                                     mHelpers->ptrTy(), mHelpers->i32Ty());
            auto* copied = mBuilder->CreateCall(
                copy,
                {mBuilder->CreateExtractValue(destination, {0}, "device.data"),
                 mBuilder->CreateExtractValue(destination, {1}, "device.length"), source, count},
                "gpu.uploaded");
            emitGpuOperationFailureCheck(copied, mCurrentFunc);
            return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
        }
        if (calleeId->name == "gpu_copy_to_host_i32" && call->args.size() == 3) {
            auto* destination = generateHostRawPointer(call->args[0].get());
            auto* source = generateDeviceBufferValue(call->args[1].get());
            auto* count = coerceCallArgument(generateExpr(call->args[2].get()), mHelpers->i32Ty());
            auto copy = mModule->getOrInsertFunction("rt_gpu_copy_to_host_i32", mHelpers->i32Ty(),
                                                     mHelpers->ptrTy(), mHelpers->ptrTy(),
                                                     mHelpers->sizeTy(), mHelpers->i32Ty());
            auto* copied = mBuilder->CreateCall(
                copy,
                {destination, mBuilder->CreateExtractValue(source, {0}, "device.data"),
                 mBuilder->CreateExtractValue(source, {1}, "device.length"), count},
                "gpu.downloaded");
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
                const TypePtr argumentType = resolveType(arg->type);
                if (isUnsignedIntegerType(argumentType) && argVal->getType()->isIntegerTy(32)) {
                    auto print = mModule->getOrInsertFunction("rt_print_u32", mHelpers->voidTy(),
                                                              mHelpers->i32Ty());
                    mBuilder->CreateCall(print, {argVal});
                } else if (argVal->getType()->isIntegerTy(32)) {
                    auto print = mModule->getOrInsertFunction("rt_print_i32", mHelpers->voidTy(),
                                                              mHelpers->i32Ty());
                    mBuilder->CreateCall(print, {argVal});
                } else {
                    auto print = mModule->getOrInsertFunction("rt_print_cstr", mHelpers->voidTy(),
                                                              mHelpers->ptrTy());
                    mBuilder->CreateCall(print, {argVal});
                }
            }
            return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
        }
    }

    // Global calls are resolved only through the verified declaration
    // table. Source names never act as a backend lookup fallback.
    if (dynamic_cast<IdentifierExpr*>(call->callee.get())) {
        llvm::Function* callee =
            call->calleeRef.complete() ? resolveFunction(call->calleeRef) : nullptr;
        if (callee) {

            std::vector<llvm::Value*> args;
            for (size_t i = 0; i < call->args.size(); ++i) {
                auto* value = generateExpr(call->args[i].get());
                if (i < callee->getFunctionType()->getNumParams())
                    value = coerceCallArgument(value, callee->getFunctionType()->getParamType(i));
                args.push_back(value);
            }

            auto* emitted = mBuilder->CreateCall(
                callee, args, callee->getReturnType()->isVoidTy() ? "" : "calltmp");
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
    TypePtr callableType = call->callee ? resolveType(call->callee->type) : nullptr;
    if (auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get())) {
        auto localType = mLocalTypes.find(calleeId->name);
        if ((!callableType || (callableType->kind != TypeKind::Function &&
                               callableType->kind != TypeKind::Closure)) &&
            localType != mLocalTypes.end())
            callableType = localType->second;
        // The canonical CFG path binds locals through LocalId, not the
        // structured-path mLocals name map. Fall back to the canonical
        // local table when the structured map has no entry.
        if ((!callableType || (callableType->kind != TypeKind::Function &&
                               callableType->kind != TypeKind::Closure)) &&
            !calleeId->local.empty() && calleeId->local.value < mCanonicalLocalTypes.size())
            callableType = mCanonicalLocalTypes[calleeId->local.value];
        // Reject only when neither the structured name map, the canonical
        // local table, nor a verified declaration can resolve the callee.
        const bool hasStructuredLocal = mLocals.find(calleeId->name) != mLocals.end();
        const bool hasCanonicalLocal = !calleeId->local.empty() &&
                                       calleeId->local.value < mCanonicalLocals.size() &&
                                       mCanonicalLocals[calleeId->local.value];
        const bool hasDeclaration = call->calleeRef.complete() || calleeId->declaration.complete();
        if (!hasStructuredLocal && !hasCanonicalLocal && !hasDeclaration) {
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
        auto* functionType = llvm::FunctionType::get(returnType, parameterTypes, false);
        return mBuilder->CreateCall(functionType, functionPointer, arguments,
                                    returnType->isVoidTy() ? "" : "indirect.call");
    }
    if (callableType && callableType->kind == TypeKind::Closure) {
        llvm::Value* closureValue = generateExpr(call->callee.get());
        llvm::Type* closureType = mHelpers->toLLVMType(callableType);
        auto* closureStorage = createEntryBlockAlloca(mCurrentFunc, closureType, "closure.call");
        mBuilder->CreateStore(closureValue, closureStorage);
        llvm::Value* codePointer = mBuilder->CreateLoad(
            mHelpers->ptrTy(), mBuilder->CreateStructGEP(closureType, closureStorage, 0),
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
        auto* functionType = llvm::FunctionType::get(returnType, parameterTypes, false);
        return mBuilder->CreateCall(functionType, codePointer, arguments,
                                    returnType->isVoidTy() ? "" : "closure.call");
    }
    return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
}

llvm::Value* CodeGenerator::generateTry(TryExpr* propagation) {
    llvm::Value* result = generateExpr(propagation->operand.get());
    llvm::Value* isOk = mBuilder->CreateExtractValue(result, {0}, "try.is_ok");
    auto* success = llvm::BasicBlock::Create(*mCtx, "try.success", mCurrentFunc);
    auto* failure = llvm::BasicBlock::Create(*mCtx, "try.error", mCurrentFunc);
    mBuilder->CreateCondBr(isOk, success, failure);
    mBuilder->SetInsertPoint(failure);
    llvm::Value* errorBits = mBuilder->CreateExtractValue(result, {1}, "try.error.bits");
    llvm::Value* errorValue = unpackResultPayload(errorBits, resolveType(propagation->errorType));
    if (!propagation->errorConversion.empty()) {
        llvm::Function* conversion = resolveFunction(propagation->errorConversion);
        if (!conversion) {
            error("error propagation references unknown From conversion '" +
                  propagation->errorConversion.symbol.value + "'");
        } else {
            errorValue = mBuilder->CreateCall(
                conversion,
                {coerceCallArgument(errorValue, conversion->getFunctionType()->getParamType(0))},
                "try.converted_error");
        }
    }
    TypePtr propagatedResult =
        resolveType(propagation->propagatedResultType.empty() ? propagation->resultType
                                                              : propagation->propagatedResultType);
    TypePtr propagatedError =
        resolveType(propagation->propagatedErrorType.empty() ? propagation->errorType
                                                             : propagation->propagatedErrorType);
    llvm::Value* propagatedBits = packResultPayload(errorValue, propagatedError, propagatedResult);
    llvm::Value* propagatedValue = llvm::UndefValue::get(mHelpers->toLLVMType(propagatedResult));
    propagatedValue = mBuilder->CreateInsertValue(
        propagatedValue, llvm::ConstantInt::get(mHelpers->boolTy(), 0), {0}, "try.error.tag");
    propagatedValue =
        mBuilder->CreateInsertValue(propagatedValue, propagatedBits, {1}, "try.error.value");
    for (const auto& cleanup : propagation->cleanups)
        emitCleanup(cleanup.place, cleanup.action);
    mBuilder->CreateRet(propagatedValue);
    mBuilder->SetInsertPoint(success);
    llvm::Value* bits = mBuilder->CreateExtractValue(result, {1}, "try.value");
    return unpackResultPayload(bits, resolveType(propagation->valueType));
}
