#include "moonir_canonical_test_support.h"

#include "diagnostics/Diagnostic.h"
#include "driver/CompilerPipeline.h"
#include "driver/MoonGeneration.h"
#include "moonir/ContainerModel.h"
#include "runtime/RuntimeFragment.h"

#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace canonical_test {
namespace {

#if defined(__clang__)
LLVM_NO_SANITIZE("function")
#endif
int32_t invokeContextEntry(const void* address, const void* context) {
    return reinterpret_cast<int32_t (*)(const void*)>(
        const_cast<void*>(address))(context);
}

bool compilePackage(const std::filesystem::path& path,
                    luna::driver::CompilerPipeline& pipeline) {
    luna::driver::CompilerPipelineOptions options;
    options.inputPath = path.string();
    options.optimizationLevel = LunaOptimizationLevel::O2;
    if (pipeline.compileToMoonIR(options)) return true;
    for (const auto& error : pipeline.errors())
        std::cerr << diagnostic::render(error) << '\n';
    return false;
}

} // namespace

int runCrossPackageContainerTest() {
    trace("verified cross-package Slot containers");
    const auto fixtures = std::filesystem::path(LUNA_TEST_SOURCE_DIR) /
        "tests/fixtures/runtime_fragment_container";
    luna::driver::CompilerPipeline hostPipeline, pluginPipeline;
    if (!compilePackage(fixtures / "host", hostPipeline) ||
        !compilePackage(fixtures / "plugin", pluginPipeline))
        return fail("cross-package container fixtures did not compile");
    initializeLunaLLVMTargets();
    auto target = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!target) return fail("cross-package container target detection failed");
    auto layout = target->getDefaultDataLayoutForTarget();
    if (!layout) return fail("cross-package container data layout detection failed");
    auto makeManifest = [&](const moon::Module& module) {
        moon::ContainerManifest manifest;
        manifest.packageId = module.name;
        manifest.packageVersion = "0.3.0";
        manifest.packageKind = moon::ContainerPackageKind::Library;
        manifest.targetTriple = target->getTargetTriple().str();
        manifest.dataLayout = layout->getStringRepresentation();
        manifest.features = module.features;
        return manifest;
    };
    const auto hostManifest = makeManifest(hostPipeline.moonModule());
    const auto pluginManifest = makeManifest(pluginPipeline.moonModule());
    std::vector<uint8_t> hostBytes, pluginBytes;
    std::string error;
    if (!moon::ContainerModelCodec::encodeContainer(hostManifest,
            hostPipeline.moonModule(), hostBytes, error) ||
        !moon::ContainerModelCodec::encodeContainer(pluginManifest,
            pluginPipeline.moonModule(), pluginBytes, error)) {
        std::cerr << error << '\n';
        return fail("cross-package containers did not encode");
    }
    moon::ContainerManifest decodedManifest;
    moon::Module owner;
    auto corruptedOwner = hostBytes;
    corruptedOwner.back() ^= 1;
    if (moon::ContainerModelCodec::decodeContainer(corruptedOwner,
            decodedManifest, owner, error) || owner.localSlotPublication)
        return fail("corrupt owner artifact issued publication evidence");
    if (moon::ContainerModelCodec::decodeContainerForTarget(hostBytes,
            hostManifest.targetTriple + "-wrong", hostManifest.dataLayout,
            decodedManifest, owner, error) || owner.localSlotPublication)
        return fail("host-target rejection published owner evidence");
    if (!moon::ContainerModelCodec::decodeContainerForTarget(hostBytes,
            hostManifest.targetTriple, hostManifest.dataLayout,
            decodedManifest, owner, error) || !owner.localSlotPublication)
        return fail("owner container did not issue verified Slot evidence");
    const moon::SlotPublicationDependencies dependencies{owner.localSlotPublication};
    auto sourceEvidence = [&](const std::string& source)
        -> std::shared_ptr<const moon::SlotPublicationEvidence> {
        luna::driver::CompilerPipeline pipeline;
        if (!pipeline.compileSourceToMoonIR(source, "<slot-evidence-owner>")) return {};
        const auto manifest = makeManifest(pipeline.moonModule());
        std::vector<uint8_t> bytes;
        moon::ContainerManifest decoded;
        moon::Module module;
        if (!moon::ContainerModelCodec::encodeContainer(
                manifest, pipeline.moonModule(), bytes, error) ||
            !moon::ContainerModelCodec::decodeContainer(bytes, decoded, module, error)) return {};
        return module.localSlotPublication;
    };
    const auto privateOwner = sourceEvidence(
        "package canonical.container.host; module effects; slot audit_slot(value: i32);");
    const auto changedContract = sourceEvidence(
        "package canonical.container.host; module effects; export slot audit_slot(value: i64);");
    const auto foreignOwner = sourceEvidence(
        "package canonical.container.unrelated; module effects; export slot audit_slot(value: i32);");
    auto otherTargetManifest = hostManifest;
    otherTargetManifest.targetTriple += "-other";
    std::vector<uint8_t> otherTargetBytes;
    moon::ContainerManifest otherDecodedManifest;
    moon::Module otherTarget;
    if (!privateOwner || !changedContract || !foreignOwner ||
        !moon::ContainerModelCodec::encodeContainer(otherTargetManifest,
            hostPipeline.moonModule(), otherTargetBytes, error) ||
        !moon::ContainerModelCodec::decodeContainer(otherTargetBytes,
            otherDecodedManifest, otherTarget, error))
        return fail("negative publication-evidence owners did not verify");
    moon::Module plugin;
    plugin.name = "unchanged.sentinel";
    decodedManifest.packageId = "unchanged.sentinel";
    if (moon::ContainerModelCodec::decodeContainer(pluginBytes,
            decodedManifest, plugin, error) ||
        error.find("not an exported control") == std::string::npos ||
        plugin.name != "unchanged.sentinel" ||
        decodedManifest.packageId != "unchanged.sentinel" || plugin.localSlotPublication)
        return fail("missing owner evidence was accepted or partially published");
    for (const moon::SlotPublicationDependencies& bad : {
            moon::SlotPublicationDependencies{nullptr},
            moon::SlotPublicationDependencies{owner.localSlotPublication,
                                              owner.localSlotPublication},
            moon::SlotPublicationDependencies{privateOwner},
            moon::SlotPublicationDependencies{changedContract},
            moon::SlotPublicationDependencies{foreignOwner},
            moon::SlotPublicationDependencies{otherTarget.localSlotPublication}}) {
        if (moon::ContainerModelCodec::decodeContainer(pluginBytes,
                decodedManifest, plugin, error, {}, bad) ||
            plugin.name != "unchanged.sentinel" ||
            decodedManifest.packageId != "unchanged.sentinel")
            return fail("invalid/duplicate owner evidence was accepted or published");
    }
    if (!moon::ContainerModelCodec::decodeContainerForTarget(pluginBytes,
            pluginManifest.targetTriple, pluginManifest.dataLayout,
            decodedManifest, plugin, error, {}, dependencies)) {
        std::cerr << error << '\n';
        return fail("plugin container rejected independently verified owner exports");
    }
    const moon::DeclarationRecord* slot = nullptr;
    const moon::DeclarationRecord* hostEntry = nullptr;
    const moon::DeclarationRecord* pluginEntry = nullptr;
    for (const auto& record : owner.declarationTable) {
        if (record.kind == moon::DeclarationKind::Slot && record.sourceName == "audit_slot") slot = &record;
        if (record.kind == moon::DeclarationKind::Function && record.sourceName == "host_probe") hostEntry = &record;
    }
    for (const auto& record : plugin.declarationTable)
        if (record.kind == moon::DeclarationKind::Function && record.sourceName == "dynamic_probe") pluginEntry = &record;
    if (!slot || !hostEntry || !pluginEntry ||
        plugin.declarationsById.count(slot->id) != 0 ||
        !owner.localSlotPublication->matches(*slot) ||
        plugin.localSlotPublication->matches(*slot))
        return fail("Slot evidence was lost or a dependency became a root publication");
    auto changed = *slot;
    changed.contractId.value += "-wrong";
    if (owner.localSlotPublication->matches(changed))
        return fail("owner evidence ignored the exact Slot ContractId");
    changed = *slot;
    changed.controlArgumentsType.value += "-wrong";
    if (owner.localSlotPublication->matches(changed))
        return fail("owner evidence ignored the frozen Slot argument layout");
    changed = *slot;
    changed.symbolId.value += "-wrong";
    if (owner.localSlotPublication->matches(changed))
        return fail("owner evidence ignored the exact Slot SymbolId");
    changed = *slot;
    changed.type.value += "-wrong";
    if (owner.localSlotPublication->matches(changed))
        return fail("owner evidence ignored the Slot structural type");
    plugin.dependencySlotPublications.clear();
    moon::Verifier verifier;
    if (verifier.verify(plugin)) return fail("consumer verified without its owner evidence");
    plugin.dependencySlotPublications = dependencies;
    if (!verifier.verify(plugin)) return fail("consumer did not restore with exact owner evidence");
    const auto savedUses = plugin.packageUses;
    plugin.packageUses.clear();
    if (verifier.verify(plugin)) return fail("owner evidence authorized an undeclared dependency");
    plugin.packageUses = savedUses;
    std::vector<uint8_t> roundtripBytes;
    if (!moon::ContainerModelCodec::encodeContainer(pluginManifest, plugin,
            roundtripBytes, error) || roundtripBytes != pluginBytes)
        return fail("dependency evidence changed the frozen container roundtrip bytes");
    // Evidence is an immutable snapshot, independent from mutable decoded IR.
    owner.exports.clear();
    if (!owner.localSlotPublication->matches(*slot))
        return fail("verified evidence aliased mutable owner export rows");

    luna::runtime::MoonRuntime::PinnedBinding hostBinding, pluginBinding;
    luna::runtime::RuntimeFragmentExecutionContext noneContext, oneContext;
    const luna::runtime::RuntimeSlotRequirement requirement{
        slot->symbolId.value, slot->contractId.value};
    {
        luna::runtime::MoonRuntime runtime;
        bool initialized = false;
        luna::runtime::MoonRuntime::StagedGeneration rejectedStage;
        if (luna::driver::stageVerifiedMoonGeneration(runtime, pluginBytes,
                pluginManifest.targetTriple, pluginManifest.dataLayout,
                [&](const auto&, const auto&, std::string&) { initialized = true; return true; },
                rejectedStage, error) || initialized || rejectedStage)
            return fail("missing owner evidence reached generation initialization");
        luna::runtime::MoonRuntime::PinnedGeneration rejected, hostGeneration, pluginGeneration;
        if (luna::driver::loadVerifiedMoonGenerationOnce(runtime, pluginBytes,
                pluginManifest.targetTriple, pluginManifest.dataLayout,
                rejected, error) || rejected || runtime.pin(pluginManifest.packageId))
            return fail("unverified plugin was JIT-published into the Runtime");
        if (!luna::driver::loadVerifiedMoonGenerationOnce(runtime, hostBytes,
                hostManifest.targetTriple, hostManifest.dataLayout, hostGeneration, error) ||
            !luna::driver::loadVerifiedMoonGenerationOnce(runtime, pluginBytes,
                pluginManifest.targetTriple, pluginManifest.dataLayout,
                pluginGeneration, error, dependencies)) {
            std::cerr << error << '\n';
            return fail("verified cross-package generations did not load");
        }
        if (hostGeneration.generationId() == pluginGeneration.generationId())
            return fail("owner and consumer were not independent generations");
        hostBinding = hostGeneration.find(hostEntry->symbolId.value, hostEntry->contractId.value);
        pluginBinding = pluginGeneration.find(pluginEntry->symbolId.value, pluginEntry->contractId.value);
        for (const auto* binding : {&hostBinding, &pluginBinding})
            if (!*binding || !binding->implementation() ||
                (binding->flags() & luna::runtime::GenerationBindingFragmentContext) == 0)
                return fail("cross-package verified entry lost its context ABI");
        luna::runtime::RuntimeFragmentBindingSet none;
        if (!luna::runtime::makeRuntimeFragmentBindingSet({}, none, error) ||
            !luna::runtime::makeRuntimeFragmentExecutionContext(none, noneContext, error))
            return fail("cross-package verified None policy failed");
        luna::runtime::RuntimeFragmentCandidateSnapshot candidates;
        if (!luna::runtime::snapshotRuntimeFragmentCandidates(
                pluginGeneration, requirement, candidates, error) || candidates.size() != 1)
            return fail("cross-package verified candidate discovery failed");
        luna::runtime::RuntimeFragmentRef reference;
        if (!luna::runtime::makeOwnedRuntimeFragmentRef(*candidates.at(0),
                requirement, {"", nullptr}, reference, error))
            return fail("host could not choose the verified plugin Fragment");
        std::vector<luna::runtime::RuntimeFragmentRef> selected;
        selected.push_back(std::move(reference));
        luna::runtime::RuntimeFragmentBindingSet one;
        if (!luna::runtime::makeRuntimeFragmentBindingSet(std::move(selected), one, error))
            return fail("cross-package verified One policy failed");
        auto safePoint = runtime.safePoint();
        if (!runtime.activateFragmentBindings(one, safePoint, error) ||
            !luna::runtime::makeRuntimeFragmentExecutionContext(
                runtime.pinFragmentBindings(), oneContext, error))
            return fail("cross-package verified One policy did not activate/pin");
        // Cache reuse must still verify evidence before returning a generation.
        if (luna::driver::loadVerifiedMoonGenerationOnce(runtime, pluginBytes,
                pluginManifest.targetTriple, pluginManifest.dataLayout,
                rejected, error) || rejected)
            return fail("load-once cache bypassed dependency verification");
    }
    for (int iteration = 0; iteration < 16; ++iteration) {
        for (const auto* binding : {&hostBinding, &pluginBinding}) {
            if (invokeContextEntry(binding->implementation(), noneContext.opaque()) != 42 ||
                invokeContextEntry(binding->implementation(), oneContext.opaque()) != 40)
                return fail("verified cross-generation dispatch failed after Runtime teardown");
        }
    }
    return 0;
}

} // namespace canonical_test
