#include "CGHelpers.h"
#include "../core/TypeLayout.h"
#include "../moonir/MoonIRTypes.h"
#include "../runtime/RuntimeFragmentABI.h"

#include <algorithm>
#include <vector>

CGHelpers::CGHelpers(llvm::LLVMContext& ctx) : mCtx(ctx) {}

llvm::Type* CGHelpers::toLLVMType(const TypePtr& type) const {
    if (!type) return voidTy();

    switch (type->kind) {
        case TypeKind::RuntimeFragmentRef:
            return ptrTy();
        case TypeKind::I8:    return llvm::Type::getInt8Ty(mCtx);
        case TypeKind::I16:   return llvm::Type::getInt16Ty(mCtx);
        case TypeKind::I32:   return i32Ty();
        case TypeKind::I64:   return i64Ty();
        case TypeKind::U8:    return llvm::Type::getInt8Ty(mCtx);
        case TypeKind::U16:   return llvm::Type::getInt16Ty(mCtx);
        case TypeKind::U32:   return i32Ty();
        case TypeKind::U64:   return i64Ty();
        case TypeKind::USize:
        case TypeKind::ISize: return sizeTy();
        case TypeKind::F32:   return f32Ty();
        case TypeKind::F64:   return f64Ty();
        case TypeKind::Bool:  return boolTy();
        case TypeKind::String:
        case TypeKind::CStr:
        case TypeKind::RawPointer:
        case TypeKind::Metadata:
        case TypeKind::MetadataView:
        case TypeKind::SymbolSet:
        case TypeKind::DeclarationView:
        case TypeKind::DeclarationRef: return ptrTy();
        case TypeKind::Iterator: return ptrTy();
        case TypeKind::DeviceBuffer: return deviceBufferTy();
        case TypeKind::Result: {
            const uint64_t valueSize = type->typeArgs.size() > 0
                ? luna::layout::valueSize(type->typeArgs[0]) : 0;
            const uint64_t errorSize = type->typeArgs.size() > 1
                ? luna::layout::valueSize(type->typeArgs[1]) : 0;
            const uint64_t words =
                std::max<uint64_t>(1, (std::max(valueSize, errorSize) + 7) / 8);
            return llvm::StructType::get(
                mCtx, {boolTy(), llvm::ArrayType::get(i64Ty(), words)});
        }
        case TypeKind::Enum: {
            const uint64_t words =
                std::max<uint64_t>(
                    1, luna::layout::enumPayloadSize(type) / 8);
            return llvm::StructType::get(
                mCtx, {i32Ty(), llvm::ArrayType::get(i64Ty(), words)});
        }
        case TypeKind::Array:
            return llvm::ArrayType::get(toLLVMType(type->inner), type->arrayLength);
        case TypeKind::Slice:
            return llvm::StructType::get(mCtx, {ptrTy(), sizeTy()});
        case TypeKind::Event: return i32Ty();
        case TypeKind::Unit:
        case TypeKind::Never: return voidTy();
        case TypeKind::Reference:
            return ptrTy();
        case TypeKind::Struct:
            return ptrTy();
        case TypeKind::Record: {
            std::vector<llvm::Type*> fields;
            fields.reserve(type->fields.size());
            for (const auto& field : type->fields)
                fields.push_back(toLLVMType(field.type));
            return llvm::StructType::get(mCtx, fields);
        }
        case TypeKind::Function:
            return ptrTy(); // function pointers are opaque ptrs
        case TypeKind::Closure: {
            std::vector<llvm::Type*> fields;
            fields.reserve(1 + type->capturedFields.size());
            fields.push_back(ptrTy());
            for (const auto& field : type->capturedFields)
                fields.push_back(toLLVMType(field.type));
            return llvm::StructType::get(mCtx, fields);
        }
        default:
            return i32Ty(); // fallback
    }
}

llvm::CallInst* CGHelpers::emitRuntimeFragmentRefDrop(
    llvm::IRBuilder<>& builder, llvm::Module& module,
    llvm::Value* carrierCell) const {
    auto drop = module.getOrInsertFunction(
        "luna_runtime_fragment_ref_drop_v1", voidTy(), ptrTy());
    return builder.CreateCall(drop, {carrierCell});
}

llvm::Value* CGHelpers::emitRuntimeFragmentRefTake(
    llvm::IRBuilder<>& builder, llvm::Value* carrierCell) const {
    auto* value = builder.CreateLoad(ptrTy(), carrierCell, "ref.take");
    builder.CreateStore(
        llvm::ConstantPointerNull::get(
            llvm::cast<llvm::PointerType>(ptrTy())), carrierCell);
    return value;
}

