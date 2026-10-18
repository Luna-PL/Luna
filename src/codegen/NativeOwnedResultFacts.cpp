#include "codegen/NativeOwnedResultFacts.h"

#include "core/TypeLayout.h"
#include "core/TypeRelations.h"
#include "driver/NativeTypedDescriptor.h"
#include "moonir/FragmentContextEffects.h"
#include "moonir/MoonIRModule.h"
#include "moonir/Verifier.h"
#include "runtime/RuntimeFragmentABI.h"
#include "runtime/RuntimeFragmentCompilerBridge.h"
#include "runtime/RuntimeOwnedResult.h"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
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

bool hasRequiredDisjointConjunct(const llvm::Value* condition,
                                 const llvm::Value* left,
                                 uint64_t leftSize,
                                 const llvm::Value* right,
                                 uint64_t rightSize) {
    return hasRequiredConjunct(condition,
        [left, leftSize, right, rightSize](const llvm::Value* value) {
            const auto* disjoint = llvm::dyn_cast<llvm::SelectInst>(value);
            const auto* before = disjoint
                ? llvm::dyn_cast<llvm::ICmpInst>(
                    disjoint->getCondition()) : nullptr;
            if (!before || before->getPredicate() !=
                    llvm::ICmpInst::ICMP_ULT)
                return false;
            const auto* leftAddress = llvm::dyn_cast<llvm::PtrToIntInst>(
                before->getOperand(0));
            const auto* rightAddress = llvm::dyn_cast<llvm::PtrToIntInst>(
                before->getOperand(1));
            if (!leftAddress || !rightAddress ||
                leftAddress->getPointerOperand() != left ||
                rightAddress->getPointerOperand() != right ||
                !leftAddress->getType()->isIntegerTy(sizeof(void*) * 8) ||
                rightAddress->getType() != leftAddress->getType())
                return false;
            const auto* leftGap = llvm::dyn_cast<llvm::ICmpInst>(
                disjoint->getTrueValue());
            const auto* rightGap = llvm::dyn_cast<llvm::ICmpInst>(
                disjoint->getFalseValue());
            if (!leftGap || !rightGap ||
                leftGap->getPredicate() != llvm::ICmpInst::ICMP_UGE ||
                rightGap->getPredicate() != llvm::ICmpInst::ICMP_UGE ||
                leftGap->getOperand(0) != rightGap->getOperand(0))
                return false;
            const auto* gap = llvm::dyn_cast<llvm::SelectInst>(
                leftGap->getOperand(0));
            const auto* leftExtent = llvm::dyn_cast<llvm::ConstantInt>(
                leftGap->getOperand(1));
            const auto* rightExtent = llvm::dyn_cast<llvm::ConstantInt>(
                rightGap->getOperand(1));
            if (!gap || gap->getCondition() != before ||
                !leftExtent || !rightExtent ||
                leftExtent->getBitWidth() != sizeof(void*) * 8 ||
                rightExtent->getBitWidth() != sizeof(void*) * 8 ||
                leftExtent->getZExtValue() != leftSize ||
                rightExtent->getZExtValue() != rightSize)
                return false;
            const auto* forward = llvm::dyn_cast<llvm::BinaryOperator>(
                gap->getTrueValue());
            const auto* backward = llvm::dyn_cast<llvm::BinaryOperator>(
                gap->getFalseValue());
            return forward && backward &&
                forward->getOpcode() == llvm::Instruction::Sub &&
                backward->getOpcode() == llvm::Instruction::Sub &&
                forward->getOperand(0) == rightAddress &&
                forward->getOperand(1) == leftAddress &&
                backward->getOperand(0) == leftAddress &&
                backward->getOperand(1) == rightAddress;
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

bool verifyNativeOwnedResultHostScalarCommit(
    const llvm::Module& generated, const std::string& hostLinkageName,
    const std::string& transferLinkageName, std::string& error) {
    if (!verifyNativeOwnedResultHostTransferFork(
            generated, hostLinkageName, transferLinkageName, error))
        return false;
    const auto reject = [&](const char* reason) {
        error = reason;
        return false;
    };
    const auto* host = generated.getFunction(hostLinkageName);
    const auto* transfer = generated.getFunction(transferLinkageName);
    const llvm::CallInst* transferCall = nullptr;
    for (const auto& block : *host)
        for (const auto& instruction : block)
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                    &instruction); call && call->getCalledFunction() == transfer)
                transferCall = call;
    const auto* tagCell = transferCall->getArgOperand(2);
    const auto* scalarCell = transferCall->getArgOperand(3);
    const auto* ownerCell = transferCall->getArgOperand(4);
    const auto* transferBranch = llvm::cast<llvm::BranchInst>(
        transferCall->getParent()->getTerminator());
    const auto* dispatched = transferBranch->getSuccessor(0);
    if (dispatched->size() != 4)
        return reject("Native v3 host scalar dispatch changed its instructions");
    const auto* tagLoad = llvm::dyn_cast<llvm::LoadInst>(
        &dispatched->front());
    const auto* ownerLoad = llvm::dyn_cast<llvm::LoadInst>(
        &*std::next(dispatched->begin()));
    const auto* dispatchBranch = llvm::dyn_cast<llvm::BranchInst>(
        dispatched->getTerminator());
    const auto* resourceCheck = dispatchBranch && dispatchBranch->isConditional()
        ? llvm::dyn_cast<llvm::ICmpInst>(dispatchBranch->getCondition())
        : nullptr;
    const auto equalsInteger = [](const llvm::ICmpInst* check,
                                  const llvm::Value* value,
                                  uint64_t expected) {
        return check && check->getPredicate() == llvm::ICmpInst::ICMP_EQ &&
            ((check->getOperand(0) == value &&
              llvm::isa<llvm::ConstantInt>(check->getOperand(1)) &&
              llvm::cast<llvm::ConstantInt>(check->getOperand(1))
                  ->getZExtValue() == expected) ||
             (check->getOperand(1) == value &&
              llvm::isa<llvm::ConstantInt>(check->getOperand(0)) &&
              llvm::cast<llvm::ConstantInt>(check->getOperand(0))
                  ->getZExtValue() == expected));
    };
    if (!tagLoad || tagLoad->getPointerOperand() != tagCell ||
        !tagLoad->getType()->isIntegerTy(32) ||
        !ownerLoad || ownerLoad->getPointerOperand() != ownerCell ||
        !ownerLoad->getType()->isPointerTy() ||
        &*std::next(dispatched->begin(), 2) != resourceCheck ||
        !equalsInteger(resourceCheck, tagLoad, 0))
        return reject("Native v3 host scalar dispatch lost its transfer results");
    const auto* resource = dispatchBranch->getSuccessor(0);
    const auto* scalar = dispatchBranch->getSuccessor(1);
    if (resource == scalar ||
        std::distance(llvm::pred_begin(resource),
                      llvm::pred_end(resource)) != 1 ||
        *llvm::pred_begin(resource) != dispatched ||
        std::distance(llvm::pred_begin(scalar),
                      llvm::pred_end(scalar)) != 1 ||
        *llvm::pred_begin(scalar) != dispatched || scalar->size() != 4)
        return reject("Native v3 host scalar branch has another ingress");
    const auto* scalarTagCheck = llvm::dyn_cast<llvm::ICmpInst>(
        &scalar->front());
    const auto* nullOwnerCheck = llvm::dyn_cast<llvm::ICmpInst>(
        &*std::next(scalar->begin()));
    const auto* both = llvm::dyn_cast<llvm::BinaryOperator>(
        &*std::next(scalar->begin(), 2));
    const auto* scalarBranch = llvm::dyn_cast<llvm::BranchInst>(
        scalar->getTerminator());
    if (!equalsInteger(scalarTagCheck, tagLoad, 1) ||
        !nullOwnerCheck ||
        nullOwnerCheck->getPredicate() != llvm::ICmpInst::ICMP_EQ ||
        !((nullOwnerCheck->getOperand(0) == ownerLoad &&
           llvm::isa<llvm::ConstantPointerNull>(
               nullOwnerCheck->getOperand(1))) ||
          (nullOwnerCheck->getOperand(1) == ownerLoad &&
           llvm::isa<llvm::ConstantPointerNull>(
               nullOwnerCheck->getOperand(0)))) ||
        !both || both->getOpcode() != llvm::Instruction::And ||
        !((both->getOperand(0) == scalarTagCheck &&
           both->getOperand(1) == nullOwnerCheck) ||
          (both->getOperand(1) == scalarTagCheck &&
           both->getOperand(0) == nullOwnerCheck)) ||
        !scalarBranch || !scalarBranch->isConditional() ||
        scalarBranch->getCondition() != both)
        return reject("Native v3 host scalar commit lost its tag or empty-owner gate");
    const auto* committed = scalarBranch->getSuccessor(0);
    const auto* invalid = scalarBranch->getSuccessor(1);
    if (committed == invalid || committed == resource ||
        invalid == resource ||
        std::distance(llvm::pred_begin(committed),
                      llvm::pred_end(committed)) != 1 ||
        *llvm::pred_begin(committed) != scalar ||
        std::distance(llvm::pred_begin(invalid),
                      llvm::pred_end(invalid)) != 1 ||
        *llvm::pred_begin(invalid) != scalar || committed->size() != 4)
        return reject("Native v3 host scalar commit bypasses its gate");
    const auto* scalarLoad = llvm::dyn_cast<llvm::LoadInst>(
        &committed->front());
    const auto* scalarStore = llvm::dyn_cast<llvm::StoreInst>(
        &*std::next(committed->begin()));
    const auto* tagStore = llvm::dyn_cast<llvm::StoreInst>(
        &*std::next(committed->begin(), 2));
    const auto* returned = llvm::dyn_cast<llvm::ReturnInst>(
        committed->getTerminator());
    const auto* status = returned
        ? llvm::dyn_cast_or_null<llvm::ConstantInt>(
            returned->getReturnValue()) : nullptr;
    if (!scalarLoad || scalarLoad->getPointerOperand() != scalarCell ||
        !scalarLoad->getType()->isIntegerTy(32) ||
        !scalarStore || scalarStore->getValueOperand() != scalarLoad ||
        scalarStore->getPointerOperand() != host->getArg(3) ||
        !tagStore || tagStore->getValueOperand() != tagLoad ||
        tagStore->getPointerOperand() != host->getArg(2) ||
        !status || !status->isZero())
        return reject("Native v3 host scalar commit changed its output or status");
    error.clear();
    return true;
}

bool verifyNativeOwnedResultHostResourceValidation(
    const llvm::Module& generated, const std::string& hostLinkageName,
    const std::string& transferLinkageName,
    const std::string& dropLinkageName, std::string& error) {
    if (!verifyNativeOwnedResultHostScalarCommit(
            generated, hostLinkageName, transferLinkageName, error) ||
        !verifyNativeOwnedResultHostAdoption(
            generated, hostLinkageName, dropLinkageName, error))
        return false;
    const auto reject = [&](const char* reason) {
        error = reason;
        return false;
    };
    const auto* host = generated.getFunction(hostLinkageName);
    const auto* transfer = generated.getFunction(transferLinkageName);
    const auto* drop = generated.getFunction(dropLinkageName);
    const auto* adoption = generated.getFunction(
        "luna_runtime_owned_result_adopt_v1");
    const llvm::CallInst* transferCall = nullptr;
    const llvm::CallInst* adoptCall = nullptr;
    for (const auto& block : *host)
        for (const auto& instruction : block)
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                    &instruction)) {
                if (call->getCalledFunction() == transfer) transferCall = call;
                if (call->getCalledFunction() == adoption) adoptCall = call;
            }
    const auto* ownerCell = transferCall->getArgOperand(4);
    const auto* transferBranch = llvm::cast<llvm::BranchInst>(
        transferCall->getParent()->getTerminator());
    const auto* dispatched = transferBranch->getSuccessor(0);
    const auto* tagLoad = llvm::cast<llvm::LoadInst>(&dispatched->front());
    const auto* ownerLoad = llvm::cast<llvm::LoadInst>(
        &*std::next(dispatched->begin()));
    const auto* dispatchBranch = llvm::cast<llvm::BranchInst>(
        dispatched->getTerminator());
    const auto* resource = dispatchBranch->getSuccessor(0);
    const auto* scalar = dispatchBranch->getSuccessor(1);
    const auto* scalarBranch = llvm::cast<llvm::BranchInst>(
        scalar->getTerminator());
    const auto* invalidCheck = scalarBranch->getSuccessor(1);
    const auto solePredecessor = [](const llvm::BasicBlock* block,
                                    const llvm::BasicBlock* expected) {
        return std::distance(llvm::pred_begin(block),
                             llvm::pred_end(block)) == 1 &&
            *llvm::pred_begin(block) == expected;
    };
    const auto ownerComparison = [ownerLoad](const llvm::ICmpInst* check,
                                             llvm::ICmpInst::Predicate predicate) {
        return check && check->getPredicate() == predicate &&
            ((check->getOperand(0) == ownerLoad &&
              llvm::isa<llvm::ConstantPointerNull>(check->getOperand(1))) ||
             (check->getOperand(1) == ownerLoad &&
              llvm::isa<llvm::ConstantPointerNull>(check->getOperand(0))));
    };
    if (resource->size() != 2)
        return reject("Native v3 host resource owner gate changed its instructions");
    const auto* resourceCheck = llvm::dyn_cast<llvm::ICmpInst>(
        &resource->front());
    const auto* resourceBranch = llvm::dyn_cast<llvm::BranchInst>(
        resource->getTerminator());
    if (!ownerComparison(resourceCheck, llvm::ICmpInst::ICMP_NE) ||
        !resourceBranch || !resourceBranch->isConditional() ||
        resourceBranch->getCondition() != resourceCheck)
        return reject("Native v3 host resource arm lost its non-null owner gate");
    const auto* adoptGate = resourceBranch->getSuccessor(0);
    const auto* invalidReturn = resourceBranch->getSuccessor(1);
    if (adoptGate == invalidReturn || !solePredecessor(adoptGate, resource) ||
        adoptGate->size() != 2)
        return reject("Native v3 host resource arm bypasses adoption gate");
    const auto* injectionCheck = llvm::dyn_cast<llvm::ICmpInst>(
        &adoptGate->front());
    const auto* injectionBranch = llvm::dyn_cast<llvm::BranchInst>(
        adoptGate->getTerminator());
    const auto* injectionZero = injectionCheck &&
        injectionCheck->getPredicate() == llvm::ICmpInst::ICMP_EQ
        ? llvm::dyn_cast<llvm::ConstantInt>(injectionCheck->getOperand(1))
        : nullptr;
    if (!injectionZero || !injectionZero->isZero() ||
        injectionCheck->getOperand(0) != host->getArg(6) ||
        !injectionBranch || !injectionBranch->isConditional() ||
        injectionBranch->getCondition() != injectionCheck)
        return reject("Native v3 host resource adoption lost zero injection gate");
    const auto* adoptBlock = injectionBranch->getSuccessor(0);
    const auto* adoptionDrop = injectionBranch->getSuccessor(1);
    if (adoptBlock == adoptionDrop || !solePredecessor(adoptBlock, adoptGate) ||
        adoptBlock != adoptCall->getParent() || adoptBlock->size() != 3 ||
        &adoptBlock->front() != adoptCall ||
        adoptCall->getArgOperand(0) != ownerLoad)
        return reject("Native v3 host resource arm changed its adoption owner");
    const auto* adoptBranch = llvm::cast<llvm::BranchInst>(
        adoptBlock->getTerminator());
    const auto* committed = adoptBranch->getSuccessor(0);
    if (adoptBranch->getSuccessor(1) != adoptionDrop ||
        std::distance(llvm::pred_begin(adoptionDrop),
                      llvm::pred_end(adoptionDrop)) != 2)
        return reject("Native v3 host resource adoption bypasses failed-owner Drop");
    for (const auto* predecessor : llvm::predecessors(adoptionDrop))
        if (predecessor != adoptGate && predecessor != adoptBlock)
            return reject("Native v3 host adoption Drop has another ingress");
    const auto* committedTag = llvm::cast<llvm::StoreInst>(
        &committed->front());
    if (committedTag->getValueOperand() != tagLoad)
        return reject("Native v3 host resource commit changed its transfer tag");
    if (invalidCheck->size() != 2)
        return reject("Native v3 host invalid-resource check changed instructions");
    const auto* invalidOwnerCheck = llvm::dyn_cast<llvm::ICmpInst>(
        &invalidCheck->front());
    const auto* invalidBranch = llvm::dyn_cast<llvm::BranchInst>(
        invalidCheck->getTerminator());
    if (!ownerComparison(invalidOwnerCheck, llvm::ICmpInst::ICMP_NE) ||
        !invalidBranch || !invalidBranch->isConditional() ||
        invalidBranch->getCondition() != invalidOwnerCheck ||
        invalidBranch->getSuccessor(1) != invalidReturn)
        return reject("Native v3 host invalid result lost its owner fork");
    const auto* invalidDrop = invalidBranch->getSuccessor(0);
    if (invalidDrop == invalidReturn || !solePredecessor(invalidDrop, invalidCheck) ||
        invalidDrop->size() != 2)
        return reject("Native v3 host invalid owner bypasses Drop");
    const auto* dropCall = llvm::dyn_cast<llvm::CallInst>(
        &invalidDrop->front());
    const auto* dropBranch = llvm::dyn_cast<llvm::BranchInst>(
        invalidDrop->getTerminator());
    if (!dropCall || dropCall->getCalledFunction() != drop ||
        dropCall->getCallingConv() != llvm::CallingConv::C ||
        dropCall->arg_size() != 1 ||
        dropCall->getArgOperand(0) != ownerCell ||
        !dropBranch || !dropBranch->isUnconditional() ||
        dropBranch->getSuccessor(0) != invalidReturn)
        return reject("Native v3 host invalid owner changed its local Drop");
    const auto* invalidStatusReturn = invalidReturn->size() == 1
        ? llvm::dyn_cast<llvm::ReturnInst>(
            invalidReturn->getTerminator()) : nullptr;
    const auto* invalidStatus = invalidStatusReturn
        ? llvm::dyn_cast_or_null<llvm::ConstantInt>(
            invalidStatusReturn->getReturnValue()) : nullptr;
    if (!invalidStatus || invalidStatus->getZExtValue() !=
            LUNA_NATIVE_REF_RESULT_OWNER_INVALID_RESOURCE_V1 ||
        std::distance(llvm::pred_begin(invalidReturn),
                      llvm::pred_end(invalidReturn)) != 3)
        return reject("Native v3 host invalid result changed its status");
    for (const auto* predecessor : llvm::predecessors(invalidReturn))
        if (predecessor != resource && predecessor != invalidCheck &&
            predecessor != invalidDrop)
            return reject("Native v3 host invalid result has another ingress");
    error.clear();
    return true;
}

