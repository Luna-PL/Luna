#include "moonir_canonical_test_support.h"

#include "driver/CompilerPipeline.h"
#include "driver/MoonGeneration.h"
#include "diagnostics/Diagnostic.h"
#include "moonir/ContainerModel.h"
#include "runtime/RuntimeFragment.h"

#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace canonical_test {
namespace {

// ORC entries have no native compiler-emitted UBSan function metadata. Exempt
// only the JIT call boundary; compiler/runtime checks remain instrumented.
#if defined(__clang__)
LLVM_NO_SANITIZE("function")
#endif
int32_t invokeContextEntry(const void* address, const void* context, int32_t value) {
    return reinterpret_cast<int32_t (*)(const void*, int32_t)>(
        const_cast<void*>(address))(context, value);
}

bool hasPublicationError(const moon::Verifier& verifier) {
    return std::any_of(verifier.errors().begin(), verifier.errors().end(),
        [](const auto& error) {
            return error.message.find("runtime Slot target is not an exported control") !=
                std::string::npos;
        });
}

} // namespace

int runRuntimeSlotContainerTest() {
    trace("verified runtime Slot container");
    const std::string source = R"luna(
package canonical.slot_container;

export slot hook(value: i32);

export fragment gate[mask: i32](value: i32) for hook {
    if (value & mask) == 0 { resume; }
}

runtime fn normal_entry(value: i32) -> i32 {
    let result = value * 3;
    hook(value) { result += 17; }
    return result;
}

runtime fn escape_entry(value: i32) -> i32 {
    hook(value) { return value * 3 + 17; }
    return value * 3;
}
)luna";
    luna::driver::CompilerPipeline pipeline;
    luna::driver::CompilerPipelineOptions options;
    options.optimizationLevel = LunaOptimizationLevel::O2;
    if (!pipeline.compileSourceToMoonIR(source, "<runtime-slot-container>", options)) {
        for (const auto& diagnostic : pipeline.errors())
            std::cerr << diagnostic::render(diagnostic) << '\n';
        return fail("runtime Slot container source did not compile");
    }
    initializeLunaLLVMTargets();
    auto target = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!target) return fail("runtime Slot container could not detect its host");
    auto layout = target->getDefaultDataLayoutForTarget();
    if (!layout) return fail("runtime Slot container could not detect its data layout");
    moon::ContainerManifest manifest;
    manifest.packageId = pipeline.moonModule().name;
    manifest.packageVersion = "0.3.0";
    manifest.packageKind = moon::ContainerPackageKind::Library;
    manifest.targetTriple = target->getTargetTriple().str();
    manifest.dataLayout = layout->getStringRepresentation();
    manifest.features = pipeline.moonModule().features;
    std::vector<uint8_t> bytes;
    std::string error;
    if (!moon::ContainerModelCodec::encodeContainer(
            manifest, pipeline.moonModule(), bytes, error)) {
        std::cerr << error << '\n';
        return fail("runtime Slot container did not encode");
    }
    moon::ContainerManifest decodedManifest;
    moon::Module decoded;
    if (!moon::ContainerModelCodec::decodeContainerForTarget(
            bytes, manifest.targetTriple, manifest.dataLayout,
            decodedManifest, decoded, error)) {
        std::cerr << error << '\n';
        return fail("runtime Slot container did not survive verified decoding");
    }
    const moon::DeclarationRecord* slot = nullptr;
    const moon::DeclarationRecord* normal = nullptr;
    const moon::DeclarationRecord* escaping = nullptr;
    for (const auto& row : decoded.declarationTable) {
        if (row.kind == moon::DeclarationKind::Slot && row.sourceName == "hook") slot = &row;
        if (row.kind == moon::DeclarationKind::Function && row.sourceName == "normal_entry") normal = &row;
        if (row.kind == moon::DeclarationKind::Function && row.sourceName == "escape_entry") escaping = &row;
    }
    if (!slot || !normal || !escaping || decoded.declarationsById.count(slot->id) != 0)
        return fail("decoded fixture did not exercise Slot publication without a SlotDecl");
    const moon::DeclarationRef slotReference{slot->symbolId, slot->contractId};
    const luna::runtime::RuntimeSlotRequirement requirement{
        slot->symbolId.value, slot->contractId.value};

    // Absence of the frontend object must not make an absent export public.
    const auto savedExports = decoded.exports;
    decoded.exports.erase(std::remove_if(decoded.exports.begin(), decoded.exports.end(),
        [&](const auto& row) { return row.declaration == slotReference; }), decoded.exports.end());
    moon::Verifier verifier;
    if (verifier.verify(decoded) || !hasPublicationError(verifier))
        return fail("decoded runtime Slot without a publication row was accepted");
    decoded.exports = savedExports;
    const auto exportedSlot = std::find_if(decoded.exports.begin(), decoded.exports.end(),
        [&](const auto& row) { return row.declaration == slotReference; });
    if (exportedSlot == decoded.exports.end())
        return fail("decoded fixture lost its saved Slot export");
    exportedSlot->declaration.contract.value += "-wrong";
    if (verifier.verify(decoded) || !hasPublicationError(verifier))
        return fail("decoded runtime Slot accepted a mismatched export ContractId");
    exportedSlot->declaration = slotReference;
    exportedSlot->kind = moon::DeclarationKind::Function;
    if (verifier.verify(decoded) || !hasPublicationError(verifier))
        return fail("decoded runtime Slot accepted an export of the wrong declaration kind");
    // Even a root export and a declared dependency cannot attest a foreign
    // Slot when its independently checked publication facts are absent.
    const auto savedName = decoded.name;
    decoded.name = "canonical.slot_container_consumer";
    decoded.packageUses.push_back({decoded.name, savedName, "owner"});
    decoded.exports = savedExports;
    if (verifier.verify(decoded) || !hasPublicationError(verifier))
        return fail("decoded foreign runtime Slot was published by an import/re-export");
    decoded.packageUses.pop_back();
    decoded.name = savedName;
    if (!verifier.verify(decoded))
        return fail("restored decoded local Slot publication was rejected");

    luna::runtime::MoonRuntime::PinnedBinding normalBinding, escapeBinding;
    luna::runtime::RuntimeFragmentExecutionContext noneContext, selectedContext;
    {
        luna::runtime::MoonRuntime runtime;
        luna::runtime::MoonRuntime::PinnedGeneration generation;
        if (!luna::driver::loadVerifiedMoonGenerationOnce(
                runtime, bytes, manifest.targetTriple, manifest.dataLayout,
                generation, error)) {
            std::cerr << error << '\n';
            return fail("runtime Slot container did not load through the verified generation adapter");
        }
        normalBinding = generation.find(normal->symbolId.value, normal->contractId.value);
        escapeBinding = generation.find(escaping->symbolId.value, escaping->contractId.value);
        for (const auto* binding : {&normalBinding, &escapeBinding})
            if (!*binding || !binding->implementation() ||
                (binding->flags() & luna::runtime::GenerationBindingFragmentContext) == 0)
                return fail("verified container lost a context-aware host entry");
        luna::runtime::RuntimeFragmentBindingSet none;
        if (!luna::runtime::makeRuntimeFragmentBindingSet({}, none, error) ||
            !luna::runtime::makeRuntimeFragmentExecutionContext(none, noneContext, error))
            return fail("verified runtime Slot container could not create None policy");
        luna::runtime::RuntimeFragmentCandidateSnapshot candidates;
        if (!luna::runtime::snapshotRuntimeFragmentCandidates(
                generation, requirement, candidates, error) || candidates.size() != 1)
            return fail("verified container lost its executable Fragment candidate");
        const auto* descriptor = static_cast<const LunaRuntimeFragmentDescriptorV1*>(
            candidates.at(0)->implementation());
        if (!descriptor || descriptor->environment_size != sizeof(int32_t) ||
            descriptor->environment_alignment != alignof(int32_t))
            return fail("verified container changed the Copy mask factory ABI");
        const int32_t mask = 1;
        luna::runtime::RuntimeFragmentRef reference;
        if (!luna::runtime::makeOwnedRuntimeFragmentRef(*candidates.at(0), requirement,
                {descriptor->factory_contract_id, &mask}, reference, error))
            return fail("verified container Fragment factory did not execute");
        std::vector<luna::runtime::RuntimeFragmentRef> selected;
        selected.push_back(std::move(reference));
        luna::runtime::RuntimeFragmentBindingSet one;
        if (!luna::runtime::makeRuntimeFragmentBindingSet(std::move(selected), one, error))
            return fail("verified container Fragment did not bind");
        auto safePoint = runtime.safePoint();
        if (!runtime.activateFragmentBindings(one, safePoint, error) ||
            !luna::runtime::makeRuntimeFragmentExecutionContext(
                runtime.pinFragmentBindings(), selectedContext, error))
            return fail("verified container Fragment did not activate/pin");
    }
    for (int32_t value = 0; value < 32; ++value) {
        for (const auto* binding : {&normalBinding, &escapeBinding}) {
            if (invokeContextEntry(binding->implementation(), noneContext.opaque(), value) != value * 3 + 17)
                return fail("verified None dispatch lost continuation completion/escape");
            const auto expected = value * 3 + ((value & 1) == 0 ? 17 : 0);
            if (invokeContextEntry(binding->implementation(), selectedContext.opaque(), value) != expected)
                return fail("verified One dispatch lost factory state, resume/discard or escape");
        }
    }
    return 0;
}

} // namespace canonical_test
