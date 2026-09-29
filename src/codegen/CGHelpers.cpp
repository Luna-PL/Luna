#include "CGHelpers.h"
#include "../core/TypeLayout.h"
#include "../core/TypeRelations.h"
#include "../moonir/FragmentContextEffects.h"
#include "../moonir/Verifier.h"
#include "../runtime/RuntimeFragmentABI.h"
#include "../runtime/RuntimeFragmentCompilerBridge.h"

#include <algorithm>
#include <optional>
#include <vector>

CGHelpers::CGHelpers(llvm::LLVMContext& ctx) : mCtx(ctx) {}

static_assert(LUNA_COMPILER_FRAGMENT_OVERRIDE_SUCCESS == 0);

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

llvm::CallInst* CGHelpers::emitRuntimeFragmentRefContextOverride(
    llvm::IRBuilder<>& builder, llvm::Module& module,
    llvm::Value* parentContext, llvm::Value* borrowedReference,
    const moon::DeclarationRef& target, llvm::Value* outputCell) const {
    if (!target.complete() || !parentContext || !borrowedReference ||
        !outputCell || !parentContext->getType()->isPointerTy() ||
        !borrowedReference->getType()->isPointerTy() ||
        !outputCell->getType()->isPointerTy())
        return nullptr;
    auto override = module.getOrInsertFunction(
        "luna_compiler_fragment_context_override_from_ref",
        i32Ty(), ptrTy(), ptrTy(), ptrTy(), ptrTy(), ptrTy());
    return builder.CreateCall(override, {
        parentContext, borrowedReference,
        builder.CreateGlobalString(target.symbol.value, "ref.apply.slot"),
        builder.CreateGlobalString(target.contract.value, "ref.apply.contract"),
        outputCell,
    }, "ref.apply.context.status");
}