llvm::CallInst* CGHelpers::emitRuntimeFragmentRefBorrowCheck(
    llvm::IRBuilder<>& builder, llvm::Module& module,
    llvm::Value* reference, const moon::DeclarationRef& target) const {
    if (!target.complete() || !reference ||
        !reference->getType()->isPointerTy()) return nullptr;
    auto check = module.getOrInsertFunction(
        "luna_runtime_fragment_ref_check_v1", i32Ty(),
        ptrTy(), ptrTy(), ptrTy());
    return builder.CreateCall(check, {
        reference,
        builder.CreateGlobalString(target.symbol.value, "ref.target.slot"),
        builder.CreateGlobalString(target.contract.value, "ref.target.contract"),
    });
}

llvm::CallInst* CGHelpers::emitRuntimeFragmentRefOwnedTransfer(
    llvm::IRBuilder<>& builder, llvm::Module& module,
    llvm::Value* sourceCell, llvm::Value* destinationCell,
    const moon::DeclarationRef& target) const {
    if (!target.complete() || !sourceCell || !destinationCell ||
        sourceCell == destinationCell ||
        !sourceCell->getType()->isPointerTy() ||
        !destinationCell->getType()->isPointerTy()) return nullptr;
    auto transfer = module.getOrInsertFunction(
        "luna_runtime_fragment_ref_transfer_v1", i32Ty(),
        ptrTy(), ptrTy(), ptrTy(), ptrTy());
    return builder.CreateCall(transfer, {
        sourceCell,
        builder.CreateGlobalString(target.symbol.value, "ref.target.slot"),
        builder.CreateGlobalString(target.contract.value, "ref.target.contract"),
        destinationCell,
    });
}

namespace {

bool validRefIngressGate(llvm::IRBuilder<>& builder,
                         llvm::Module& module,
                         llvm::BasicBlock* bodyEntry,
                         llvm::Type* statusType) {
    auto* entry = builder.GetInsertBlock();
    auto* wrapper = entry ? entry->getParent() : nullptr;
    return wrapper && wrapper->getParent() == &module &&
        !entry->getTerminator() && bodyEntry &&
        bodyEntry != entry && bodyEntry->getParent() == wrapper &&
        bodyEntry->empty() && wrapper->getReturnType() == statusType;
}

void finishRefIngressGate(llvm::IRBuilder<>& builder,
                          llvm::CallInst* status,
                          llvm::BasicBlock* bodyEntry) {
    auto* wrapper = builder.GetInsertBlock()->getParent();
    auto* failure = llvm::BasicBlock::Create(
        builder.getContext(), "ref.ingress.failed", wrapper);
    auto* accepted = builder.CreateICmpEQ(
        status,
        llvm::ConstantInt::get(
            status->getType(), LUNA_RUNTIME_FRAGMENT_REF_SUCCESS_V1),
        "ref.ingress.accepted");
    builder.CreateCondBr(accepted, bodyEntry, failure);
    builder.SetInsertPoint(failure);
    builder.CreateRet(status);
    builder.SetInsertPoint(bodyEntry);
}

} // namespace

llvm::CallInst* CGHelpers::emitRuntimeFragmentRefBorrowIngressGate(
    llvm::IRBuilder<>& builder, llvm::Module& module,
    llvm::Value* reference, const moon::DeclarationRef& target,
    llvm::BasicBlock* bodyEntry) const {
    if (!validRefIngressGate(builder, module, bodyEntry, i32Ty())) return nullptr;
    auto* status = emitRuntimeFragmentRefBorrowCheck(
        builder, module, reference, target);
    if (status) finishRefIngressGate(builder, status, bodyEntry);
    return status;
}

llvm::CallInst* CGHelpers::emitRuntimeFragmentRefOwnedIngressGate(
    llvm::IRBuilder<>& builder, llvm::Module& module,
    llvm::Value* sourceCell, llvm::Value* destinationCell,
    const moon::DeclarationRef& target,
    llvm::BasicBlock* bodyEntry) const {
    if (!validRefIngressGate(builder, module, bodyEntry, i32Ty())) return nullptr;
    auto* status = emitRuntimeFragmentRefOwnedTransfer(
        builder, module, sourceCell, destinationCell, target);
    if (status) finishRefIngressGate(builder, status, bodyEntry);
    return status;
}