bool verifyNativeOwnedResultHostClosedGraph(
    const llvm::Module& generated, const std::string& hostLinkageName,
    const std::string& transferLinkageName,
    const std::string& dropLinkageName, std::string& error) {
    if (!verifyNativeOwnedResultHostResourceValidation(
            generated, hostLinkageName, transferLinkageName,
            dropLinkageName, error) ||
        !verifyNativeOwnedResultHostOwnerLeaseGates(
            generated, hostLinkageName, transferLinkageName, error) ||
        !verifyNativeOwnedResultHostOutputDisjointness(
            generated, hostLinkageName, error))
        return false;
    const auto reject = [&](const char* reason) {
        error = reason;
        return false;
    };
    const auto* host = generated.getFunction(hostLinkageName);
    const auto* transfer = generated.getFunction(transferLinkageName);
    const auto* drop = generated.getFunction(dropLinkageName);
    const auto* adoption = generated.getFunction(
        "luna_runtime_owned_result_adopt_v1");
    const auto* lease = generated.getFunction(
        "luna_runtime_owned_result_lease_check_v1");
    const llvm::CallInst* transferCall = nullptr;
    for (const auto& block : *host)
        for (const auto& instruction : block)
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                    &instruction); call && call->getCalledFunction() == transfer)
                transferCall = call;
    const std::array<const llvm::Value*, 3> scratch = {
        transferCall->getArgOperand(2), transferCall->getArgOperand(3),
        transferCall->getArgOperand(4)};
    if (host->size() != 18)
        return reject("Native v3 host graph changed its block count");
    llvm::SmallPtrSet<const llvm::BasicBlock*, 32> reached;
    std::vector<const llvm::BasicBlock*> pending = {&host->getEntryBlock()};
    reached.insert(pending.front());
    while (!pending.empty()) {
        const auto* block = pending.back();
        pending.pop_back();
        for (const auto* successor : llvm::successors(block))
            if (reached.insert(successor).second)
                pending.push_back(successor);
    }
    if (reached.size() != host->size())
        return reject("Native v3 host graph has an unreachable block");
    unsigned allocas = 0;
    unsigned loads = 0;
    unsigned ownerInitializations = 0;
    unsigned scalarStores = 0;
    unsigned tagStores = 0;
    unsigned leaseCalls = 0;
    unsigned transferCalls = 0;
    unsigned adoptCalls = 0;
    unsigned dropCalls = 0;
    unsigned returns = 0;
    unsigned branches = 0;
    for (const auto& block : *host)
        for (const auto& instruction : block) {
            if (const auto* allocated = llvm::dyn_cast<llvm::AllocaInst>(
                    &instruction)) {
                const auto* size = llvm::dyn_cast<llvm::ConstantInt>(
                    allocated->getArraySize());
                if (allocated->getParent() != &host->getEntryBlock() ||
                    std::find(scratch.begin(), scratch.end(), allocated) ==
                        scratch.end() ||
                    !size || !size->isOne())
                    return reject("Native v3 host graph changed its scratch allocation");
                ++allocas;
            } else if (const auto* loaded = llvm::dyn_cast<llvm::LoadInst>(
                           &instruction)) {
                if (loaded->isVolatile() || loaded->isAtomic())
                    return reject("Native v3 host graph added a volatile or atomic load");
                ++loads;
            } else if (const auto* stored = llvm::dyn_cast<llvm::StoreInst>(
                           &instruction)) {
                if (stored->isVolatile() || stored->isAtomic())
                    return reject("Native v3 host graph added a volatile or atomic store");
                const auto* destination = stored->getPointerOperand();
                if (destination == scratch[2]) ++ownerInitializations;
                else if (destination == host->getArg(3)) ++scalarStores;
                else if (destination == host->getArg(2)) ++tagStores;
                else return reject("Native v3 host graph added an output store");
            } else if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                           &instruction)) {
                const auto* callee = call->getCalledFunction();
                if (callee == lease) ++leaseCalls;
                else if (callee == transfer) ++transferCalls;
                else if (callee == adoption) ++adoptCalls;
                else if (callee == drop) ++dropCalls;
                else return reject("Native v3 host graph added an unrecognized call");
            } else if (llvm::isa<llvm::ReturnInst>(&instruction)) {
                ++returns;
            } else if (llvm::isa<llvm::BranchInst>(&instruction)) {
                ++branches;
            } else if (instruction.mayHaveSideEffects()) {
                return reject("Native v3 host graph added a side effect");
            }
        }
    if (allocas != 3 || loads != 4 || ownerInitializations != 1 ||
        scalarStores != 1 || tagStores != 2 || leaseCalls != 1 ||
        transferCalls != 1 || adoptCalls != 1 || dropCalls != 2 ||
        returns != 7 || branches != 11)
        return reject("Native v3 host graph changed its effects or exits");
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

