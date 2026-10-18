#include "codegen/NativeOwnedResultFacts.h"

#include "core/TypeLayout.h"
#include "core/TypeRelations.h"
#include "driver/NativeTypedDescriptor.h"
#include "moonir/FragmentContextEffects.h"
#include "moonir/MoonIRModule.h"
#include "moonir/Verifier.h"
#include "runtime/RuntimeOwnedResult.h"

#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <utility>

namespace luna::codegen {

namespace {

bool boundedIdentity(const std::string& value) {
    return !value.empty() && value.size() < 4096 &&
        std::none_of(value.begin(), value.end(), [](char byte) {
            return byte == '\0' || byte == '\r' ||
                   byte == '\n' || byte == '\t';
        }) && luna::driver::validNativeDescriptorUtf8(value);
}

template <typename Matcher>
bool hasRequiredConjunct(const llvm::Value* condition,
                         const Matcher& matches,
                         unsigned remaining = 32) {
    if (!remaining) return false;
    if (const auto* conjunction = llvm::dyn_cast<llvm::BinaryOperator>(
            condition); conjunction &&
        conjunction->getOpcode() == llvm::Instruction::And &&
        conjunction->getType()->isIntegerTy(1))
        return hasRequiredConjunct(conjunction->getOperand(0), matches,
                                   remaining - 1) ||
            hasRequiredConjunct(conjunction->getOperand(1), matches,
                                remaining - 1);
    return matches(condition);
}

bool hasRequiredNonnullConjunct(const llvm::Value* condition,
                                const llvm::Value* output) {
    return hasRequiredConjunct(condition, [output](const llvm::Value* value) {
        const auto* comparison = llvm::dyn_cast<llvm::ICmpInst>(value);
        return comparison &&
            comparison->getPredicate() == llvm::ICmpInst::ICMP_NE &&
            ((comparison->getOperand(0) == output &&
              llvm::isa<llvm::ConstantPointerNull>(
                  comparison->getOperand(1))) ||
             (comparison->getOperand(1) == output &&
              llvm::isa<llvm::ConstantPointerNull>(
                  comparison->getOperand(0))));
    });
}

bool hasRequiredAlignmentConjunct(const llvm::Value* condition,
                                  const llvm::Value* output,
                                  uint64_t alignment) {
    return hasRequiredConjunct(condition,
        [output, alignment](const llvm::Value* value) {
            const auto* comparison = llvm::dyn_cast<llvm::ICmpInst>(value);
            if (!comparison ||
                comparison->getPredicate() != llvm::ICmpInst::ICMP_EQ)
                return false;
            const auto* masked = llvm::dyn_cast<llvm::BinaryOperator>(
                comparison->getOperand(0));
            const auto* zero = llvm::dyn_cast<llvm::ConstantInt>(
                comparison->getOperand(1));
            if (!masked || !zero || !zero->isZero()) {
                masked = llvm::dyn_cast<llvm::BinaryOperator>(
                    comparison->getOperand(1));
                zero = llvm::dyn_cast<llvm::ConstantInt>(
                    comparison->getOperand(0));
            }
            if (!masked || !zero || !zero->isZero() ||
                masked->getOpcode() != llvm::Instruction::And)
                return false;
            const auto* address = llvm::dyn_cast<llvm::PtrToIntInst>(
                masked->getOperand(0));
            const auto* mask = llvm::dyn_cast<llvm::ConstantInt>(
                masked->getOperand(1));
            if (!address || !mask) {
                address = llvm::dyn_cast<llvm::PtrToIntInst>(
                    masked->getOperand(1));
                mask = llvm::dyn_cast<llvm::ConstantInt>(
                    masked->getOperand(0));
            }
            return address && mask &&
                address->getPointerOperand() == output &&
                address->getType()->isIntegerTy(sizeof(void*) * 8) &&
                mask->getBitWidth() == sizeof(void*) * 8 &&
                mask->getZExtValue() == alignment - 1;
        });
}

} // namespace

bool deriveNativeOwnedResultSourceFacts(
    const moon::Module& program, const moon::FunctionDecl& function,
    NativeOwnedResultSourceFacts& facts, std::string& error) {
    const auto reject = [&](const char* reason) {
        error = reason;
        return false;
    };
    if (!program.typeTableSealed || !function.controlFlow ||
        !function.controlFlow->sealed || function.body || function.isExtern ||
        function.isKernel || function.isSelector ||
        !function.typeParams.empty() || !function.abi.empty() ||
        !function.linkName.empty() ||
        function.packageId != program.name || function.params.size() != 1 ||
        function.returnUsage == luna::ownership::Usage::Copy ||
        !function.requiresFragmentContext ||
        function.controlFlow->runtimeRefApplies.empty() ||
        !std::any_of(program.declarations.begin(), program.declarations.end(),
            [&function](const auto& declaration) {
                return declaration.get() == &function;
            }))
        return reject("Native v3 source candidate has no sealed Ref callable");
    moon::Verifier verifier;
    if (!verifier.verify(*function.controlFlow, program))
        return reject("Native v3 source candidate has no verified CFG");
    const auto effects = moon::computeFragmentContextEffects(program);
    const auto effect = effects.find(moon::fragmentContextEffectKey(
        {function.symbolId, function.contractId}));
    if (effect == effects.end() || !effect->second)
        return reject("Native v3 source candidate lost its context effect");

    const auto& parameter = function.params.front();
    const auto* ref = program.findType(parameter.type);
    const auto target = program.resolveRuntimeFragmentRefTarget(parameter.type);
    const auto* slot = target ? program.findDeclaration(*target) : nullptr;
    const auto* result = program.findType(function.returnType);
    const auto* scalar = result && result->typeArgumentIds.size() == 2
        ? program.findType(result->typeArgumentIds[0]) : nullptr;
    const auto* owned = result && result->typeArgumentIds.size() == 2
        ? program.findType(result->typeArgumentIds[1]) : nullptr;
    const auto* drop = owned && !owned->dropGlue.empty()
        ? program.findDeclaration(owned->dropGlue) : nullptr;
    if (parameter.isLinear ||
        parameter.relation != luna::ownership::Relation::SharedBorrow ||
        parameter.usage != luna::ownership::Usage::Copy ||
        !ref || ref->kind != TypeKind::RuntimeFragmentRef || !target ||
        !slot || slot->kind != moon::DeclarationKind::Slot ||
        !result || result->kind != TypeKind::Result ||
        result->typeArgumentIds.size() != 2 ||
        !scalar || scalar->kind != TypeKind::I32 ||
        !owned || owned->kind != TypeKind::Struct ||
        !owned->sysmeta.resource.needsDrop ||
        !owned->sysmeta.resource.cleanupRequired ||
        !drop || drop->kind != moon::DeclarationKind::Function ||
        drop->sourceName != "drop")
        return reject("Native v3 source candidate is outside the Ref/Result owner shape");

    const auto* declaration = program.findDeclarationById(function.declarationId);
    const auto* callable = declaration
        ? program.findType(declaration->type) : nullptr;
    if (!declaration || declaration->kind != moon::DeclarationKind::Function ||
        declaration->id != function.declarationId ||
        declaration->symbolId != function.symbolId ||
        declaration->symbolId != luna::identity::symbolIdFromCanonical(
            declaration->id) ||
        declaration->contractId != function.contractId ||
        declaration->linkageName != function.generatedSymbolName ||
        declaration->canonicalContract != moon::canonicalContract(*declaration) ||
        declaration->contractId != luna::identity::contractIdFromCanonical(
            declaration->canonicalContract) ||
        !callable || callable->kind != TypeKind::Function ||
        callable->parameterTypeIds.size() != 1 ||
        callable->parameterTypeIds.front() != parameter.type ||
        callable->parameterContracts.size() != 1 ||
        callable->parameterContracts.front() !=
            luna::ownership::Contract{parameter.relation, parameter.usage} ||
        callable->returnTypeId != function.returnType ||
        callable->returnContract != luna::ownership::Contract{
            luna::ownership::Relation::Owned, function.returnUsage} ||
        declaration->sysmeta.resource.parameters.size() != 1 ||
        declaration->sysmeta.resource.parameters.front() !=
            callable->parameterContracts.front() ||
        declaration->sysmeta.resource.result != callable->returnContract)
        return reject("Native v3 source candidate differs from its frozen signature");

    const auto& graph = *function.controlFlow;
    const moon::LocalRecord* parameterLocal = nullptr;
    for (const auto& local : graph.locals)
        if (local.kind == moon::LocalKind::Parameter) {
            if (parameterLocal)
                return reject("Native v3 source candidate has ambiguous Ref ingress");
            parameterLocal = &local;
        }
    if (!parameterLocal || parameterLocal->scope != graph.rootScope ||
        parameterLocal->name != parameter.name ||
        parameterLocal->type != parameter.type ||
        parameterLocal->relation != parameter.relation ||
        parameterLocal->usage != parameter.usage ||
        !std::all_of(graph.runtimeRefApplies.begin(),
                     graph.runtimeRefApplies.end(),
            [&](const auto& apply) {
                return apply.reference == parameterLocal->id &&
                    apply.slot == *target;
            }))
        return reject("Native v3 source candidate changed its Ref target");

    moon::TypeMaterializer materializer(program);
    const TypePtr restored = materializer.materialize(owned->id);
    if (!restored || luna::types::typeId(restored) != owned->id ||
        luna::types::canonicalType(restored) != owned->canonicalType ||
        luna::layout::valueSize(restored) != owned->valueSize ||
        luna::layout::valueAlignment(restored) != owned->valueAlignment ||
        owned->canonicalAbiLayout != moon::canonicalAbiLayout(*owned) ||
        owned->abiLayoutId != luna::identity::abiLayoutIdFromCanonical(
            owned->canonicalAbiLayout) ||
        owned->sysmeta.identity.abiLayout != owned->abiLayoutId ||
        owned->sysmeta.identity.type != owned->id ||
        owned->valueSize == 0 || owned->valueSize > (1u << 20) ||
        owned->valueAlignment == 0 || owned->valueAlignment > 4096 ||
        (owned->valueAlignment & (owned->valueAlignment - 1)) != 0)
        return reject("Native v3 source candidate lost its frozen owner layout");
    if (drop->symbolId != owned->dropGlue.symbol ||
        drop->contractId != owned->dropGlue.contract ||
        drop->canonicalContract != moon::canonicalContract(*drop) ||
        drop->contractId != luna::identity::contractIdFromCanonical(
            drop->canonicalContract))
        return reject("Native v3 source candidate lost its frozen Drop identity");

    NativeOwnedResultSourceFacts candidate;
    candidate.functionSymbolId = declaration->symbolId.value;
    candidate.functionContractId = declaration->contractId.value;
    candidate.sourceLinkageName = declaration->linkageName;
    candidate.refSlotSymbolId = target->symbol.value;
    candidate.refSlotContractId = target->contract.value;
    candidate.resultTypeId = result->id.value;
    candidate.errorTypeId = owned->id.value;
    candidate.errorAbiLayoutId = owned->abiLayoutId.value;
    candidate.errorDropSymbolId = drop->symbolId.value;
    candidate.errorDropContractId = drop->contractId.value;
    candidate.errorValueSize = owned->valueSize;
    candidate.errorValueAlignment = owned->valueAlignment;
    const std::array<std::string, 10> identities = {
        candidate.functionSymbolId, candidate.functionContractId,
        candidate.sourceLinkageName, candidate.refSlotSymbolId,
        candidate.refSlotContractId, candidate.resultTypeId,
        candidate.errorTypeId, candidate.errorAbiLayoutId,
        candidate.errorDropSymbolId, candidate.errorDropContractId};
    if (!std::all_of(identities.begin(), identities.end(), boundedIdentity))
        return reject("Native v3 source candidate has an invalid identity");
    facts = std::move(candidate);
    error.clear();
    return true;
}

std::array<std::string, 10>
NativeOwnedResultDescriptorV3Candidate::descriptorIdentities() const {
    return {source.functionSymbolId, source.functionContractId,
            wrapperLinkageName, source.refSlotSymbolId,
            source.refSlotContractId, source.resultTypeId,
            source.errorTypeId, source.errorAbiLayoutId,
            source.errorDropSymbolId, source.errorDropContractId};
}

std::string NativeOwnedResultDescriptorV3Candidate::canonicalRow() const {
    return luna::driver::canonicalNativeOwnedResultExportV3(
        LUNA_NATIVE_DECLARATION_FUNCTION_V1,
        LUNA_NATIVE_EXPORT_CALLABLE_V1,
        LUNA_NATIVE_ENTRY_ABI_REF_RESULT_OWNER_V1,
        LUNA_NATIVE_REF_SHARED_BORROW_V1,
        LUNA_NATIVE_RESULT_I32_OWNED_ERROR_V1,
        LUNA_NATIVE_STATUS_DOMAIN_REF_RESULT_OWNER_V1,
        source.errorValueSize, source.errorValueAlignment,
        descriptorIdentities());
}

bool deriveNativeOwnedResultDescriptorV3Candidate(
    const moon::Module& program, const moon::FunctionDecl& function,
    const std::string& wrapperLinkageName,
    NativeOwnedResultDescriptorV3Candidate& candidate, std::string& error) {
    NativeOwnedResultSourceFacts source;
    if (!deriveNativeOwnedResultSourceFacts(program, function, source, error))
        return false;
    if (!boundedIdentity(wrapperLinkageName) ||
        wrapperLinkageName == source.sourceLinkageName) {
        error = "Native v3 candidate has no distinct wrapper linkage";
        return false;
    }
    NativeOwnedResultDescriptorV3Candidate verified;
    verified.source = std::move(source);
    verified.wrapperLinkageName = wrapperLinkageName;
    candidate = std::move(verified);
    error.clear();
    return true;
}

bool derivePairedNativeOwnedResultDescriptorV3Candidate(
    const moon::Module& program, const moon::FunctionDecl& function,
    const llvm::Module& generated, const std::string& wrapperLinkageName,
    const std::string& hostLinkageName,
    NativeOwnedResultDescriptorV3Candidate& candidate, std::string& error) {
    NativeOwnedResultDescriptorV3Candidate verified;
    if (!deriveNativeOwnedResultDescriptorV3Candidate(
            program, function, wrapperLinkageName, verified, error))
        return false;
    const auto reject = [&](const char* reason) {
        error = reason;
        return false;
    };
    const auto* body = generated.getFunction(
        verified.source.sourceLinkageName);
    const auto* entry = generated.getFunction(wrapperLinkageName);
    const auto* host = generated.getFunction(hostLinkageName);
    const auto exactEntry = [](const llvm::Function* value,
                               unsigned pointerCount,
                               bool injection) {
        if (!value || value->isDeclaration() || value->isVarArg() ||
            value->getCallingConv() != llvm::CallingConv::C ||
            !value->getReturnType()->isIntegerTy(32) ||
            value->arg_size() != pointerCount + (injection ? 1u : 0u))
            return false;
        for (unsigned index = 0; index < pointerCount; ++index)
            if (!value->getArg(index)->getType()->isPointerTy() ||
                value->getArg(index)->getType()->getPointerAddressSpace() != 0)
                return false;
        return !injection ||
            value->getArg(pointerCount)->getType()->isIntegerTy(32);
    };
    if (!body || body->isDeclaration() || !body->hasInternalLinkage() ||
        !exactEntry(entry, 6, false) || !entry->hasExternalLinkage() ||
        !exactEntry(host, 6, true) || !host->hasExternalLinkage() ||
        entry == host || entry->size() != 1)
        return reject("Native v3 candidate lacks its exact generated entry pair");
    const auto& block = entry->getEntryBlock();
    if (block.size() != 2)
        return reject("Native v3 candidate entry has extra instructions");
    const auto* call = llvm::dyn_cast<llvm::CallInst>(&block.front());
    const auto* returned = llvm::dyn_cast<llvm::ReturnInst>(
        block.getTerminator());
    if (!call || call->getCalledFunction() != host ||
        call->getCallingConv() != llvm::CallingConv::C ||
        call->arg_size() != 7 || !returned ||
        returned->getReturnValue() != call)
        return reject("Native v3 candidate entry does not return its host call");
    for (unsigned index = 0; index < 6; ++index)
        if (call->getArgOperand(index) != entry->getArg(index))
            return reject("Native v3 candidate entry changed a host argument");
    const auto* injection = llvm::dyn_cast<llvm::ConstantInt>(
        call->getArgOperand(6));
    if (!injection || !injection->isZero())
        return reject("Native v3 candidate entry enables test injection");
    candidate = std::move(verified);
    error.clear();
    return true;
}

bool verifyNativeOwnedResultHostAdoption(
    const llvm::Module& generated, const std::string& hostLinkageName,
    const std::string& dropLinkageName, std::string& error) {
    const auto reject = [&](const char* reason) {
        error = reason;
        return false;
    };
    const auto* host = generated.getFunction(hostLinkageName);
    const auto* drop = generated.getFunction(dropLinkageName);
    const auto* adoption = generated.getFunction(
        "luna_runtime_owned_result_adopt_v1");
    if (!host || host->isDeclaration() || host->isVarArg() ||
        host->getCallingConv() != llvm::CallingConv::C ||
        !host->getReturnType()->isIntegerTy(32) || host->arg_size() != 7 ||
        !host->hasExternalLinkage() ||
        !drop || drop->isDeclaration() || drop->isVarArg() ||
        drop->getCallingConv() != llvm::CallingConv::C ||
        !drop->getReturnType()->isIntegerTy(32) || drop->arg_size() != 1 ||
        !adoption || !adoption->isDeclaration() || adoption->isVarArg() ||
        adoption->getCallingConv() != llvm::CallingConv::C ||
        !adoption->getReturnType()->isIntegerTy(32) ||
        adoption->arg_size() != 4)
        return reject("Native v3 host adoption lacks its exact C profile");
    for (unsigned index = 0; index < 6; ++index)
        if (!host->getArg(index)->getType()->isPointerTy() ||
            host->getArg(index)->getType()->getPointerAddressSpace() != 0)
            return reject("Native v3 host adoption changed a pointer parameter");
    if (!host->getArg(6)->getType()->isIntegerTy(32) ||
        !drop->getArg(0)->getType()->isPointerTy() ||
        drop->getArg(0)->getType()->getPointerAddressSpace() != 0)
        return reject("Native v3 host adoption changed its status or Drop parameter");
    for (const auto& argument : adoption->args())
        if (!argument.getType()->isPointerTy() ||
            argument.getType()->getPointerAddressSpace() != 0)
            return reject("Native v3 host adoption changed a Runtime parameter");
    const llvm::CallInst* adoptCall = nullptr;
    for (const auto& block : *host)
        for (const auto& instruction : block)
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                    &instruction); call && call->getCalledFunction() == adoption) {
                if (adoptCall)
                    return reject("Native v3 host adoption calls Runtime twice");
                adoptCall = call;
            }
    if (!adoptCall || adoptCall->getCallingConv() != llvm::CallingConv::C ||
        adoptCall->arg_size() != 4 ||
        adoptCall->getArgOperand(1) != drop ||
        adoptCall->getArgOperand(2) != host->getArg(5) ||
        adoptCall->getArgOperand(3) != host->getArg(4))
        return reject("Native v3 host adoption changed owner, Drop or lease wiring");
    const auto* ownerLoad = llvm::dyn_cast<llvm::LoadInst>(
        adoptCall->getArgOperand(0));
    const auto* ownerCell = ownerLoad
        ? llvm::dyn_cast<llvm::AllocaInst>(ownerLoad->getPointerOperand())
        : nullptr;
    if (!ownerCell || ownerLoad->getFunction() != host ||
        ownerCell->getFunction() != host)
        return reject("Native v3 host adoption lost its local owner cell");
    const auto* adoptBranch = llvm::dyn_cast<llvm::BranchInst>(
        adoptCall->getParent()->getTerminator());
    const auto* successCheck = adoptBranch && adoptBranch->isConditional()
        ? llvm::dyn_cast<llvm::ICmpInst>(adoptBranch->getCondition())
        : nullptr;
    if (!successCheck || successCheck->getPredicate() !=
            llvm::ICmpInst::ICMP_EQ ||
        !((successCheck->getOperand(0) == adoptCall &&
           llvm::isa<llvm::ConstantInt>(successCheck->getOperand(1)) &&
           llvm::cast<llvm::ConstantInt>(successCheck->getOperand(1))->isZero()) ||
          (successCheck->getOperand(1) == adoptCall &&
           llvm::isa<llvm::ConstantInt>(successCheck->getOperand(0)) &&
           llvm::cast<llvm::ConstantInt>(successCheck->getOperand(0))->isZero())))
        return reject("Native v3 host adoption lost its success branch");
    const auto* committed = adoptBranch->getSuccessor(0);
    const auto* failed = adoptBranch->getSuccessor(1);
    if (std::distance(llvm::pred_begin(committed),
                      llvm::pred_end(committed)) != 1 ||
        *llvm::pred_begin(committed) != adoptCall->getParent() ||
        committed->size() != 2 || failed->size() != 2)
        return reject("Native v3 host adoption bypasses commit or cleanup");
    const auto* tagStore = llvm::dyn_cast<llvm::StoreInst>(
        &committed->front());
    const auto* committedReturn = llvm::dyn_cast<llvm::ReturnInst>(
        committed->getTerminator());
    const auto* tagLoad = tagStore
        ? llvm::dyn_cast<llvm::LoadInst>(tagStore->getValueOperand())
        : nullptr;
    if (!tagStore || tagStore->getPointerOperand() != host->getArg(2) ||
        !tagLoad || tagLoad->getFunction() != host ||
        !llvm::isa<llvm::AllocaInst>(tagLoad->getPointerOperand()) ||
        !committedReturn ||
        !llvm::isa<llvm::ConstantInt>(committedReturn->getReturnValue()) ||
        !llvm::cast<llvm::ConstantInt>(
            committedReturn->getReturnValue())->isZero())
        return reject("Native v3 host adoption lost its final tag commit");
    const auto* failedDrop = llvm::dyn_cast<llvm::CallInst>(&failed->front());
    const auto* failedReturn = llvm::dyn_cast<llvm::ReturnInst>(
        failed->getTerminator());
    const auto* failureStatus = failedReturn
        ? llvm::dyn_cast_or_null<llvm::ConstantInt>(
            failedReturn->getReturnValue()) : nullptr;
    if (!failedDrop || failedDrop->getCalledFunction() != drop ||
        failedDrop->getCallingConv() != llvm::CallingConv::C ||
        failedDrop->arg_size() != 1 ||
        failedDrop->getArgOperand(0) != ownerCell ||
        !failureStatus || failureStatus->getZExtValue() !=
            LUNA_NATIVE_REF_RESULT_OWNER_ADOPTION_FAILURE_V1)
        return reject("Native v3 host adoption lost failed-owner cleanup");
    error.clear();
    return true;
}

