#include "codegen/NativeRefUnitFacts.h"

#include "core/TypeRelations.h"
#include "driver/NativeTypedDescriptor.h"
#include "moonir/FragmentContextEffects.h"
#include "moonir/MoonIRModule.h"
#include "moonir/Verifier.h"
#include "runtime/NativeArtifactABI.h"
#include "runtime/RuntimeFragmentABI.h"
#include "runtime/RuntimeFragmentCompilerBridge.h"

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <algorithm>
#include <unordered_set>
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

} // namespace

bool deriveNativeRefUnitSourceFacts(
    const moon::Module& program, const moon::FunctionDecl& function,
    NativeRefUnitSourceFacts& facts, std::string& error) {
    const auto reject = [&](const char* reason) {
        error = reason;
        return false;
    };
    if (!program.typeTableSealed || !function.controlFlow ||
        !function.controlFlow->sealed ||
        function.controlFlow->runtimeRefApplies.empty() ||
        function.isExtern || function.isKernel || function.isSelector ||
        !function.typeParams.empty() || !function.abi.empty() ||
        !function.linkName.empty() || function.generatedSymbolName.empty() ||
        function.body || !function.requiresFragmentContext ||
        function.params.size() != 1 || function.returnsLinear ||
        function.returnUsage != luna::ownership::Usage::Copy ||
        !std::any_of(program.declarations.begin(), program.declarations.end(),
            [&function](const auto& declaration) {
                return declaration.get() == &function;
            }))
        return reject("Native Ref/unit candidate has no sealed source callable");
    moon::Verifier verifier;
    if (!verifier.verify(*function.controlFlow, program))
        return reject("Native Ref/unit candidate has no verified CFG");
    const auto effects = moon::computeFragmentContextEffects(program);
    const auto found = effects.find(moon::fragmentContextEffectKey(
        {function.symbolId, function.contractId}));
    if (found == effects.end() || !found->second)
        return reject("Native Ref/unit candidate lost its context effect");

    const auto& parameter = function.params.front();
    const auto* ref = program.findType(parameter.type);
    const auto* result = program.findType(function.returnType);
    const auto target = program.resolveRuntimeFragmentRefTarget(parameter.type);
    const auto* slot = target ? program.findDeclaration(*target) : nullptr;
    if (!ref || ref->kind != TypeKind::RuntimeFragmentRef ||
        !result || result->kind != TypeKind::Unit || !target || !slot ||
        slot->kind != moon::DeclarationKind::Slot || parameter.isLinear ||
        parameter.relation != luna::ownership::Relation::SharedBorrow ||
        parameter.usage != luna::ownership::Usage::Copy)
        return reject("Native Ref/unit candidate has no exact borrowed Slot target");

    const auto* record = program.findDeclarationById(function.declarationId);
    const auto* callable = record ? program.findType(record->type) : nullptr;
    moon::TypeMaterializer materializer(program);
    const TypePtr restoredCallable = callable
        ? materializer.materialize(record->type) : nullptr;
    if (!record || record->kind != moon::DeclarationKind::Function ||
        record->id != function.declarationId ||
        record->symbolId != function.symbolId ||
        record->symbolId != luna::identity::symbolIdFromCanonical(record->id) ||
        record->contractId != function.contractId ||
        record->canonicalContract != moon::canonicalContract(*record) ||
        record->contractId != luna::identity::contractIdFromCanonical(
            record->canonicalContract) ||
        record->sysmeta.identity.symbol != record->symbolId ||
        record->sysmeta.identity.contract != record->contractId ||
        record->linkageName != function.generatedSymbolName ||
        !callable || callable->kind != TypeKind::Function ||
        !restoredCallable ||
        luna::types::typeId(restoredCallable) != callable->id ||
        luna::types::canonicalType(restoredCallable) != callable->canonicalType ||
        callable->parameterTypeIds.size() != 1 ||
        callable->parameterTypeIds.front() != parameter.type ||
        callable->returnTypeId != function.returnType ||
        callable->parameterContracts.size() != 1 ||
        callable->parameterContracts.front() !=
            luna::ownership::Contract{parameter.relation, parameter.usage} ||
        callable->returnContract != luna::ownership::Contract{
            luna::ownership::Relation::Owned, function.returnUsage} ||
        record->sysmeta.resource.parameters.size() != 1 ||
        record->sysmeta.resource.parameters.front() !=
            callable->parameterContracts.front() ||
        record->sysmeta.resource.result != callable->returnContract)
        return reject("Native Ref/unit candidate differs from its frozen signature");

    const auto& graph = *function.controlFlow;
    const moon::LocalRecord* parameterLocal = nullptr;
    for (const auto& local : graph.locals)
        if (local.kind == moon::LocalKind::Parameter) {
            if (parameterLocal)
                return reject("Native Ref/unit candidate has ambiguous ingress");
            parameterLocal = &local;
        }
    if (!parameterLocal || parameterLocal->scope != graph.rootScope ||
        parameterLocal->name != parameter.name ||
        parameterLocal->type != parameter.type ||
        parameterLocal->relation != parameter.relation ||
        parameterLocal->usage != parameter.usage ||
        !std::all_of(graph.runtimeRefApplies.begin(),
                     graph.runtimeRefApplies.end(), [&](const auto& apply) {
            return apply.reference == parameterLocal->id &&
                   apply.slot == *target;
        }))
        return reject("Native Ref/unit candidate changed its Apply target");

    NativeRefUnitSourceFacts candidate;
    candidate.moduleName = program.name;
    candidate.packageId = function.packageId;
    candidate.functionSymbolId = record->symbolId.value;
    candidate.functionContractId = record->contractId.value;
    candidate.functionTypeId = callable->id.value;
    candidate.sourceLinkageName = record->linkageName;
    candidate.refTypeId = ref->id.value;
    candidate.slotSymbolId = target->symbol.value;
    candidate.slotContractId = target->contract.value;
    candidate.slotTypeId = slot->type.value;
    candidate.contextEffect = 1;
    const auto identities = candidate.identities();
    if (!std::all_of(identities.begin(), identities.end(), boundedIdentity))
        return reject("Native Ref/unit candidate has an invalid identity");
    facts = std::move(candidate);
    error.clear();
    return true;
}