llvm::Function* CGHelpers::emitRuntimeFragmentRefUnitIngressWrapper(
    llvm::Module& module, llvm::Function& body,
    const moon::DeclarationRef& target,
    RuntimeFragmentRefIngressMode mode,
    bool hasFragmentContext,
    const std::string& name) const {
    if ((mode != RuntimeFragmentRefIngressMode::Borrowed &&
         mode != RuntimeFragmentRefIngressMode::Owned) ||
        !target.complete() || name.empty() || module.getFunction(name) ||
        body.getParent() != &module || body.isVarArg() ||
        !body.getReturnType()->isVoidTy() ||
        body.arg_size() != (hasFragmentContext ? 2u : 1u)) return nullptr;
    for (const auto& argument : body.args()) {
        if (!argument.getType()->isPointerTy()) return nullptr;
    }

    std::vector<llvm::Type*> parameters(body.arg_size(), ptrTy());
    auto* wrapper = llvm::Function::Create(
        llvm::FunctionType::get(i32Ty(), parameters, false),
        llvm::Function::InternalLinkage, name, module);
    auto* entry = llvm::BasicBlock::Create(mCtx, "entry", wrapper);
    auto* bodyEntry = llvm::BasicBlock::Create(mCtx, "ref.body", wrapper);
    llvm::IRBuilder<> builder(entry);
    auto* incoming = wrapper->getArg(
        static_cast<unsigned>(wrapper->arg_size() - 1));
    llvm::Value* reference = incoming;
    llvm::CallInst* status = nullptr;
    if (mode == RuntimeFragmentRefIngressMode::Owned) {
        auto* destination = builder.CreateAlloca(ptrTy(), nullptr, "ref.owner");
        builder.CreateStore(
            llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(ptrTy())), destination);
        status = emitRuntimeFragmentRefOwnedIngressGate(
            builder, module, incoming, destination, target, bodyEntry);
        if (status) reference = emitRuntimeFragmentRefTake(builder, destination);
    } else {
        status = emitRuntimeFragmentRefBorrowIngressGate(
            builder, module, incoming, target, bodyEntry);
    }
    if (!status) {
        wrapper->eraseFromParent();
        return nullptr;
    }

    std::vector<llvm::Value*> arguments;
    if (hasFragmentContext) arguments.push_back(wrapper->getArg(0));
    arguments.push_back(reference);
    auto* call = builder.CreateCall(&body, arguments);
    call->setCallingConv(body.getCallingConv());
    builder.CreateRet(llvm::ConstantInt::get(
        i32Ty(), LUNA_RUNTIME_FRAGMENT_REF_SUCCESS_V1));
    return wrapper;
}

uint64_t typeSize(const TypePtr& type) {
    if (!type) return 0;
    switch (type->kind) {
        case TypeKind::I8:
        case TypeKind::U8: return 1;
        case TypeKind::I16:
        case TypeKind::U16: return 2;
        case TypeKind::I32:   return 4;
        case TypeKind::I64:   return 8;
        case TypeKind::U32:   return 4;
        case TypeKind::U64:
        case TypeKind::USize:
        case TypeKind::ISize: return 8;
        case TypeKind::F32:   return 4;
        case TypeKind::F64:   return 8;
        case TypeKind::Bool:  return 1;
        case TypeKind::String:
        case TypeKind::CStr: return 8; // pointer size
        case TypeKind::Reference:
        case TypeKind::RawPointer: return 8;
        case TypeKind::DeviceBuffer: return 16;
        case TypeKind::Metadata:
        case TypeKind::MetadataView:
        case TypeKind::SymbolSet:
        case TypeKind::DeclarationView:
        case TypeKind::DeclarationRef: return 8;
        case TypeKind::Iterator: return 8;
        case TypeKind::Array: return type->arrayLength * typeSize(type->inner);
        case TypeKind::Slice: return 16;
        case TypeKind::Event: return 4;
        case TypeKind::Struct: {
            return luna::layout::productStorageSize(type);
        }
        case TypeKind::Record:
            return luna::layout::valueSize(type);
        case TypeKind::Closure:
            return luna::layout::valueSize(type);
        case TypeKind::Enum:
            return luna::layout::valueSize(type);
        case TypeKind::Result: {
            return luna::layout::valueSize(type);
        }
        case TypeKind::Never: return 0;
        default: return 0;
    }
}

uint64_t typeAlignment(const TypePtr& type) {
    if (!type) return 1;
    switch (type->kind) {
        case TypeKind::I8:
        case TypeKind::U8:
        case TypeKind::Bool: return 1;
        case TypeKind::I16:
        case TypeKind::U16: return 2;
        case TypeKind::I32:
        case TypeKind::U32:
        case TypeKind::F32:
        case TypeKind::Event: return 4;
        case TypeKind::Array: return typeAlignment(type->inner);
        case TypeKind::Struct:
            return luna::layout::productStorageAlignment(type);
        case TypeKind::Record: {
            uint64_t alignment = 1;
            for (const auto& field : type->fields)
                alignment = std::max(alignment, typeAlignment(field.type));
            return alignment;
        }
        case TypeKind::Closure: {
            uint64_t alignment = 8;
            for (const auto& field : type->capturedFields)
                alignment = std::max(alignment, typeAlignment(field.type));
            return alignment;
        }
        case TypeKind::Unit:
        case TypeKind::Never: return 1;
        case TypeKind::Result:
        case TypeKind::Enum: return 8;
        default: return 8;
    }
}