bool verifyNativeOwnedResultHostTransferFork(
    const llvm::Module& generated, const std::string& hostLinkageName,
    const std::string& transferLinkageName, std::string& error) {
    const auto reject = [&](const char* reason) {
        error = reason;
        return false;
    };
    const auto* host = generated.getFunction(hostLinkageName);
    const auto* transfer = generated.getFunction(transferLinkageName);
    if (!host || host->isDeclaration() || host->isVarArg() ||
        host->getCallingConv() != llvm::CallingConv::C ||
        !host->getReturnType()->isIntegerTy(32) || host->arg_size() != 7 ||
        !host->hasExternalLinkage() ||
        !transfer || transfer->isDeclaration() || transfer->isVarArg() ||
        transfer->getCallingConv() != llvm::CallingConv::C ||
        !transfer->getReturnType()->isIntegerTy(32) ||
        transfer->arg_size() != 6 || !transfer->hasExternalLinkage())
        return reject("Native v3 host transfer lacks its exact C profile");
    for (unsigned index = 0; index < 6; ++index)
        if (!host->getArg(index)->getType()->isPointerTy() ||
            host->getArg(index)->getType()->getPointerAddressSpace() != 0)
            return reject("Native v3 host transfer changed a pointer parameter");
    if (!host->getArg(6)->getType()->isIntegerTy(32))
        return reject("Native v3 host transfer changed its injection parameter");
    for (unsigned index = 0; index < 5; ++index)
        if (!transfer->getArg(index)->getType()->isPointerTy() ||
            transfer->getArg(index)->getType()->getPointerAddressSpace() != 0)
            return reject("Native v3 host transfer changed a scratch parameter");
    if (!transfer->getArg(5)->getType()->isIntegerTy(32))
        return reject("Native v3 host transfer changed its injection parameter");
    const llvm::CallInst* transferCall = nullptr;
    for (const auto& block : *host)
        for (const auto& instruction : block)
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                    &instruction); call && call->getCalledFunction() == transfer) {
                if (transferCall)
                    return reject("Native v3 host transfer calls the body twice");
                transferCall = call;
            }
    if (!transferCall ||
        transferCall->getCallingConv() != llvm::CallingConv::C ||
        transferCall->arg_size() != 6 ||
        transferCall->getArgOperand(0) != host->getArg(0) ||
        transferCall->getArgOperand(1) != host->getArg(1) ||
        !llvm::isa<llvm::ConstantInt>(transferCall->getArgOperand(5)) ||
        !llvm::cast<llvm::ConstantInt>(
            transferCall->getArgOperand(5))->isZero())
        return reject("Native v3 host transfer changed ingress or injection");
    const auto* tagCell = llvm::dyn_cast<llvm::AllocaInst>(
        transferCall->getArgOperand(2));
    const auto* scalarCell = llvm::dyn_cast<llvm::AllocaInst>(
        transferCall->getArgOperand(3));
    const auto* ownerCell = llvm::dyn_cast<llvm::AllocaInst>(
        transferCall->getArgOperand(4));
    if (!tagCell || !scalarCell || !ownerCell ||
        tagCell == scalarCell || tagCell == ownerCell ||
        scalarCell == ownerCell ||
        tagCell->getFunction() != host ||
        scalarCell->getFunction() != host ||
        ownerCell->getFunction() != host ||
        !tagCell->getAllocatedType()->isIntegerTy(32) ||
        !scalarCell->getAllocatedType()->isIntegerTy(32) ||
        !ownerCell->getAllocatedType()->isPointerTy() ||
        ownerCell->getAllocatedType()->getPointerAddressSpace() != 0)
        return reject("Native v3 host transfer lost its local scratch cells");
    const auto* block = transferCall->getParent();
    const auto* init = block->size() == 4
        ? llvm::dyn_cast<llvm::StoreInst>(&block->front()) : nullptr;
    const auto* branch = llvm::dyn_cast<llvm::BranchInst>(
        block->getTerminator());
    const auto* check = branch && branch->isConditional()
        ? llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition()) : nullptr;
    if (!init || !llvm::isa<llvm::ConstantPointerNull>(
            init->getValueOperand()) ||
        init->getPointerOperand() != ownerCell ||
        &*std::next(block->begin()) != transferCall ||
        &*std::next(block->begin(), 2) != check ||
        !check || check->getPredicate() != llvm::ICmpInst::ICMP_EQ ||
        !((check->getOperand(0) == transferCall &&
           llvm::isa<llvm::ConstantInt>(check->getOperand(1)) &&
           llvm::cast<llvm::ConstantInt>(check->getOperand(1))->isZero()) ||
          (check->getOperand(1) == transferCall &&
           llvm::isa<llvm::ConstantInt>(check->getOperand(0)) &&
           llvm::cast<llvm::ConstantInt>(check->getOperand(0))->isZero())))
        return reject("Native v3 host transfer lost its initialized success fork");
    const auto* succeeded = branch->getSuccessor(0);
    const auto* failed = branch->getSuccessor(1);
    if (std::distance(llvm::pred_begin(succeeded),
                      llvm::pred_end(succeeded)) != 1 ||
        *llvm::pred_begin(succeeded) != block ||
        std::distance(llvm::pred_begin(failed),
                      llvm::pred_end(failed)) != 1 ||
        *llvm::pred_begin(failed) != block || failed->size() != 1)
        return reject("Native v3 host transfer bypasses its result fork");
    const auto* failedReturn = llvm::dyn_cast<llvm::ReturnInst>(
        failed->getTerminator());
    if (!failedReturn || failedReturn->getReturnValue() != transferCall)
        return reject("Native v3 host transfer changed its failure status");
    error.clear();
    return true;
}