llvm::CallInst* CGHelpers::emitRuntimeFragmentContextDrop(
    llvm::IRBuilder<>& builder, llvm::Module& module,
    llvm::Value* contextCell) const {
    if (!contextCell || !contextCell->getType()->isPointerTy())
        return nullptr;
    auto drop = module.getOrInsertFunction(
        "luna_compiler_fragment_context_drop", voidTy(), ptrTy());
    return builder.CreateCall(drop, {contextCell});
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

namespace {

struct RefIngressPlan {
    moon::DeclarationRef target;
    bool owned = false;
    bool hasFragmentContext = false;
};

std::optional<RefIngressPlan> resolveRefIngressPlan(
    const moon::Module& module, const moon::FunctionDecl& function,
    bool returnsOwnedRef) {
    if (!module.typeTableSealed || function.isExtern || function.isKernel ||
        function.isSelector || !function.typeParams.empty() ||
        function.generatedSymbolName.empty() || !function.linkName.empty() ||
        function.params.size() != 1 || function.body ||
        !function.controlFlow || !function.controlFlow->sealed ||
        !function.controlFlow->runtimeRefApplies.empty())
        return std::nullopt;
    const bool member = std::any_of(
        module.declarations.begin(), module.declarations.end(),
        [&function](const auto& declaration) {
            return declaration.get() == &function;
        });
    if (!member) return std::nullopt;
    const auto& parameter = function.params.front();
    const auto target = module.resolveRuntimeFragmentRefTarget(parameter.type);
    const auto* refType = module.findType(parameter.type);
    const auto* returnType = module.findType(function.returnType);
    if (!target || !refType || !returnType ||
        (returnsOwnedRef
            ? returnType->kind != TypeKind::RuntimeFragmentRef ||
              returnType->id != refType->id ||
              function.returnUsage != luna::ownership::Usage::Affine ||
              function.requiresFragmentContext
            : returnType->kind != TypeKind::Unit ||
              function.returnUsage != luna::ownership::Usage::Copy) ||
        function.returnsLinear ||
        refType->sysmeta.resource.management !=
            luna::sysmeta::ResourceManagement::Unique ||
        refType->sysmeta.resource.releaseDomain !=
            luna::sysmeta::ReleaseDomain::Executable ||
        refType->sysmeta.resource.lifetime !=
            luna::sysmeta::ResourceLifetime::Lexical ||
        refType->sysmeta.resource.relation !=
            luna::ownership::Relation::Owned ||
        refType->sysmeta.resource.usage !=
            luna::ownership::Usage::Affine ||
        !refType->sysmeta.resource.cleanupRequired ||
        !refType->sysmeta.resource.needsDrop ||
        refType->sysmeta.resource.cleanup !=
            luna::ownership::CleanupAction::Drop ||
        !refType->sysmeta.capability.hostOnly ||
        parameter.isLinear ||
        (parameter.relation == luna::ownership::Relation::SharedBorrow
            ? parameter.usage != luna::ownership::Usage::Copy
            : parameter.relation != luna::ownership::Relation::Owned ||
              parameter.usage != luna::ownership::Usage::Affine))
        return std::nullopt;

    const auto* record = module.findDeclarationById(function.declarationId);
    const auto* callable = record ? module.findType(record->type) : nullptr;
    moon::TypeMaterializer materializer(module);
    const TypePtr restoredCallable = callable
        ? materializer.materialize(record->type) : nullptr;
    if (!record || record->kind != moon::DeclarationKind::Function ||
        record->id != function.declarationId ||
        record->symbolId != luna::identity::symbolIdFromCanonical(record->id) ||
        record->symbolId != function.symbolId ||
        record->contractId != function.contractId ||
        record->sysmeta.identity.symbol != record->symbolId ||
        record->sysmeta.identity.contract != record->contractId ||
        record->linkageName != function.generatedSymbolName ||
        record->canonicalContract != moon::canonicalContract(*record) ||
        record->contractId != luna::identity::contractIdFromCanonical(
            record->canonicalContract) ||
        !callable || callable->kind != TypeKind::Function ||
        !restoredCallable ||
        luna::types::typeId(restoredCallable) != callable->id ||
        luna::types::canonicalType(restoredCallable) !=
            callable->canonicalType ||
        callable->parameterTypeIds.size() != 1 ||
        callable->parameterTypeIds.front() != parameter.type ||
        callable->returnTypeId != function.returnType ||
        callable->returnContract != luna::ownership::Contract{
            luna::ownership::Relation::Owned, function.returnUsage} ||
        callable->parameterContracts.size() != 1 ||
        callable->parameterContracts.front() !=
            luna::ownership::Contract{parameter.relation, parameter.usage} ||
        record->sysmeta.resource.parameters.size() != 1 ||
        record->sysmeta.resource.parameters.front() !=
            callable->parameterContracts.front() ||
        record->sysmeta.resource.result != callable->returnContract)
        return std::nullopt;

    const auto& graph = *function.controlFlow;
    const auto* root = graph.findRegion(graph.rootRegion);
    if (!root || root->kind != moon::RegionKind::Function)
        return std::nullopt;
    const moon::LocalRecord* parameterLocal = nullptr;
    for (const auto& local : graph.locals) {
        if (local.kind != moon::LocalKind::Parameter) continue;
        if (parameterLocal) return std::nullopt;
        parameterLocal = &local;
    }
    if (!parameterLocal || parameterLocal->scope != graph.rootScope ||
        parameterLocal->name != parameter.name ||
        parameterLocal->type != parameter.type ||
        parameterLocal->relation != parameter.relation ||
        parameterLocal->usage != parameter.usage)
        return std::nullopt;
    size_t matchingCleanups = 0;
    for (const auto& cleanup : graph.cleanups) {
        if (cleanup.place.root != parameterLocal->id) continue;
        ++matchingCleanups;
        if (!cleanup.place.projections.empty() || cleanup.guard ||
            cleanup.scope != graph.rootScope ||
            cleanup.type != parameter.type ||
            cleanup.kind != moon::CleanupKind::Value ||
            cleanup.action != luna::ownership::CleanupAction::Drop)
            return std::nullopt;
    }
    if (matchingCleanups !=
        (parameter.relation == luna::ownership::Relation::Owned ? 1u : 0u))
        return std::nullopt;
    if (returnsOwnedRef) {
        if (parameter.relation != luna::ownership::Relation::Owned)
            return std::nullopt;
        size_t directReturns = 0;
        for (const auto& block : graph.blocks) {
            // No callback may mutate the host's output cell after the
            // preflight check but before the returned owner is installed.
            if (!block.operations.empty()) return std::nullopt;
            if (block.terminator.kind != moon::TerminatorKind::Return) {
                if (block.terminator.kind != moon::TerminatorKind::Jump &&
                    block.terminator.kind != moon::TerminatorKind::Unreachable)
                    return std::nullopt;
                continue;
            }
            const auto* identifier = dynamic_cast<const moon::IdentifierExpr*>(
                block.terminator.operand.get());
            if (!identifier || identifier->local != parameterLocal->id)
                return std::nullopt;
            ++directReturns;
        }
        if (!directReturns) return std::nullopt;
    }
    moon::Verifier verifier;
    if (!verifier.verify(graph, module)) return std::nullopt;
    const auto effects = moon::computeFragmentContextEffects(module);
    const auto effect = effects.find(moon::fragmentContextEffectKey(
        {function.symbolId, function.contractId}));
    if (function.requiresFragmentContext !=
        (effect != effects.end() && effect->second))
        return std::nullopt;
    return RefIngressPlan{
        *target,
        parameter.relation == luna::ownership::Relation::Owned,
        function.requiresFragmentContext};
}

} // namespace

llvm::Function* CGHelpers::emitRuntimeFragmentRefUnitIngressWrapper(
    llvm::Module& module, llvm::Function& body,
    const moon::Module& sourceModule,
    const moon::FunctionDecl& sourceFunction,
    const std::string& name) const {
    const auto plan = resolveRefIngressPlan(
        sourceModule, sourceFunction, false);
    if (!plan || name.empty() || module.getFunction(name) ||
        body.getParent() != &module || body.isVarArg() ||
        body.getName() != sourceFunction.generatedSymbolName ||
        !body.getReturnType()->isVoidTy() ||
        body.arg_size() != (plan->hasFragmentContext ? 2u : 1u))
        return nullptr;
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
    if (plan->owned) {
        auto* destination = builder.CreateAlloca(ptrTy(), nullptr, "ref.owner");
        builder.CreateStore(
            llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(ptrTy())), destination);
        status = emitRuntimeFragmentRefOwnedIngressGate(
            builder, module, incoming, destination, plan->target, bodyEntry);
        if (status) reference = emitRuntimeFragmentRefTake(builder, destination);
    } else {
        status = emitRuntimeFragmentRefBorrowIngressGate(
            builder, module, incoming, plan->target, bodyEntry);
    }
    if (!status) {
        wrapper->eraseFromParent();
        return nullptr;
    }

    std::vector<llvm::Value*> arguments;
    if (plan->hasFragmentContext) arguments.push_back(wrapper->getArg(0));
    arguments.push_back(reference);
    auto* call = builder.CreateCall(&body, arguments);
    call->setCallingConv(body.getCallingConv());
    builder.CreateRet(llvm::ConstantInt::get(
        i32Ty(), LUNA_RUNTIME_FRAGMENT_REF_SUCCESS_V1));
    return wrapper;
}