std::array<std::string, 10>
NativeRefUnitDescriptorV4Candidate::descriptorIdentities() const {
    return {source.functionSymbolId, source.functionContractId,
            wrapperLinkageName, source.moduleName, source.packageId,
            source.functionTypeId, source.refTypeId, source.slotSymbolId,
            source.slotContractId, source.slotTypeId};
}

std::string NativeRefUnitDescriptorV4Candidate::canonicalRow() const {
    return luna::driver::canonicalNativeRefUnitExportV4(
        LUNA_NATIVE_DECLARATION_FUNCTION_V1,
        LUNA_NATIVE_EXPORT_CALLABLE_V1,
        LUNA_NATIVE_ENTRY_ABI_REF_UNIT_V1,
        LUNA_NATIVE_REF_SHARED_BORROW_V1,
        LUNA_NATIVE_RESULT_UNIT_V1,
        LUNA_NATIVE_STATUS_DOMAIN_REF_UNIT_V1,
        descriptorIdentities());
}

bool deriveNativeRefUnitDescriptorV4Candidate(
    const moon::Module& program, const moon::FunctionDecl& function,
    const std::string& wrapperLinkageName,
    NativeRefUnitDescriptorV4Candidate& candidate, std::string& error) {
    NativeRefUnitSourceFacts source;
    if (!deriveNativeRefUnitSourceFacts(program, function, source, error))
        return false;
    if (source.packageId != program.name ||
        !boundedIdentity(wrapperLinkageName) ||
        wrapperLinkageName == source.sourceLinkageName) {
        error = "Native v4 candidate has no matching package or distinct wrapper linkage";
        return false;
    }
    NativeRefUnitDescriptorV4Candidate verified;
    verified.source = std::move(source);
    verified.wrapperLinkageName = wrapperLinkageName;
    candidate = std::move(verified);
    error.clear();
    return true;
}