bool verifyNativeOwnedResultHostOwnerLeaseGates(
    const llvm::Module& generated, const std::string& hostLinkageName,
    const std::string& transferLinkageName, std::string& error) {
    const auto reject = [&](const char* reason) {
        error = reason;
        return false;
    };
    const auto* host = generated.getFunction(hostLinkageName);
    const auto* transfer = generated.getFunction(transferLinkageName);
    const auto* lease = generated.getFunction(
        "luna_runtime_owned_result_lease_check_v1");
    if (!host || host->isDeclaration() || host->isVarArg() ||
        host->getCallingConv() != llvm::CallingConv::C ||
        !host->getReturnType()->isIntegerTy(32) ||
        host->arg_size() != 7 || !host->hasExternalLinkage() ||
        !host->getArg(4)->getType()->isPointerTy() ||
        host->getArg(4)->getType()->getPointerAddressSpace() != 0 ||
        !host->getArg(5)->getType()->isPointerTy() ||
        host->getArg(5)->getType()->getPointerAddressSpace() != 0 ||
        !transfer || transfer->isDeclaration() ||
        !lease || !lease->isDeclaration() || lease->isVarArg() ||
        lease->getCallingConv() != llvm::CallingConv::C ||
        !lease->getReturnType()->isIntegerTy(32) ||
        lease->arg_size() != 1 ||
        !lease->getArg(0)->getType()->isPointerTy() ||
        lease->getArg(0)->getType()->getPointerAddressSpace() != 0)
        return reject("Native v3 host preflight lacks its Runtime lease profile");
    const llvm::CallInst* transferCall = nullptr;
    for (const auto& block : *host)
        for (const auto& instruction : block)
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                    &instruction); call && call->getCalledFunction() == transfer) {
                if (transferCall)
                    return reject("Native v3 host preflight has multiple transfers");
                transferCall = call;
            }
    if (!transferCall)
        return reject("Native v3 host preflight has no transfer");
    const auto* transferBlock = transferCall->getParent();
    if (std::distance(llvm::pred_begin(transferBlock),
                      llvm::pred_end(transferBlock)) != 1)
        return reject("Native v3 host transfer bypasses its lease gate");
    const auto* leaseBlock = *llvm::pred_begin(transferBlock);
    if (leaseBlock->size() != 3)
        return reject("Native v3 host lease gate changed its instructions");
    const auto* leaseCall = llvm::dyn_cast<llvm::CallInst>(
        &leaseBlock->front());
    const auto* leaseCompare = llvm::dyn_cast<llvm::ICmpInst>(
        &*std::next(leaseBlock->begin()));
    const auto* leaseBranch = llvm::dyn_cast<llvm::BranchInst>(
        leaseBlock->getTerminator());
    if (!leaseCall || leaseCall->getCalledFunction() != lease ||
        leaseCall->getCallingConv() != llvm::CallingConv::C ||
        leaseCall->arg_size() != 1 ||
        leaseCall->getArgOperand(0) != host->getArg(5) ||
        !leaseCompare ||
        leaseCompare->getPredicate() != llvm::ICmpInst::ICMP_EQ ||
        !((leaseCompare->getOperand(0) == leaseCall &&
           llvm::isa<llvm::ConstantInt>(leaseCompare->getOperand(1)) &&
           llvm::cast<llvm::ConstantInt>(leaseCompare->getOperand(1))
               ->getZExtValue() == LUNA_RUNTIME_OWNED_RESULT_LEASE_LIVE_V1) ||
          (leaseCompare->getOperand(1) == leaseCall &&
           llvm::isa<llvm::ConstantInt>(leaseCompare->getOperand(0)) &&
           llvm::cast<llvm::ConstantInt>(leaseCompare->getOperand(0))
               ->getZExtValue() == LUNA_RUNTIME_OWNED_RESULT_LEASE_LIVE_V1)) ||
        !leaseBranch || !leaseBranch->isConditional() ||
        leaseBranch->getCondition() != leaseCompare ||
        leaseBranch->getSuccessor(0) != transferBlock)
        return reject("Native v3 host lease gate changed its live branch");
    const auto* invalidLease = leaseBranch->getSuccessor(1);
    const auto* leaseFailure = invalidLease->size() == 1
        ? llvm::dyn_cast<llvm::ReturnInst>(
            invalidLease->getTerminator()) : nullptr;
    const auto* leaseStatus = leaseFailure
        ? llvm::dyn_cast_or_null<llvm::ConstantInt>(
            leaseFailure->getReturnValue()) : nullptr;
    if (!leaseStatus || leaseStatus->getZExtValue() !=
            LUNA_NATIVE_REF_RESULT_OWNER_INVALID_LEASE_V1 ||
        std::distance(llvm::pred_begin(invalidLease),
                      llvm::pred_end(invalidLease)) != 1 ||
        *llvm::pred_begin(invalidLease) != leaseBlock ||
        std::distance(llvm::pred_begin(leaseBlock),
                      llvm::pred_end(leaseBlock)) != 1)
        return reject("Native v3 host lease failure or ingress changed");
    const auto* ownerBlock = *llvm::pred_begin(leaseBlock);
    if (ownerBlock->size() != 3)
        return reject("Native v3 host owner-cell gate changed its instructions");
    const auto* ownerLoad = llvm::dyn_cast<llvm::LoadInst>(
        &ownerBlock->front());
    const auto* ownerCompare = llvm::dyn_cast<llvm::ICmpInst>(
        &*std::next(ownerBlock->begin()));
    const auto* ownerBranch = llvm::dyn_cast<llvm::BranchInst>(
        ownerBlock->getTerminator());
    if (!ownerLoad || ownerLoad->getPointerOperand() != host->getArg(4) ||
        !ownerLoad->getType()->isPointerTy() ||
        ownerLoad->getType()->getPointerAddressSpace() != 0 ||
        !ownerCompare ||
        ownerCompare->getPredicate() != llvm::ICmpInst::ICMP_EQ ||
        !((ownerCompare->getOperand(0) == ownerLoad &&
           llvm::isa<llvm::ConstantPointerNull>(
               ownerCompare->getOperand(1))) ||
          (ownerCompare->getOperand(1) == ownerLoad &&
           llvm::isa<llvm::ConstantPointerNull>(
               ownerCompare->getOperand(0)))) ||
        !ownerBranch || !ownerBranch->isConditional() ||
        ownerBranch->getCondition() != ownerCompare ||
        ownerBranch->getSuccessor(0) != leaseBlock)
        return reject("Native v3 host owner-cell gate changed its empty branch");
    const auto* invalidOutput = ownerBranch->getSuccessor(1);
    const auto* outputFailure = invalidOutput->size() == 1
        ? llvm::dyn_cast<llvm::ReturnInst>(
            invalidOutput->getTerminator()) : nullptr;
    const auto* outputStatus = outputFailure
        ? llvm::dyn_cast_or_null<llvm::ConstantInt>(
            outputFailure->getReturnValue()) : nullptr;
    const auto* entry = &host->getEntryBlock();
    const auto* entryBranch = llvm::dyn_cast<llvm::BranchInst>(
        entry->getTerminator());
    if (!outputStatus || outputStatus->getZExtValue() !=
            LUNA_NATIVE_REF_RESULT_OWNER_INVALID_OUTPUT_V1 ||
        std::distance(llvm::pred_begin(ownerBlock),
                      llvm::pred_end(ownerBlock)) != 1 ||
        *llvm::pred_begin(ownerBlock) != entry ||
        !entryBranch || !entryBranch->isConditional() ||
        entryBranch->getSuccessor(0) != ownerBlock ||
        entryBranch->getSuccessor(1) != invalidOutput ||
        std::distance(llvm::pred_begin(invalidOutput),
                      llvm::pred_end(invalidOutput)) != 2)
        return reject("Native v3 host owner-cell failure or ingress changed");
    for (const auto* predecessor : llvm::predecessors(invalidOutput))
        if (predecessor != entry && predecessor != ownerBlock)
            return reject("Native v3 host output failure has another ingress");
    error.clear();
    return true;
}