bool verifyNativeOwnedResultHostOutputDisjointness(
    const llvm::Module& generated, const std::string& hostLinkageName,
    std::string& error) {
    if (!verifyNativeOwnedResultHostOutputAlignment(
            generated, hostLinkageName, error))
        return false;
    const auto* host = generated.getFunction(hostLinkageName);
    const auto* condition = llvm::cast<llvm::BranchInst>(
        host->getEntryBlock().getTerminator())->getCondition();
    struct OutputPair {
        unsigned left;
        uint64_t leftSize;
        unsigned right;
        uint64_t rightSize;
    };
    const std::array<OutputPair, 3> pairs = {{
        {2, sizeof(uint32_t), 3, sizeof(int32_t)},
        {2, sizeof(uint32_t), 4, sizeof(void*)},
        {3, sizeof(int32_t), 4, sizeof(void*)},
    }};
    for (const auto& pair : pairs)
        if (!hasRequiredDisjointConjunct(
                condition, host->getArg(pair.left), pair.leftSize,
                host->getArg(pair.right), pair.rightSize)) {
            error = "Native v3 host output check lost a disjointness guard";
            return false;
        }
    for (const auto& instruction : host->getEntryBlock())
        if (!llvm::isa<llvm::AllocaInst>(&instruction) &&
            !llvm::isa<llvm::PtrToIntInst>(&instruction) &&
            !llvm::isa<llvm::ICmpInst>(&instruction) &&
            !llvm::isa<llvm::BinaryOperator>(&instruction) &&
            !llvm::isa<llvm::SelectInst>(&instruction) &&
            !llvm::isa<llvm::BranchInst>(&instruction)) {
            error = "Native v3 host output check accesses memory before preflight";
            return false;
        }
    error.clear();
    return true;
}

