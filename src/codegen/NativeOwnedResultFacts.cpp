#include "codegen/NativeOwnedResultFacts.h"

#include "core/TypeLayout.h"
#include "core/TypeRelations.h"
#include "moonir/FragmentContextEffects.h"
#include "moonir/MoonIRModule.h"
#include "moonir/Verifier.h"

#include <algorithm>
#include <array>
#include <utility>

namespace luna::codegen {

namespace {

bool boundedIdentity(const std::string& value) {
    if (value.empty() || value.size() > 4096)
        return false;
    for (char ch : value)
        if (ch == '\r' || ch == '\n' || ch == '\t')
            return false;
    return true;
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

} // namespace luna::codegen