bool verifyNativeOwnedResultHostOutputNonNull(
    const llvm::Module& generated, const std::string& hostLinkageName,
    std::string& error) {
    const auto reject = [&](const char* reason) {
        error = reason;
        return false;
    };
    const auto* host = generated.getFunction(hostLinkageName);
    if (!host || host->isDeclaration() || host->isVarArg() ||
        host->getCallingConv() != llvm::CallingConv::C ||
        !host->getReturnType()->isIntegerTy(32) ||
        host->arg_size() != 7 || !host->hasExternalLinkage())
        return reject("Native v3 host output check lacks its C profile");
    const auto* branch = llvm::dyn_cast<llvm::BranchInst>(
        host->getEntryBlock().getTerminator());
    if (!branch || !branch->isConditional())
        return reject("Native v3 host output check lost its entry fork");
    for (unsigned index = 2; index <= 4; ++index) {
        const auto* output = host->getArg(index);
        if (!output->getType()->isPointerTy() ||
            output->getType()->getPointerAddressSpace() != 0 ||
            !hasRequiredNonnullConjunct(branch->getCondition(), output))
            return reject("Native v3 host output check lost a non-null guard");
    }
    const auto* invalid = branch->getSuccessor(1);
    const auto* returned = invalid->size() == 1
        ? llvm::dyn_cast<llvm::ReturnInst>(
            invalid->getTerminator()) : nullptr;
    const auto* status = returned
        ? llvm::dyn_cast_or_null<llvm::ConstantInt>(
            returned->getReturnValue()) : nullptr;
    if (!status || status->getZExtValue() !=
            LUNA_NATIVE_REF_RESULT_OWNER_INVALID_OUTPUT_V1)
        return reject("Native v3 host output check lost its failure status");
    error.clear();
    return true;
}

bool verifyNativeOwnedResultHostOutputAlignment(
    const llvm::Module& generated, const std::string& hostLinkageName,
    std::string& error) {
    if (!verifyNativeOwnedResultHostOutputNonNull(
            generated, hostLinkageName, error))
        return false;
    const auto* host = generated.getFunction(hostLinkageName);
    const auto* condition = llvm::cast<llvm::BranchInst>(
        host->getEntryBlock().getTerminator())->getCondition();
    const std::array<uint64_t, 3> alignments = {
        alignof(uint32_t), alignof(int32_t), alignof(void*)};
    for (unsigned index = 0; index < alignments.size(); ++index)
        if (!hasRequiredAlignmentConjunct(
                condition, host->getArg(index + 2), alignments[index])) {
            error = "Native v3 host output check lost an alignment guard";
            return false;
        }
    error.clear();
    return true;
}

} // namespace luna::codegen