bool verifyNativeOwnedResultTransferInjectedCleanup(
    const llvm::Module& generated, const std::string& transferLinkageName,
    const NativeOwnedResultSourceFacts& source,
    const std::vector<std::string>& expectedCleanupCalls,
    uint64_t allocationSize, uint64_t allocationAlignment,
    uint32_t invalidResourceStatus, uint32_t injectedFailureStatus,
    std::string& error) {
    const auto reject = [&](const char* reason) {
        error = reason;
        return false;
    };
    const auto* transfer = generated.getFunction(transferLinkageName);
    const auto* body = generated.getFunction(source.sourceLinkageName);
    if (!transfer || transfer->isDeclaration() || transfer->isVarArg() ||
        transfer->getCallingConv() != llvm::CallingConv::C ||
        !transfer->getReturnType()->isIntegerTy(32) ||
        transfer->arg_size() != 6 || !transfer->hasExternalLinkage() ||
        !body || body->isDeclaration() || !body->hasInternalLinkage() ||
        allocationSize == 0 || allocationSize > (1u << 20) ||
        allocationAlignment == 0 || allocationAlignment > 4096 ||
        (allocationAlignment & (allocationAlignment - 1)) != 0 ||
        expectedCleanupCalls.size() < 2 ||
        expectedCleanupCalls.front() == "rt_dealloc" ||
        expectedCleanupCalls.back() != "rt_dealloc")
        return reject("Native v3 transfer cleanup lacks its source profile");
    for (unsigned index = 0; index < 5; ++index)
        if (!transfer->getArg(index)->getType()->isPointerTy() ||
            transfer->getArg(index)->getType()->getPointerAddressSpace() != 0)
            return reject("Native v3 transfer cleanup changed a pointer parameter");
    if (!transfer->getArg(5)->getType()->isIntegerTy(32))
        return reject("Native v3 transfer cleanup changed its injection parameter");
    const llvm::CallInst* bodyCall = nullptr;
    const llvm::IntToPtrInst* returnedOwner = nullptr;
    for (const auto& block : *transfer)
        for (const auto& instruction : block) {
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                    &instruction); call && call->getCalledFunction() == body) {
                if (bodyCall)
                    return reject("Native v3 transfer cleanup calls the body twice");
                bodyCall = call;
            }
            if (const auto* owner = llvm::dyn_cast<llvm::IntToPtrInst>(
                    &instruction)) {
                const auto* extracted = llvm::dyn_cast<llvm::ExtractValueInst>(
                    owner->getOperand(0));
                if (extracted && extracted->getNumIndices() == 2 &&
                    extracted->getIndices()[0] == 1 &&
                    extracted->getIndices()[1] == 0) {
                    if (returnedOwner)
                        return reject("Native v3 transfer cleanup has ambiguous owner");
                    returnedOwner = owner;
                }
            }
        }
    const auto* extracted = returnedOwner
        ? llvm::dyn_cast<llvm::ExtractValueInst>(
            returnedOwner->getOperand(0)) : nullptr;
    if (!bodyCall || !returnedOwner || !extracted ||
        extracted->getAggregateOperand() != bodyCall ||
        returnedOwner->getType() != transfer->getArg(4)->getType())
        return reject("Native v3 transfer cleanup lost the returned owner");
    const auto* resource = returnedOwner->getParent();
    const auto* resourceBranch = llvm::dyn_cast<llvm::BranchInst>(
        resource->getTerminator());
    const auto* nonnull = resourceBranch && resourceBranch->isConditional()
        ? llvm::dyn_cast<llvm::ICmpInst>(
            resourceBranch->getCondition()) : nullptr;
    if (!nonnull || nonnull->getPredicate() != llvm::ICmpInst::ICMP_NE ||
        !((nonnull->getOperand(0) == returnedOwner &&
           llvm::isa<llvm::ConstantPointerNull>(nonnull->getOperand(1))) ||
          (nonnull->getOperand(1) == returnedOwner &&
           llvm::isa<llvm::ConstantPointerNull>(nonnull->getOperand(0)))))
        return reject("Native v3 transfer cleanup lost its owner gate");
    const auto* valid = resourceBranch->getSuccessor(0);
    const auto* nullResource = resourceBranch->getSuccessor(1);
    const auto statusReturn = [](const llvm::BasicBlock* block,
                                 uint64_t status) {
        const auto* returned = block->size() == 1
            ? llvm::dyn_cast<llvm::ReturnInst>(
                block->getTerminator()) : nullptr;
        const auto* constant = returned
            ? llvm::dyn_cast_or_null<llvm::ConstantInt>(
                returned->getReturnValue()) : nullptr;
        return constant && constant->getBitWidth() == 32 &&
            constant->getZExtValue() == status;
    };
    if (!statusReturn(nullResource, invalidResourceStatus) ||
        std::distance(llvm::pred_begin(nullResource),
                      llvm::pred_end(nullResource)) != 1 ||
        *llvm::pred_begin(nullResource) != resource ||
        std::distance(llvm::pred_begin(valid),
                      llvm::pred_end(valid)) != 1 ||
        *llvm::pred_begin(valid) != resource || valid->size() != 2)
        return reject("Native v3 transfer cleanup bypasses its owner gate");
    const auto* injected = llvm::dyn_cast<llvm::ICmpInst>(&valid->front());
    const auto* injectionBranch = llvm::dyn_cast<llvm::BranchInst>(
        valid->getTerminator());
    if (!injected || injected->getPredicate() !=
            llvm::ICmpInst::ICMP_NE ||
        !((injected->getOperand(0) == transfer->getArg(5) &&
           llvm::isa<llvm::ConstantInt>(injected->getOperand(1)) &&
           llvm::cast<llvm::ConstantInt>(
               injected->getOperand(1))->isZero()) ||
          (injected->getOperand(1) == transfer->getArg(5) &&
           llvm::isa<llvm::ConstantInt>(injected->getOperand(0)) &&
           llvm::cast<llvm::ConstantInt>(
               injected->getOperand(0))->isZero())) ||
        !injectionBranch || !injectionBranch->isConditional() ||
        injectionBranch->getCondition() != injected)
        return reject("Native v3 transfer cleanup lost its injection gate");
    const auto* failed = injectionBranch->getSuccessor(0);
    if (std::distance(llvm::pred_begin(failed),
                      llvm::pred_end(failed)) != 1 ||
        *llvm::pred_begin(failed) != valid ||
        !llvm::isa<llvm::ReturnInst>(failed->getTerminator()))
        return reject("Native v3 transfer cleanup bypasses its failure edge");
    const auto* failedReturn = llvm::cast<llvm::ReturnInst>(
        failed->getTerminator());
    const auto* failedStatus = llvm::dyn_cast_or_null<llvm::ConstantInt>(
        failedReturn->getReturnValue());
    if (!failedStatus || failedStatus->getBitWidth() != 32 ||
        failedStatus->getZExtValue() != injectedFailureStatus)
        return reject("Native v3 transfer cleanup changed its failure status");
    std::vector<const llvm::CallInst*> calls;
    for (const auto& instruction : *failed) {
        if (llvm::isa<llvm::StoreInst>(instruction) ||
            llvm::isa<llvm::AllocaInst>(instruction))
            return reject("Native v3 transfer cleanup added scratch mutation");
        if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                &instruction)) {
            if (!call->getCalledFunction() ||
                calls.size() == expectedCleanupCalls.size() ||
                call->getCalledFunction()->getName() !=
                    expectedCleanupCalls[calls.size()])
                return reject("Native v3 transfer cleanup changed Drop order");
            for (unsigned index = 0; index < call->arg_size(); ++index)
                if (call->getArgOperand(index) == transfer->getArg(2) ||
                    call->getArgOperand(index) == transfer->getArg(3) ||
                    call->getArgOperand(index) == transfer->getArg(4))
                    return reject("Native v3 transfer cleanup passed an output cell");
            calls.push_back(call);
        } else if (!llvm::isa<llvm::ReturnInst>(instruction) &&
                   instruction.mayHaveSideEffects())
            return reject("Native v3 transfer cleanup added a side effect");
    }
    if (calls.size() != expectedCleanupCalls.size() ||
        calls.front()->arg_size() < 1 ||
        calls.front()->getArgOperand(0) != returnedOwner)
        return reject("Native v3 transfer cleanup lost the root Drop owner");
    const auto* deallocate = calls.back();
    const auto* deallocateFunction = deallocate->getCalledFunction();
    const auto* size = deallocate->arg_size() == 3
        ? llvm::dyn_cast<llvm::ConstantInt>(
            deallocate->getArgOperand(1)) : nullptr;
    const auto* alignment = deallocate->arg_size() == 3
        ? llvm::dyn_cast<llvm::ConstantInt>(
            deallocate->getArgOperand(2)) : nullptr;
    if (!deallocateFunction || !deallocateFunction->isDeclaration() ||
        deallocateFunction->getCallingConv() != llvm::CallingConv::C ||
        !deallocateFunction->getReturnType()->isVoidTy() ||
        deallocate->getCallingConv() != llvm::CallingConv::C ||
        deallocate->arg_size() != 3)
        return reject("Native v3 transfer cleanup changed deallocation arity");
    if (deallocate->getArgOperand(0) != returnedOwner)
        return reject("Native v3 transfer cleanup changed deallocation owner");
    if (!size || !alignment ||
        size->getBitWidth() != sizeof(void*) * 8 ||
        alignment->getBitWidth() != sizeof(void*) * 8)
        return reject("Native v3 transfer cleanup changed deallocation type");
    if (size->getZExtValue() != allocationSize ||
        alignment->getZExtValue() != allocationAlignment) {
        error = "Native v3 transfer cleanup changed deallocation layout: " +
            std::to_string(size->getZExtValue()) + "/" +
            std::to_string(alignment->getZExtValue()) + " versus " +
            std::to_string(allocationSize) + "/" +
            std::to_string(allocationAlignment);
        return false;
    }
    error.clear();
    return true;
}