bool derivePairedNativeRefUnitDescriptorV4Candidate(
    const moon::Module& program, const moon::FunctionDecl& function,
    const llvm::Module& generated, const std::string& wrapperLinkageName,
    NativeRefUnitDescriptorV4Candidate& candidate, std::string& error) {
    NativeRefUnitDescriptorV4Candidate verified;
    if (!deriveNativeRefUnitDescriptorV4Candidate(
            program, function, wrapperLinkageName, verified, error))
        return false;
    const auto reject = [&](const char* reason) {
        error = reason;
        return false;
    };
    const auto* wrapper = generated.getFunction(wrapperLinkageName);
    const auto* body = generated.getFunction(
        verified.source.sourceLinkageName);
    const auto exactPointerPair = [](const llvm::Function* entry,
                                     bool returnsStatus) {
        return entry && !entry->isDeclaration() && !entry->isVarArg() &&
            entry->getCallingConv() == llvm::CallingConv::C &&
            entry->arg_size() == 2 &&
            entry->getArg(0)->getType()->isPointerTy() &&
            entry->getArg(1)->getType()->isPointerTy() &&
            (returnsStatus ? entry->getReturnType()->isIntegerTy(32)
                           : entry->getReturnType()->isVoidTy());
    };
    if (!exactPointerPair(wrapper, true) ||
        !exactPointerPair(body, false) ||
        !wrapper->hasExternalLinkage() || !body->hasInternalLinkage())
        return reject("Native v4 wrapper/body lacks the exact generated C profile");

    size_t contextChecks = 0, refChecks = 0, bodyCalls = 0;
    for (const auto& block : *wrapper)
        for (const auto& instruction : block)
            if (const auto* call = llvm::dyn_cast<llvm::CallBase>(
                    &instruction)) {
                const auto* callee = call->getCalledFunction();
                if (!llvm::isa<llvm::CallInst>(call) || !callee ||
                    call->getCallingConv() != llvm::CallingConv::C)
                    return reject("Native v4 wrapper has an indirect or non-C call");
                if (callee == body) {
                    if (call->arg_size() != 2 ||
                        call->getArgOperand(0) != wrapper->getArg(0) ||
                        call->getArgOperand(1) != wrapper->getArg(1))
                        return reject("Native v4 wrapper changed its source body arguments");
                    ++bodyCalls;
                } else if (callee->getName() ==
                           "luna_compiler_fragment_context_check") {
                    if (call->arg_size() != 1 ||
                        call->getArgOperand(0) != wrapper->getArg(0) ||
                        !callee->getReturnType()->isIntegerTy(32))
                        return reject("Native v4 wrapper changed its context check");
                    ++contextChecks;
                } else if (callee->getName() ==
                           "luna_runtime_fragment_ref_check_v1") {
                    llvm::StringRef slot, contract;
                    if (call->arg_size() != 3 ||
                        call->getArgOperand(0) != wrapper->getArg(1) ||
                        !callee->getReturnType()->isIntegerTy(32) ||
                        !llvm::getConstantStringInfo(
                            call->getArgOperand(1), slot) ||
                        !llvm::getConstantStringInfo(
                            call->getArgOperand(2), contract) ||
                        slot.str() != verified.source.slotSymbolId ||
                        contract.str() != verified.source.slotContractId)
                        return reject("Native v4 wrapper changed its exact borrowed Slot target");
                    ++refChecks;
                } else {
                    return reject("Native v4 wrapper has an unproved call");
                }
            }
    if (contextChecks != 1 || refChecks != 1 || bodyCalls != 1)
        return reject("Native v4 wrapper lost its context/Ref/body pairing");
    candidate = std::move(verified);
    error.clear();
    return true;
}

