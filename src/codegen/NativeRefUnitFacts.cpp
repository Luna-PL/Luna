#include "codegen/NativeRefUnitFacts.h"

#include "core/TypeRelations.h"
#include "driver/NativeTypedDescriptor.h"
#include "moonir/FragmentContextEffects.h"
#include "moonir/MoonIRModule.h"
#include "moonir/Verifier.h"

#include <algorithm>
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

} // namespace luna::codegen