bool verifyNativeOwnedResultTransferCleanupProvenance(
    const llvm::Module& generated, const std::string& transferLinkageName,
    const NativeOwnedResultSourceFacts& source,
    const std::vector<std::string>& expectedCleanupCalls,
    const std::vector<NativeOwnedResultCleanupNode>& cleanupNodes,
    uint32_t invalidResourceStatus, uint32_t injectedFailureStatus,
    std::string& error) {
    const auto reject = [&](const char* reason) {
        error = reason;
        return false;
    };
    if (cleanupNodes.empty() || cleanupNodes.size() > 3 ||
        expectedCleanupCalls.size() != cleanupNodes.size() * 2 ||
        cleanupNodes.front().parentIndex != 0 ||
        cleanupNodes.front().fieldOffset != 0)
        return reject("Native v3 transfer cleanup lacks a bounded owner tree");
    for (size_t index = 0; index < cleanupNodes.size(); ++index) {
        const auto& node = cleanupNodes[index];
        if (node.dropLinkageName.empty() ||
            node.dropLinkageName == "rt_dealloc" ||
            node.allocationSize == 0 || node.allocationSize > (1u << 20) ||
            node.allocationAlignment == 0 ||
            node.allocationAlignment > 4096 ||
            (node.allocationAlignment & (node.allocationAlignment - 1)) != 0)
            return reject("Native v3 transfer cleanup changed an owner layout");
        if (index == 0) continue;
        if (node.parentIndex >= index ||
            cleanupNodes[node.parentIndex].allocationSize < sizeof(void*) ||
            node.fieldOffset >
                cleanupNodes[node.parentIndex].allocationSize - sizeof(void*) ||
            node.fieldOffset % alignof(void*) != 0)
            return reject("Native v3 transfer cleanup changed a nested field offset");
    }
    if (!verifyNativeOwnedResultTransferInjectedCleanup(
            generated, transferLinkageName, source, expectedCleanupCalls,
            cleanupNodes.front().allocationSize,
            cleanupNodes.front().allocationAlignment,
            invalidResourceStatus, injectedFailureStatus, error))
        return false;
    const auto* transfer = generated.getFunction(transferLinkageName);
    const auto* body = generated.getFunction(source.sourceLinkageName);
    const llvm::CallInst* bodyCall = nullptr;
    for (const auto& block : *transfer)
        for (const auto& instruction : block)
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                    &instruction); call && call->getCalledFunction() == body)
                bodyCall = call;
    const auto* dispatch = llvm::cast<llvm::BranchInst>(
        bodyCall->getParent()->getTerminator());
    const auto* resource = llvm::cast<llvm::BranchInst>(
        dispatch->getSuccessor(1)->getTerminator());
    const auto* valid = llvm::cast<llvm::BranchInst>(
        resource->getSuccessor(0)->getTerminator());
    const auto* cleanup = valid->getSuccessor(0);
    std::vector<const llvm::CallInst*> calls;
    for (const auto& instruction : *cleanup)
        if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction))
            calls.push_back(call);
    std::vector<const llvm::Value*> owners(cleanupNodes.size());
    std::vector<size_t> active;
    size_t nextNode = 0;
    for (const auto* call : calls) {
        const auto* callee = call->getCalledFunction();
        if (!callee)
            return reject("Native v3 transfer cleanup changed a Drop call");
        if (callee->getName() == "rt_dealloc") {
            if (active.empty())
                return reject("Native v3 transfer cleanup deallocates without an owner");
            const size_t index = active.back();
            const auto& node = cleanupNodes[index];
            const auto* size = call->arg_size() == 3
                ? llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(1))
                : nullptr;
            const auto* alignment = call->arg_size() == 3
                ? llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(2))
                : nullptr;
            if (!callee->isDeclaration() ||
                callee->isVarArg() || callee->arg_size() != 3 ||
                !callee->getFunctionType()->getParamType(0)->isPointerTy() ||
                !callee->getFunctionType()->getParamType(1)->isIntegerTy(
                    sizeof(void*) * 8) ||
                !callee->getFunctionType()->getParamType(2)->isIntegerTy(
                    sizeof(void*) * 8) ||
                callee->getCallingConv() != llvm::CallingConv::C ||
                !callee->getReturnType()->isVoidTy() ||
                call->getCallingConv() != llvm::CallingConv::C ||
                call->arg_size() != 3 ||
                call->getArgOperand(0) != owners[index] ||
                !size || !alignment ||
                size->getBitWidth() != sizeof(void*) * 8 ||
                alignment->getBitWidth() != sizeof(void*) * 8 ||
                size->getZExtValue() != node.allocationSize ||
                alignment->getZExtValue() != node.allocationAlignment)
                return reject("Native v3 transfer cleanup changed nested deallocation");
            active.pop_back();
            continue;
        }
        if (nextNode == cleanupNodes.size() ||
            callee->getName() != cleanupNodes[nextNode].dropLinkageName ||
            callee->isVarArg() || callee->arg_size() != 1 ||
            !callee->getFunctionType()->getParamType(0)->isPointerTy() ||
            !callee->getReturnType()->isVoidTy() ||
            callee->getCallingConv() != llvm::CallingConv::C ||
            call->getCallingConv() != llvm::CallingConv::C ||
            call->arg_size() != 1)
            return reject("Native v3 transfer cleanup changed nested Drop order");
        const size_t index = nextNode++;
        const auto* owner = call->getArgOperand(0);
        if (index == 0) {
            if (!active.empty())
                return reject("Native v3 transfer cleanup changed root ownership");
        } else {
            if (active.empty() ||
                cleanupNodes[index].parentIndex != active.back())
                return reject("Native v3 transfer cleanup changed nested ownership");
            const auto* loaded = llvm::dyn_cast<llvm::LoadInst>(owner);
            if (!loaded || loaded->getParent() != cleanup ||
                !loaded->getType()->isPointerTy() ||
                loaded->isVolatile() || loaded->isAtomic())
                return reject("Native v3 transfer cleanup lost a nested owner load");
            const auto* field = loaded->getPointerOperand();
            const auto offset = cleanupNodes[index].fieldOffset;
            if (offset == 0) {
                if (field != owners[active.back()])
                    return reject("Native v3 transfer cleanup changed a zero-offset owner field");
            } else {
                const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(
                    field);
                const auto* indexValue = gep && gep->getNumIndices() == 1
                    ? llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(1))
                    : nullptr;
                if (!gep || gep->getParent() != cleanup ||
                    !gep->getSourceElementType()->isIntegerTy(8) ||
                    gep->getPointerOperand() != owners[active.back()] ||
                    !indexValue ||
                    indexValue->getBitWidth() != sizeof(void*) * 8 ||
                    indexValue->getZExtValue() != offset)
                    return reject("Native v3 transfer cleanup changed a nested owner field");
            }
        }
        owners[index] = owner;
        active.push_back(index);
    }
    if (nextNode != cleanupNodes.size() || !active.empty())
        return reject("Native v3 transfer cleanup left an owner unaccounted");
    error.clear();
    return true;
}

bool verifyNativeOwnedResultTransferScalarFork(
    const llvm::Module& generated, const std::string& transferLinkageName,
    const NativeOwnedResultSourceFacts& source,
    uint32_t injectedFailureStatus, uint32_t successStatus,
    std::string& error) {
    const auto reject = [&](const char* reason) {
        error = reason;
        return false;
    };
    const auto* transfer = generated.getFunction(transferLinkageName);
    const auto* body = generated.getFunction(source.sourceLinkageName);
    if (!transfer || transfer->isDeclaration() || transfer->isVarArg() ||
        transfer->getCallingConv() != llvm::CallingConv::C ||
        !transfer->getReturnType()->isIntegerTy(32) ||
        transfer->arg_size() != 6 || !transfer->hasExternalLinkage() ||
        !body || body->isDeclaration() || !body->hasInternalLinkage() ||
        !transfer->getArg(5)->getType()->isIntegerTy(32))
        return reject("Native v3 scalar transfer lacks its source profile");
    for (unsigned index = 0; index < 5; ++index)
        if (!transfer->getArg(index)->getType()->isPointerTy() ||
            transfer->getArg(index)->getType()->getPointerAddressSpace() != 0)
            return reject(
                "Native v3 scalar transfer changed a pointer parameter");
    const llvm::CallInst* bodyCall = nullptr;
    for (const auto& block : *transfer)
        for (const auto& instruction : block)
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                    &instruction); call && call->getCalledFunction() == body) {
                if (bodyCall)
                    return reject("Native v3 scalar transfer calls the body twice");
                bodyCall = call;
            }
    if (!bodyCall)
        return reject("Native v3 scalar transfer lost its body call");
    const auto* dispatch = bodyCall->getParent();
    const auto* resultBranch = llvm::dyn_cast<llvm::BranchInst>(
        dispatch->getTerminator());
    const auto* tag = resultBranch && resultBranch->isConditional()
        ? llvm::dyn_cast<llvm::ExtractValueInst>(
            resultBranch->getCondition()) : nullptr;
    if (!resultBranch || !resultBranch->isConditional() ||
        dispatch->size() != 3 || !tag || tag->getNumIndices() != 1 ||
        tag->getIndices()[0] != 0 ||
        tag->getAggregateOperand() != bodyCall ||
        !tag->getType()->isIntegerTy(1))
        return reject("Native v3 scalar transfer lost its body tag fork");
    const auto* scalar = resultBranch->getSuccessor(0);
    if (scalar == resultBranch->getSuccessor(1) ||
        std::distance(llvm::pred_begin(scalar),
                      llvm::pred_end(scalar)) != 1 ||
        *llvm::pred_begin(scalar) != dispatch)
        return reject("Native v3 scalar transfer bypasses body tag dispatch");
    if (scalar->size() != 2) {
        error = "Native v3 scalar transfer has " +
            std::to_string(scalar->size()) + " gate instructions";
        for (const auto& instruction : *scalar)
            error += " " + std::string(instruction.getOpcodeName());
        return false;
    }
    const auto* injected = llvm::dyn_cast<llvm::ICmpInst>(&scalar->front());
    const auto* injectionBranch = llvm::dyn_cast<llvm::BranchInst>(
        scalar->getTerminator());
    if (!injected || injected->getPredicate() != llvm::ICmpInst::ICMP_NE ||
        !((injected->getOperand(0) == transfer->getArg(5) &&
           llvm::isa<llvm::ConstantInt>(injected->getOperand(1)) &&
           llvm::cast<llvm::ConstantInt>(
               injected->getOperand(1))->isZero()) ||
          (injected->getOperand(1) == transfer->getArg(5) &&
           llvm::isa<llvm::ConstantInt>(injected->getOperand(0)) &&
           llvm::cast<llvm::ConstantInt>(
               injected->getOperand(0))->isZero())) ||
        !injectionBranch || !injectionBranch->isConditional() ||
        injectionBranch->getCondition() != injected)
        return reject("Native v3 scalar transfer lost its injection gate");
    const auto* failed = injectionBranch->getSuccessor(0);
    const auto* commit = injectionBranch->getSuccessor(1);
    const auto returnsStatus = [](const llvm::BasicBlock* block,
                                  uint32_t status) {
        const auto* returned = llvm::dyn_cast<llvm::ReturnInst>(
            block->getTerminator());
        const auto* value = returned
            ? llvm::dyn_cast_or_null<llvm::ConstantInt>(
                returned->getReturnValue()) : nullptr;
        return value && value->getBitWidth() == 32 &&
            value->getZExtValue() == status;
    };
    if (failed == commit ||
        std::distance(llvm::pred_begin(failed),
                      llvm::pred_end(failed)) != 1 ||
        *llvm::pred_begin(failed) != scalar || failed->size() != 1 ||
        !returnsStatus(failed, injectedFailureStatus))
        return reject("Native v3 scalar transfer changed injected failure");
    if (std::distance(llvm::pred_begin(commit),
                      llvm::pred_end(commit)) != 1 ||
        *llvm::pred_begin(commit) != scalar ||
        !returnsStatus(commit, successStatus))
        return reject("Native v3 scalar transfer bypasses success commit");
    std::vector<const llvm::StoreInst*> stores;
    for (const auto& instruction : *commit) {
        if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(
                &instruction))
            stores.push_back(store);
        else if (!llvm::isa<llvm::ExtractValueInst>(instruction) &&
                 !llvm::isa<llvm::CastInst>(instruction) &&
                 !llvm::isa<llvm::ReturnInst>(instruction))
            return reject("Native v3 scalar transfer added a commit effect");
    }
    if (stores.size() != 2 ||
        stores[0]->getPointerOperand() != transfer->getArg(2) ||
        stores[1]->getPointerOperand() != transfer->getArg(3))
        return reject("Native v3 scalar transfer changed output cells");
    const auto* scalarTag = llvm::dyn_cast<llvm::ConstantInt>(
        stores[0]->getValueOperand());
    if (!scalarTag || scalarTag->getBitWidth() != 32 ||
        scalarTag->getZExtValue() != 1)
        return reject("Native v3 scalar transfer changed the Ok tag");
    const llvm::Value* scalarValue = stores[1]->getValueOperand();
    if (const auto* cast = llvm::dyn_cast<llvm::CastInst>(scalarValue)) {
        if (cast->getOpcode() != llvm::Instruction::Trunc &&
            cast->getOpcode() != llvm::Instruction::SExt)
            return reject("Native v3 scalar transfer changed signed payload cast");
        scalarValue = cast->getOperand(0);
    }
    const auto* payload = llvm::dyn_cast<llvm::ExtractValueInst>(scalarValue);
    if (!stores[1]->getValueOperand()->getType()->isIntegerTy(32) ||
        !payload || payload->getNumIndices() != 2 ||
        payload->getIndices()[0] != 1 || payload->getIndices()[1] != 0 ||
        payload->getAggregateOperand() != bodyCall)
        return reject("Native v3 scalar transfer lost the body payload");
    error.clear();
    return true;
}