bool derivePreflightNativeRefUnitDescriptorV4Candidate(
    const moon::Module& program, const moon::FunctionDecl& function,
    const llvm::Module& generated, const std::string& wrapperLinkageName,
    NativeRefUnitDescriptorV4Candidate& candidate, std::string& error) {
    NativeRefUnitDescriptorV4Candidate verified;
    if (!derivePairedNativeRefUnitDescriptorV4Candidate(
            program, function, generated, wrapperLinkageName,
            verified, error))
        return false;
    const auto reject = [&](const char* reason) {
        error = reason;
        return false;
    };
    const auto* wrapper = generated.getFunction(wrapperLinkageName);
    if (wrapper->size() != 7)
        return reject("Native v4 preflight has an unexpected block");
    const auto* entry = &wrapper->getEntryBlock();
    const auto* contextCall = entry->size() == 2
        ? llvm::dyn_cast<llvm::CallInst>(&entry->front()) : nullptr;
    const auto* contextBranch = llvm::dyn_cast_or_null<llvm::SwitchInst>(
        entry->getTerminator());
    const auto switchDestination = [](const llvm::SwitchInst* branch,
                                      int32_t status) -> const llvm::BasicBlock* {
        if (!branch) return nullptr;
        for (const auto choice : branch->cases())
            if (choice.getCaseValue()->getSExtValue() == status)
                return choice.getCaseSuccessor();
        return nullptr;
    };
    if (!contextCall || !contextCall->getCalledFunction() ||
        contextCall->getCalledFunction()->getName() !=
            "luna_compiler_fragment_context_check" ||
        !contextBranch || contextBranch->getCondition() != contextCall ||
        contextBranch->getNumCases() != 2)
        return reject("Native v4 preflight lost its context status switch");
    const auto* checkedContext = switchDestination(
        contextBranch, LUNA_COMPILER_FRAGMENT_OVERRIDE_SUCCESS);
    const auto* invalidContext = switchDestination(
        contextBranch, LUNA_COMPILER_FRAGMENT_OVERRIDE_INVALID_CONTEXT);
    const auto* unexpected = contextBranch->getDefaultDest();
    const auto* refCall = checkedContext && checkedContext->size() == 2
        ? llvm::dyn_cast<llvm::CallInst>(&checkedContext->front()) : nullptr;
    const auto* refBranch = checkedContext
        ? llvm::dyn_cast_or_null<llvm::SwitchInst>(
            checkedContext->getTerminator())
        : nullptr;
    if (!refCall || !refCall->getCalledFunction() ||
        refCall->getCalledFunction()->getName() !=
            "luna_runtime_fragment_ref_check_v1" ||
        !refBranch || refBranch->getCondition() != refCall ||
        refBranch->getNumCases() != 3 ||
        refBranch->getDefaultDest() != unexpected)
        return reject("Native v4 preflight lost its Ref status switch");
    const auto* bodyBlock = switchDestination(
        refBranch, LUNA_RUNTIME_FRAGMENT_REF_SUCCESS_V1);
    const auto* invalidHandle = switchDestination(
        refBranch, LUNA_RUNTIME_FRAGMENT_REF_INVALID_HANDLE_V1);
    const auto* invalidTarget = switchDestination(
        refBranch, LUNA_RUNTIME_FRAGMENT_REF_INVALID_TARGET_V1);
    const auto* body = generated.getFunction(
        verified.source.sourceLinkageName);
    const auto* bodyCall = bodyBlock && bodyBlock->size() == 2
        ? llvm::dyn_cast<llvm::CallInst>(&bodyBlock->front()) : nullptr;
    if (!bodyCall || bodyCall->getCalledFunction() != body)
        return reject("Native v4 preflight lost its guarded body call");
    const std::unordered_set<const llvm::BasicBlock*> blocks = {
        entry, checkedContext, invalidContext, invalidHandle,
        invalidTarget, unexpected, bodyBlock};
    if (blocks.size() != 7 || blocks.count(nullptr) ||
        std::any_of(blocks.begin(), blocks.end(),
            [wrapper](const auto* block) {
                return block->getParent() != wrapper;
            }))
        return reject("Native v4 preflight aliases a status path");
    const auto returnsStatus = [](const llvm::BasicBlock* block,
                                  int32_t expected) {
        const auto* returned = block && block->size() == 1
            ? llvm::dyn_cast_or_null<llvm::ReturnInst>(
                block->getTerminator())
            : nullptr;
        const auto* status = returned
            ? llvm::dyn_cast_or_null<llvm::ConstantInt>(
                returned->getReturnValue())
            : nullptr;
        return status && status->getSExtValue() == expected;
    };
    const auto* successReturn = llvm::dyn_cast_or_null<llvm::ReturnInst>(
        bodyBlock->getTerminator());
    const auto* successStatus = successReturn
        ? llvm::dyn_cast_or_null<llvm::ConstantInt>(
            successReturn->getReturnValue())
        : nullptr;
    if (!returnsStatus(invalidContext,
            LUNA_NATIVE_REF_UNIT_INVALID_CONTEXT_V1) ||
        !returnsStatus(invalidHandle,
            LUNA_NATIVE_REF_UNIT_INVALID_HANDLE_V1) ||
        !returnsStatus(invalidTarget,
            LUNA_NATIVE_REF_UNIT_INVALID_TARGET_V1) ||
        !returnsStatus(unexpected,
            LUNA_NATIVE_REF_UNIT_UNEXPECTED_CHECK_V1) ||
        !successStatus ||
        successStatus->getSExtValue() != LUNA_NATIVE_REF_UNIT_SUCCESS_V1)
        return reject("Native v4 preflight changed its status returns");
    candidate = std::move(verified);
    error.clear();
    return true;
}

} // namespace luna::codegen