llvm::Function* CGHelpers::emitRuntimeFragmentRefOwnedReturnWrapper(
    llvm::Module& module, llvm::Function& body,
    const moon::Module& sourceModule,
    const moon::FunctionDecl& sourceFunction,
    const std::string& name) const {
    const auto plan = resolveRefIngressPlan(
        sourceModule, sourceFunction, true);
    if (!plan || !plan->owned || plan->hasFragmentContext || name.empty() ||
        module.getFunction(name) || body.getParent() != &module ||
        !body.hasInternalLinkage() || body.isVarArg() || body.getName() !=
            sourceFunction.generatedSymbolName ||
        body.getReturnType() != ptrTy() || body.arg_size() != 1 ||
        body.getArg(0)->getType() != ptrTy())
        return nullptr;

    auto* wrapper = llvm::Function::Create(
        llvm::FunctionType::get(i32Ty(), {ptrTy(), ptrTy()}, false),
        llvm::Function::InternalLinkage, name, module);
    auto* entry = llvm::BasicBlock::Create(mCtx, "entry", wrapper);
    auto* outputCheck = llvm::BasicBlock::Create(
        mCtx, "ref.output.check", wrapper);
    auto* ingress = llvm::BasicBlock::Create(
        mCtx, "ref.ingress", wrapper);
    auto* bodyEntry = llvm::BasicBlock::Create(
        mCtx, "ref.body", wrapper);
    auto* invalidCarrier = llvm::BasicBlock::Create(
        mCtx, "ref.invalid.carrier", wrapper);
    llvm::IRBuilder<> builder(entry);
    auto* source = wrapper->getArg(0);
    auto* output = wrapper->getArg(1);
    auto* nullHandle = llvm::ConstantPointerNull::get(
        llvm::cast<llvm::PointerType>(ptrTy()));
    auto* validAddresses = builder.CreateAnd(
        builder.CreateAnd(builder.CreateICmpNE(source, nullHandle),
                          builder.CreateICmpNE(output, nullHandle)),
        builder.CreateICmpNE(source, output));
    builder.CreateCondBr(validAddresses, outputCheck, invalidCarrier);
    builder.SetInsertPoint(invalidCarrier);
    builder.CreateRet(llvm::ConstantInt::getSigned(
        i32Ty(), LUNA_RUNTIME_FRAGMENT_REF_INVALID_CARRIER_V1));
    builder.SetInsertPoint(outputCheck);
    auto* outputEmpty = builder.CreateICmpEQ(
        builder.CreateLoad(ptrTy(), output, "ref.output.current"), nullHandle);
    builder.CreateCondBr(outputEmpty, ingress, invalidCarrier);

    builder.SetInsertPoint(ingress);
    auto* privateOwner = builder.CreateAlloca(
        ptrTy(), nullptr, "ref.private.owner");
    builder.CreateStore(nullHandle, privateOwner);
    if (!emitRuntimeFragmentRefOwnedIngressGate(
            builder, module, source, privateOwner, plan->target, bodyEntry)) {
        wrapper->eraseFromParent();
        return nullptr;
    }
    auto* taken = emitRuntimeFragmentRefTake(builder, privateOwner);
    auto* returned = builder.CreateCall(&body, {taken}, "ref.returned");
    returned->setCallingConv(body.getCallingConv());
    auto* returnedOwner = builder.CreateAlloca(
        ptrTy(), nullptr, "ref.return.owner");
    builder.CreateStore(returned, returnedOwner);
    auto* status = emitRuntimeFragmentRefOwnedTransfer(
        builder, module, returnedOwner, output, plan->target);
    if (!status) {
        wrapper->eraseFromParent();
        return nullptr;
    }
    auto* success = llvm::BasicBlock::Create(
        mCtx, "ref.return.accepted", wrapper);
    auto* failure = llvm::BasicBlock::Create(
        mCtx, "ref.return.failed", wrapper);
    builder.CreateCondBr(builder.CreateICmpEQ(
        status, llvm::ConstantInt::get(
            i32Ty(), LUNA_RUNTIME_FRAGMENT_REF_SUCCESS_V1)),
        success, failure);
    builder.SetInsertPoint(failure);
    emitRuntimeFragmentRefDrop(builder, module, returnedOwner);
    builder.CreateRet(status);
    builder.SetInsertPoint(success);
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