bool verifyNativeOwnedResultTransferResourceCommit(
    const llvm::Module& generated, const std::string& transferLinkageName,
    const NativeOwnedResultSourceFacts& source,
    uint32_t successStatus, std::string& error) {
    const auto reject = [&](const char* reason) {
        error = reason;
        return false;
    };
    const auto* transfer = generated.getFunction(transferLinkageName);
    const auto* body = generated.getFunction(source.sourceLinkageName);
    if (!transfer || transfer->isDeclaration() || transfer->isVarArg() ||
        transfer->getCallingConv() != llvm::CallingConv::C ||
        !transfer->getReturnType()->isIntegerTy(32) ||
        transfer->arg_size() != 6 || !transfer->hasExternalLinkage() ||
        !body || body->isDeclaration() || !body->hasInternalLinkage() ||
        !transfer->getArg(5)->getType()->isIntegerTy(32))
        return reject("Native v3 resource commit lacks its source profile");
    for (unsigned index = 0; index < 5; ++index)
        if (!transfer->getArg(index)->getType()->isPointerTy() ||
            transfer->getArg(index)->getType()->getPointerAddressSpace() != 0)
            return reject("Native v3 resource commit changed a pointer parameter");
    const llvm::CallInst* bodyCall = nullptr;
    for (const auto& block : *transfer)
        for (const auto& instruction : block)
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                    &instruction); call && call->getCalledFunction() == body) {
                if (bodyCall)
                    return reject("Native v3 resource commit calls the body twice");
                bodyCall = call;
            }
    if (!bodyCall)
        return reject("Native v3 resource commit lost the body call");
    const auto* dispatch = bodyCall->getParent();
    const auto* bodyBranch = llvm::dyn_cast<llvm::BranchInst>(
        dispatch->getTerminator());
    const auto* tag = bodyBranch && bodyBranch->isConditional()
        ? llvm::dyn_cast<llvm::ExtractValueInst>(
            bodyBranch->getCondition()) : nullptr;
    if (!bodyBranch || !bodyBranch->isConditional() ||
        dispatch->size() != 3 || !tag || tag->getNumIndices() != 1 ||
        tag->getIndices()[0] != 0 ||
        tag->getAggregateOperand() != bodyCall ||
        !tag->getType()->isIntegerTy(1))
        return reject("Native v3 resource commit lost its body tag fork");
    const auto* resource = bodyBranch->getSuccessor(1);
    if (resource == bodyBranch->getSuccessor(0) ||
        std::distance(llvm::pred_begin(resource),
                      llvm::pred_end(resource)) != 1 ||
        *llvm::pred_begin(resource) != dispatch || resource->size() != 4)
        return reject("Native v3 resource commit bypasses body tag dispatch");
    const auto* extracted = llvm::dyn_cast<llvm::ExtractValueInst>(
        &resource->front());
    const auto* owner = extracted
        ? llvm::dyn_cast<llvm::IntToPtrInst>(extracted->getNextNode())
        : nullptr;
    const auto* nonnull = owner
        ? llvm::dyn_cast<llvm::ICmpInst>(owner->getNextNode()) : nullptr;
    const auto* ownerBranch = llvm::dyn_cast<llvm::BranchInst>(
        resource->getTerminator());
    if (!extracted || extracted->getNumIndices() != 2 ||
        extracted->getIndices()[0] != 1 ||
        extracted->getIndices()[1] != 0 ||
        extracted->getAggregateOperand() != bodyCall ||
        !owner || owner->getOperand(0) != extracted ||
        owner->getType() != transfer->getArg(4)->getType() ||
        !nonnull || nonnull->getPredicate() != llvm::ICmpInst::ICMP_NE ||
        !((nonnull->getOperand(0) == owner &&
           llvm::isa<llvm::ConstantPointerNull>(nonnull->getOperand(1))) ||
          (nonnull->getOperand(1) == owner &&
           llvm::isa<llvm::ConstantPointerNull>(nonnull->getOperand(0)))) ||
        !ownerBranch || !ownerBranch->isConditional() ||
        ownerBranch->getCondition() != nonnull)
        return reject("Native v3 resource commit lost the returned owner gate");
    const auto* valid = ownerBranch->getSuccessor(0);
    if (valid == ownerBranch->getSuccessor(1) ||
        std::distance(llvm::pred_begin(valid),
                      llvm::pred_end(valid)) != 1 ||
        *llvm::pred_begin(valid) != resource || valid->size() != 2)
        return reject("Native v3 resource commit bypasses owner validation");
    const auto* injected = llvm::dyn_cast<llvm::ICmpInst>(&valid->front());
    const auto* injectionBranch = llvm::dyn_cast<llvm::BranchInst>(
        valid->getTerminator());
    if (!injected || injected->getPredicate() != llvm::ICmpInst::ICMP_NE ||
        !((injected->getOperand(0) == transfer->getArg(5) &&
           llvm::isa<llvm::ConstantInt>(injected->getOperand(1)) &&
           llvm::cast<llvm::ConstantInt>(
               injected->getOperand(1))->isZero()) ||
          (injected->getOperand(1) == transfer->getArg(5) &&
           llvm::isa<llvm::ConstantInt>(injected->getOperand(0)) &&
           llvm::cast<llvm::ConstantInt>(
               injected->getOperand(0))->isZero())) ||
        !injectionBranch || !injectionBranch->isConditional() ||
        injectionBranch->getCondition() != injected)
        return reject("Native v3 resource commit lost its injection gate");
    const auto* commit = injectionBranch->getSuccessor(1);
    if (commit == injectionBranch->getSuccessor(0) ||
        std::distance(llvm::pred_begin(commit),
                      llvm::pred_end(commit)) != 1 ||
        *llvm::pred_begin(commit) != valid || commit->size() != 3)
        return reject("Native v3 resource commit bypasses its success edge");
    const auto* tagStore = llvm::dyn_cast<llvm::StoreInst>(&commit->front());
    const auto* ownerStore = tagStore
        ? llvm::dyn_cast<llvm::StoreInst>(tagStore->getNextNode()) : nullptr;
    const auto* returned = llvm::dyn_cast<llvm::ReturnInst>(
        commit->getTerminator());
    const auto* tagValue = tagStore
        ? llvm::dyn_cast<llvm::ConstantInt>(tagStore->getValueOperand())
        : nullptr;
    const auto* status = returned
        ? llvm::dyn_cast_or_null<llvm::ConstantInt>(
            returned->getReturnValue()) : nullptr;
    if (!tagStore || tagStore->getPointerOperand() != transfer->getArg(2) ||
        !tagValue || tagValue->getBitWidth() != 32 || !tagValue->isZero() ||
        !ownerStore || ownerStore->getPointerOperand() !=
            transfer->getArg(4) ||
        ownerStore->getValueOperand() != owner ||
        !status || status->getBitWidth() != 32 ||
        status->getZExtValue() != successStatus)
        return reject("Native v3 resource commit changed tag, owner or status");
    error.clear();
    return true;
}

bool verifyNativeOwnedResultTransferContextRefPreflight(
    const llvm::Module& generated, const std::string& transferLinkageName,
    const NativeOwnedResultSourceFacts& source,
    uint32_t invalidContextStatus, uint32_t invalidHandleStatus,
    uint32_t invalidTargetStatus, uint32_t unexpectedStatus,
    std::string& error) {
    const auto reject = [&](const char* reason) {
        error = reason;
        return false;
    };
    const auto* transfer = generated.getFunction(transferLinkageName);
    const auto* body = generated.getFunction(source.sourceLinkageName);
    if (!transfer || transfer->isDeclaration() || transfer->isVarArg() ||
        transfer->getCallingConv() != llvm::CallingConv::C ||
        !transfer->getReturnType()->isIntegerTy(32) ||
        transfer->arg_size() != 6 || !transfer->hasExternalLinkage() ||
        !body || body->isDeclaration() || !body->hasInternalLinkage() ||
        !boundedIdentity(source.refSlotSymbolId) ||
        !boundedIdentity(source.refSlotContractId))
        return reject("Native v3 transfer preflight lacks source identities");
    for (unsigned index = 0; index < 5; ++index)
        if (!transfer->getArg(index)->getType()->isPointerTy() ||
            transfer->getArg(index)->getType()->getPointerAddressSpace() != 0)
            return reject("Native v3 transfer preflight changed a pointer parameter");
    if (!transfer->getArg(5)->getType()->isIntegerTy(32))
        return reject("Native v3 transfer preflight changed injection type");
    const llvm::CallInst* contextCall = nullptr;
    const llvm::CallInst* refCall = nullptr;
    const llvm::CallInst* bodyCall = nullptr;
    for (const auto& block : *transfer)
        for (const auto& instruction : block)
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                    &instruction)) {
                const auto* callee = call->getCalledFunction();
                if (!callee) continue;
                if (callee->getName() ==
                        "luna_compiler_fragment_context_check") {
                    if (contextCall) return reject(
                        "Native v3 transfer checks context twice");
                    contextCall = call;
                } else if (callee->getName() ==
                        "luna_runtime_fragment_ref_check_v1") {
                    if (refCall) return reject(
                        "Native v3 transfer checks Ref twice");
                    refCall = call;
                } else if (callee == body) {
                    if (bodyCall) return reject(
                        "Native v3 transfer calls the body twice");
                    bodyCall = call;
                }
            }
    const auto validCheck = [](const llvm::CallInst* call,
                               unsigned argumentCount) {
        const auto* callee = call ? call->getCalledFunction() : nullptr;
        return callee && callee->isDeclaration() && !callee->isVarArg() &&
            callee->getCallingConv() == llvm::CallingConv::C &&
            callee->getReturnType()->isIntegerTy(32) &&
            callee->arg_size() == argumentCount &&
            call->getCallingConv() == llvm::CallingConv::C &&
            call->arg_size() == argumentCount;
    };
    if (!validCheck(contextCall, 1) || !validCheck(refCall, 3) ||
        !bodyCall || contextCall->getArgOperand(0) != transfer->getArg(0) ||
        refCall->getArgOperand(0) != transfer->getArg(1) ||
        bodyCall->arg_size() != 2 ||
        bodyCall->getArgOperand(0) != transfer->getArg(0) ||
        bodyCall->getArgOperand(1) != transfer->getArg(1))
        return reject("Native v3 transfer preflight changed borrowed inputs");
    const auto matchesFrozenString = [](const llvm::Value* pointer,
                                        const std::string& expected) {
        const auto* global = llvm::dyn_cast<llvm::GlobalVariable>(
            pointer->stripPointerCasts());
        const auto* bytes = global && global->isConstant() &&
            global->hasInitializer()
            ? llvm::dyn_cast<llvm::ConstantDataArray>(
                global->getInitializer()) : nullptr;
        return bytes && bytes->isCString() &&
            bytes->getAsCString().str() == expected;
    };
    if (!matchesFrozenString(refCall->getArgOperand(1),
            source.refSlotSymbolId) ||
        !matchesFrozenString(refCall->getArgOperand(2),
            source.refSlotContractId))
        return reject("Native v3 transfer preflight changed Ref target");
    const auto* context = contextCall->getParent();
    const auto* contextSwitch = llvm::dyn_cast<llvm::SwitchInst>(
        context->getTerminator());
    const auto* checkedRef = refCall->getParent();
    const auto* refSwitch = llvm::dyn_cast<llvm::SwitchInst>(
        checkedRef->getTerminator());
    if (!contextSwitch || context->size() != 2 ||
        contextSwitch->getCondition() != contextCall ||
        contextSwitch->getNumCases() != 2 ||
        !refSwitch || checkedRef->size() != 2 ||
        refSwitch->getCondition() != refCall ||
        refSwitch->getNumCases() != 3 ||
        std::distance(llvm::pred_begin(checkedRef),
                      llvm::pred_end(checkedRef)) != 1 ||
        *llvm::pred_begin(checkedRef) != context)
        return reject("Native v3 transfer preflight lost ordered checks");
    const auto caseTarget = [](const llvm::SwitchInst* branch,
                               int64_t value) -> const llvm::BasicBlock* {
        for (const auto& item : branch->cases())
            if (item.getCaseValue()->getSExtValue() == value)
                return item.getCaseSuccessor();
        return nullptr;
    };
    const auto statusBlock = [](const llvm::BasicBlock* block,
                                uint32_t expected) {
        const auto* returned = block && block->size() == 1
            ? llvm::dyn_cast<llvm::ReturnInst>(
                block->getTerminator()) : nullptr;
        const auto* status = returned
            ? llvm::dyn_cast_or_null<llvm::ConstantInt>(
                returned->getReturnValue()) : nullptr;
        return status && status->getBitWidth() == 32 &&
            status->getZExtValue() == expected;
    };
    const auto* unexpected = contextSwitch->getDefaultDest();
    const auto* bodyBlock = bodyCall->getParent();
    if (caseTarget(contextSwitch,
            LUNA_COMPILER_FRAGMENT_OVERRIDE_SUCCESS) != checkedRef ||
        !statusBlock(caseTarget(contextSwitch,
            LUNA_COMPILER_FRAGMENT_OVERRIDE_INVALID_CONTEXT),
            invalidContextStatus) ||
        refSwitch->getDefaultDest() != unexpected ||
        !statusBlock(unexpected, unexpectedStatus) ||
        caseTarget(refSwitch, LUNA_RUNTIME_FRAGMENT_REF_SUCCESS_V1) !=
            bodyBlock ||
        !statusBlock(caseTarget(refSwitch,
            LUNA_RUNTIME_FRAGMENT_REF_INVALID_HANDLE_V1),
            invalidHandleStatus) ||
        !statusBlock(caseTarget(refSwitch,
            LUNA_RUNTIME_FRAGMENT_REF_INVALID_TARGET_V1),
            invalidTargetStatus) ||
        std::distance(llvm::pred_begin(bodyBlock),
                      llvm::pred_end(bodyBlock)) != 1 ||
        *llvm::pred_begin(bodyBlock) != checkedRef)
        return reject("Native v3 transfer preflight changed status routing");
    error.clear();
    return true;
}

bool verifyNativeOwnedResultTransferOutputOwnerGates(
    const llvm::Module& generated, const std::string& transferLinkageName,
    uint32_t invalidOutputStatus, std::string& error) {
    const auto reject = [&](const char* reason) {
        error = reason;
        return false;
    };
    const auto* transfer = generated.getFunction(transferLinkageName);
    if (!transfer || transfer->isDeclaration() || transfer->isVarArg() ||
        transfer->getCallingConv() != llvm::CallingConv::C ||
        !transfer->getReturnType()->isIntegerTy(32) ||
        transfer->arg_size() != 6 || !transfer->hasExternalLinkage())
        return reject("Native v3 transfer output gate lacks its C profile");
    const auto* entry = &transfer->getEntryBlock();
    const auto* entryBranch = llvm::dyn_cast<llvm::BranchInst>(
        entry->getTerminator());
    if (!entryBranch || !entryBranch->isConditional())
        return reject("Native v3 transfer output gate lost its entry fork");
    for (unsigned index = 2; index <= 4; ++index) {
        const auto* output = transfer->getArg(index);
        if (!output->getType()->isPointerTy() ||
            output->getType()->getPointerAddressSpace() != 0 ||
            !hasRequiredNonnullConjunct(entryBranch->getCondition(), output))
            return reject("Native v3 transfer output gate lost a non-null check");
    }
    const std::array<uint64_t, 3> alignments = {
        alignof(uint32_t), alignof(int32_t), alignof(void*)};
    for (unsigned index = 0; index < alignments.size(); ++index)
        if (!hasRequiredAlignmentConjunct(entryBranch->getCondition(),
                transfer->getArg(index + 2), alignments[index]))
            return reject("Native v3 transfer output gate lost an alignment check");
    struct OutputPair {
        unsigned left;
        uint64_t leftSize;
        unsigned right;
        uint64_t rightSize;
    };
    const std::array<OutputPair, 3> pairs = {{
        {2, sizeof(uint32_t), 3, sizeof(int32_t)},
        {2, sizeof(uint32_t), 4, sizeof(void*)},
        {3, sizeof(int32_t), 4, sizeof(void*)},
    }};
    for (const auto& pair : pairs)
        if (!hasRequiredDisjointConjunct(entryBranch->getCondition(),
                transfer->getArg(pair.left), pair.leftSize,
                transfer->getArg(pair.right), pair.rightSize))
            return reject("Native v3 transfer output gate lost a region gap");
    for (const auto& instruction : *entry)
        if (!llvm::isa<llvm::AllocaInst>(instruction) &&
            !llvm::isa<llvm::PtrToIntInst>(instruction) &&
            !llvm::isa<llvm::ICmpInst>(instruction) &&
            !llvm::isa<llvm::BinaryOperator>(instruction) &&
            !llvm::isa<llvm::SelectInst>(instruction) &&
            !llvm::isa<llvm::BranchInst>(instruction))
            return reject("Native v3 transfer output gate accesses memory early");
    const auto* ownerBlock = entryBranch->getSuccessor(0);
    const auto* invalid = entryBranch->getSuccessor(1);
    if (ownerBlock == invalid || ownerBlock->size() != 3 ||
        std::distance(llvm::pred_begin(ownerBlock),
                      llvm::pred_end(ownerBlock)) != 1 ||
        *llvm::pred_begin(ownerBlock) != entry)
        return reject("Native v3 transfer owner gate bypasses output checks");
    const auto* ownerLoad = llvm::dyn_cast<llvm::LoadInst>(
        &ownerBlock->front());
    const auto* empty = ownerLoad
        ? llvm::dyn_cast<llvm::ICmpInst>(ownerLoad->getNextNode()) : nullptr;
    const auto* ownerBranch = llvm::dyn_cast<llvm::BranchInst>(
        ownerBlock->getTerminator());
    if (!ownerLoad || ownerLoad->getPointerOperand() !=
            transfer->getArg(4) ||
        !ownerLoad->getType()->isPointerTy() ||
        ownerLoad->getType()->getPointerAddressSpace() != 0 ||
        !empty || empty->getPredicate() != llvm::ICmpInst::ICMP_EQ ||
        !((empty->getOperand(0) == ownerLoad &&
           llvm::isa<llvm::ConstantPointerNull>(empty->getOperand(1))) ||
          (empty->getOperand(1) == ownerLoad &&
           llvm::isa<llvm::ConstantPointerNull>(empty->getOperand(0)))) ||
        !ownerBranch || !ownerBranch->isConditional() ||
        ownerBranch->getCondition() != empty ||
        ownerBranch->getSuccessor(1) != invalid)
        return reject("Native v3 transfer owner gate lost its empty-cell check");
    const auto* context = ownerBranch->getSuccessor(0);
    const auto* contextCall = context->size() == 2
        ? llvm::dyn_cast<llvm::CallInst>(&context->front()) : nullptr;
    const auto* contextFunction = contextCall
        ? contextCall->getCalledFunction() : nullptr;
    if (!contextFunction || contextFunction->getName() !=
            "luna_compiler_fragment_context_check" ||
        contextCall->arg_size() != 1 ||
        contextCall->getArgOperand(0) != transfer->getArg(0) ||
        std::distance(llvm::pred_begin(context),
                      llvm::pred_end(context)) != 1 ||
        *llvm::pred_begin(context) != ownerBlock)
        return reject("Native v3 transfer context bypasses owner gate");
    const auto* returned = invalid->size() == 1
        ? llvm::dyn_cast<llvm::ReturnInst>(
            invalid->getTerminator()) : nullptr;
    const auto* status = returned
        ? llvm::dyn_cast_or_null<llvm::ConstantInt>(
            returned->getReturnValue()) : nullptr;
    if (!status || status->getBitWidth() != 32 ||
        status->getZExtValue() != invalidOutputStatus ||
        std::distance(llvm::pred_begin(invalid),
                      llvm::pred_end(invalid)) != 2)
        return reject("Native v3 transfer output gate changed failure status");
    for (const auto* predecessor : llvm::predecessors(invalid))
        if (predecessor != entry && predecessor != ownerBlock)
            return reject("Native v3 transfer output failure has another edge");
    error.clear();
    return true;
}

bool verifyNativeOwnedResultTransferClosedGraph(
    const llvm::Module& generated, const std::string& transferLinkageName,
    const NativeOwnedResultSourceFacts& source,
    const std::vector<std::string>& expectedCleanupCalls,
    const std::vector<NativeOwnedResultCleanupNode>& cleanupNodes,
    const NativeOwnedResultTransferStatuses& statuses,
    std::string& error) {
    if (!verifyNativeOwnedResultTransferCleanupProvenance(
            generated, transferLinkageName, source, expectedCleanupCalls,
            cleanupNodes,
            statuses.invalidResource, statuses.injectedFailure,
            error) ||
        !verifyNativeOwnedResultTransferScalarFork(
            generated, transferLinkageName, source,
            statuses.injectedFailure, statuses.success, error) ||
        !verifyNativeOwnedResultTransferResourceCommit(
            generated, transferLinkageName, source,
            statuses.success, error) ||
        !verifyNativeOwnedResultTransferContextRefPreflight(
            generated, transferLinkageName, source,
            statuses.invalidContext, statuses.invalidHandle,
            statuses.invalidTarget, statuses.unexpectedCheck,
            error) ||
        !verifyNativeOwnedResultTransferOutputOwnerGates(
            generated, transferLinkageName,
            statuses.invalidOutput,
            error))
        return false;
    const auto reject = [&](const char* reason) {
        error = reason;
        return false;
    };
    const auto* transfer = generated.getFunction(transferLinkageName);
    const auto* body = generated.getFunction(source.sourceLinkageName);
    if (transfer->size() != 18)
        return reject("Native v3 transfer graph changed its block count");
    llvm::SmallPtrSet<const llvm::BasicBlock*, 32> reached;
    std::vector<const llvm::BasicBlock*> pending = {&transfer->getEntryBlock()};
    reached.insert(pending.front());
    while (!pending.empty()) {
        const auto* block = pending.back();
        pending.pop_back();
        for (const auto* successor : llvm::successors(block))
            if (reached.insert(successor).second)
                pending.push_back(successor);
    }
    if (reached.size() != transfer->size())
        return reject("Native v3 transfer graph has an unreachable block");
    const auto* ownerGate = llvm::cast<llvm::BranchInst>(
        transfer->getEntryBlock().getTerminator())->getSuccessor(0);
    const llvm::CallInst* bodyCall = nullptr;
    for (const auto& block : *transfer)
        for (const auto& instruction : block)
            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                    &instruction); call && call->getCalledFunction() == body)
                bodyCall = call;
    const auto* dispatch = llvm::cast<llvm::BranchInst>(
        bodyCall->getParent()->getTerminator());
    const auto* resource = llvm::cast<llvm::BranchInst>(
        dispatch->getSuccessor(1)->getTerminator());
    const auto* valid = llvm::cast<llvm::BranchInst>(
        resource->getSuccessor(0)->getTerminator());
    const auto* cleanup = valid->getSuccessor(0);
    unsigned ownerLoads = 0;
    unsigned tagStores = 0;
    unsigned scalarStores = 0;
    unsigned ownerStores = 0;
    unsigned preflightCalls = 0;
    unsigned bodyCalls = 0;
    unsigned cleanupCalls = 0;
    unsigned returns = 0;
    unsigned branches = 0;
    unsigned switches = 0;
    for (const auto& block : *transfer)
        for (const auto& instruction : block) {
            if (llvm::isa<llvm::AllocaInst>(instruction))
                return reject("Native v3 transfer graph added an allocation");
            if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(
                    &instruction)) {
                if (load->isVolatile() || load->isAtomic())
                    return reject("Native v3 transfer graph added a volatile or atomic load");
                if (&block == ownerGate &&
                    load->getPointerOperand() == transfer->getArg(4))
                    ++ownerLoads;
                else if (&block != cleanup)
                    return reject("Native v3 transfer graph added a read outside cleanup");
            } else if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(
                           &instruction)) {
                if (store->isVolatile() || store->isAtomic())
                    return reject("Native v3 transfer graph added a volatile or atomic store");
                const auto* destination = store->getPointerOperand();
                if (destination == transfer->getArg(2)) ++tagStores;
                else if (destination == transfer->getArg(3)) ++scalarStores;
                else if (destination == transfer->getArg(4)) ++ownerStores;
                else return reject("Native v3 transfer graph added a store");
            } else if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                           &instruction)) {
                const auto* callee = call->getCalledFunction();
                if (&block == cleanup && callee &&
                    cleanupCalls < expectedCleanupCalls.size() &&
                    callee->getName() == expectedCleanupCalls[cleanupCalls])
                    ++cleanupCalls;
                else if (callee == body) ++bodyCalls;
                else if (callee && (callee->getName() ==
                             "luna_compiler_fragment_context_check" ||
                         callee->getName() ==
                             "luna_runtime_fragment_ref_check_v1"))
                    ++preflightCalls;
                else return reject("Native v3 transfer graph added an unrecognized call");
            } else if (llvm::isa<llvm::ReturnInst>(instruction)) {
                ++returns;
            } else if (llvm::isa<llvm::BranchInst>(instruction)) {
                ++branches;
            } else if (llvm::isa<llvm::SwitchInst>(instruction)) {
                ++switches;
            } else if (instruction.mayHaveSideEffects() ||
                       instruction.isTerminator()) {
                return reject("Native v3 transfer graph added a side effect or exit");
            }
        }
    if (ownerLoads != 1 || tagStores != 2 || scalarStores != 1 ||
        ownerStores != 1 || preflightCalls != 2 || bodyCalls != 1 ||
        cleanupCalls != expectedCleanupCalls.size() || returns != 10 ||
        branches != 6 || switches != 2)
        return reject("Native v3 transfer graph changed its effects or exits");
    error.clear();
    return true;
}

} // namespace luna::codegen
