#include "moonir_canonical_test_support.h"

#include "codegen/CodeGenerator.h"
#include "diagnostics/Diagnostic.h"
#include "driver/CompilerPipeline.h"
#include "driver/MoonGeneration.h"
#include "moonir/ContainerModel.h"
#include "moonir/Lowering.h"
#include "moonir/Sealer.h"
#include "moonir/Verifier.h"
#include "runtime/RuntimeFragment.h"
#include "runtime/RuntimeFragmentCompilerBridge.h"
#include "tooling/AnalysisSnapshot.h"

#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
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
    auto refSnapshot = luna::tooling::AnalysisSnapshot::analyzePath(
        (fixtures / "refprep").string());
    if (!refSnapshot.success()) {
        for (const auto& issue : refSnapshot.errors())
            std::cerr << diagnostic::render(issue) << '\n';
        return fail("cross-package Ref/apply source did not analyze");
    }
    moon::LunaLowerer refLowerer;
    auto refModule = refLowerer.lower(
        *refSnapshot.program(), *refSnapshot.symbolTable());
    if (!refModule || !refLowerer.errors().empty()) {
        for (const auto& issue : refLowerer.errors())
            std::cerr << diagnostic::render(issue) << '\n';
        return fail("cross-package Ref/apply source did not lower");
    }
    moon::Sealer refSealer;
    if (!refSealer.sealFunctionBodies(*refModule)) {
        for (const auto& message : refSealer.errors())
            std::cerr << message << '\n';
        return fail("cross-package Ref/apply body did not seal");
    }
    moon::Verifier refPreparationVerifier(
        moon::VerificationPolicy::NativeRefUnitPreparation);
    if (!refPreparationVerifier.verify(*refModule)) {
        for (const auto& diagnostic : refPreparationVerifier.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("cross-package Ref/apply preparation did not verify");
    }
    const auto refManifest = makeManifest(*refModule);
    if (moon::ContainerModelCodec::verifyNativeRefUnitPreparationRoundTrip(
            refManifest, *refModule, error) ||
        error.find("requires verified owner Slot evidence") ==
            std::string::npos)
        return fail("cross-package Ref/apply preview accepted missing owner evidence");
    refModule->dependencySlotPublications = {changedContract};
    std::vector<uint8_t> rejectedPreparedRefBytes{1, 2, 3};
    if (moon::ContainerModelCodec::encodeNativeRefUnitPreparationForTest(
            refManifest, *refModule, rejectedPreparedRefBytes, error) ||
        !rejectedPreparedRefBytes.empty() ||
        error.find("requires verified owner Slot evidence") ==
            std::string::npos)
        return fail("cross-package Ref/apply preparation emitted a changed Slot contract");
    refModule->dependencySlotPublications = {
        otherTarget.localSlotPublication};
    if (moon::ContainerModelCodec::verifyNativeRefUnitPreparationRoundTrip(
            refManifest, *refModule, error) ||
        error.find("target does not match") == std::string::npos)
        return fail("cross-package Ref/apply preview accepted a wrong-target owner");
    refModule->dependencySlotPublications = dependencies;
    const auto refPackageUses = refModule->packageUses;
    refModule->packageUses.clear();
    if (moon::ContainerModelCodec::verifyNativeRefUnitPreparationRoundTrip(
            refManifest, *refModule, error))
        return fail("cross-package Ref/apply preview accepted an undeclared owner");
    refModule->packageUses = refPackageUses;
    const auto refSourcePath = fixtures / "refprep/src/effects.luna";
    std::ifstream refSourceInput(refSourcePath);
    std::string extraRefSource(
        std::istreambuf_iterator<char>{refSourceInput}, {});
    if (extraRefSource.empty())
        return fail("cross-package Ref/apply fixture source is missing");
    extraRefSource +=
        "\nexport runtime fn hold_ref(selected: "
        "RuntimeFragmentRef<host::effects::audit_slot>) -> unit {}\n";
    auto extraRefSnapshot =
        luna::tooling::AnalysisSnapshot::analyzePathWithOverlay(
            (fixtures / "refprep").string(), refSourcePath.string(),
            extraRefSource);
    if (!extraRefSnapshot.success())
        return fail("cross-package extra Ref callable did not analyze");
    moon::LunaLowerer extraRefLowerer;
    auto extraRefModule = extraRefLowerer.lower(
        *extraRefSnapshot.program(), *extraRefSnapshot.symbolTable());
    moon::Sealer extraRefSealer;
    if (!extraRefModule || !extraRefLowerer.errors().empty() ||
        !extraRefSealer.sealFunctionBodies(*extraRefModule))
        return fail("cross-package extra Ref callable did not lower and seal");
    extraRefModule->dependencySlotPublications = dependencies;
    std::vector<uint8_t> rejectedExtraRefBytes{1, 2, 3};
    if (moon::ContainerModelCodec::encodeNativeRefUnitPreparationForTest(
            makeManifest(*extraRefModule), *extraRefModule,
            rejectedExtraRefBytes, error) ||
        !rejectedExtraRefBytes.empty() ||
        error.find("another Ref callable") == std::string::npos)
        return fail("cross-package preparation admitted an extra Ref callable");
    std::vector<uint8_t> refBytes;
    if (!moon::ContainerModelCodec::encodeNativeRefUnitPreparationForTest(
            refManifest, *refModule, refBytes, error) || refBytes.empty()) {
        std::cerr << error << '\n';
        return fail("cross-package Ref/apply owner-backed 0.4 encoding failed");
    }
    auto rejectedPrivateDecode = [&](const std::vector<uint8_t>& bytes,
            const std::string& targetTriple, const std::string& dataLayout,
            const moon::SlotPublicationDependencies& ownerEvidence) {
        moon::ContainerManifest rejectedManifest;
        rejectedManifest.packageId = "unchanged.sentinel";
        moon::Module rejectedModule;
        rejectedModule.name = "unchanged.sentinel";
        return !moon::ContainerModelCodec::decodeNativeRefUnitPreparationForTarget(
                   bytes, targetTriple, dataLayout, ownerEvidence,
                   rejectedManifest, rejectedModule, error) &&
            rejectedManifest.packageId == "unchanged.sentinel" &&
            rejectedModule.name == "unchanged.sentinel" &&
            !rejectedModule.localSlotPublication;
    };
    auto corruptedRefBytes = refBytes;
    corruptedRefBytes.back() ^= 1;
    if (!rejectedPrivateDecode(refBytes, refManifest.targetTriple,
            refManifest.dataLayout, {}) ||
        !rejectedPrivateDecode(refBytes, refManifest.targetTriple,
            refManifest.dataLayout, {changedContract}) ||
        !rejectedPrivateDecode(refBytes, refManifest.targetTriple,
            refManifest.dataLayout, {otherTarget.localSlotPublication}) ||
        !rejectedPrivateDecode(refBytes, refManifest.targetTriple + "-wrong",
            refManifest.dataLayout, dependencies) ||
        !rejectedPrivateDecode(refBytes, refManifest.targetTriple,
            refManifest.dataLayout + "-wrong", dependencies) ||
        !rejectedPrivateDecode(corruptedRefBytes, refManifest.targetTriple,
            refManifest.dataLayout, dependencies))
        return fail("cross-package Ref/apply preparation decoder accepted an invalid artifact");
    moon::ContainerManifest preparedRefManifest;
    moon::Module preparedRefModule;
    if (!moon::ContainerModelCodec::decodeNativeRefUnitPreparationForTarget(
            refBytes, refManifest.targetTriple, refManifest.dataLayout,
            dependencies, preparedRefManifest, preparedRefModule, error)) {
        std::cerr << error << '\n';
        return fail("cross-package Ref/apply owner-backed 0.4 decode failed");
    }
    if (preparedRefModule.formatMinor != moon::RefWireFormatMinor ||
        preparedRefModule.name != refManifest.packageId ||
        preparedRefManifest.packageId != refManifest.packageId ||
        preparedRefModule.dependencySlotPublications.size() != 1)
        return fail("cross-package Ref/apply JIT input was not the verified 0.4 decode");
    moon::ContainerManifest rejectedPublishedRefManifest;
    rejectedPublishedRefManifest.packageId = "unchanged.sentinel";
    moon::Module rejectedPublishedRefModule;
    rejectedPublishedRefModule.name = "unchanged.sentinel";
    if (moon::ContainerModelCodec::decodeContainerForTarget(refBytes,
            refManifest.targetTriple, refManifest.dataLayout,
            rejectedPublishedRefManifest, rejectedPublishedRefModule,
            error, {}, dependencies) ||
        rejectedPublishedRefManifest.packageId != "unchanged.sentinel" ||
        rejectedPublishedRefModule.name != "unchanged.sentinel")
        return fail("ordinary container decoder published cross-package Ref/apply");
    {
        luna::runtime::MoonRuntime runtime;
        luna::runtime::MoonRuntime::PinnedGeneration rejectedGeneration;
        if (luna::driver::loadVerifiedMoonGenerationOnce(runtime, refBytes,
                refManifest.targetTriple, refManifest.dataLayout,
                rejectedGeneration, error, dependencies) || rejectedGeneration ||
            runtime.pin(refManifest.packageId))
            return fail("private Ref/apply artifact entered the public generation loader");
    }
    std::vector<uint8_t> rejectedRefContainer{1, 2, 3};
    if (moon::ContainerModelCodec::encodeContainer(
            refManifest, *refModule, rejectedRefContainer, error,
            {}, moon::RefWireFormatMinor) ||
        !rejectedRefContainer.empty() ||
        error.find("MoonIR verification failed") == std::string::npos)
        return fail("ordinary container encoder published cross-package Ref/apply");
    moon::FunctionDecl* refSource = nullptr;
    for (auto& declaration : preparedRefModule.declarations)
        if (auto* function = dynamic_cast<moon::FunctionDecl*>(
                declaration.get()); function && function->name == "apply_ref")
            refSource = function;
    if (!refSource || !refSource->controlFlow)
        return fail("cross-package Ref/apply decoded module lost its executable function");
    luna::runtime::MoonRuntime::PinnedGeneration refConsumerGeneration;
    std::weak_ptr<LunaPrivateRefUnitApplyLoadedEntry> refEntryWeak;
    std::weak_ptr<luna::driver::PrivatePreparedRefGenerationForTest>
        refPreparationWeak;
    luna::driver::PrivateImportedSlotRefUnitCandidateForTest refCandidate;
    luna::driver::VerifiedMoonRefUnitGeneration verifiedConsumerGeneration;
    luna::driver::VerifiedMoonImportedSlotRefUnitProof verifiedImport;
    luna::driver::VerifiedMoonImportedSlotGeneration verifiedSlotGeneration;
    luna::driver::VerifiedMoonImportedSlotGeneration integratedRefGeneration;
    {
        luna::runtime::MoonRuntime runtime;
        luna::driver::VerifiedMoonSlotOwner rejectedOwner;
        if (luna::driver::loadVerifiedMoonSlotOwner(
                corruptedOwner, hostManifest.targetTriple,
                hostManifest.dataLayout, rejectedOwner, error) || rejectedOwner)
            return fail("corrupt owner artifact issued a private loader proof");
        if (luna::driver::loadVerifiedMoonSlotOwner(
                otherTargetBytes, hostManifest.targetTriple,
                hostManifest.dataLayout, rejectedOwner, error) || rejectedOwner)
            return fail("wrong-target owner artifact issued a private loader proof");
        luna::driver::CompilerPipeline slotlessPipeline;
        std::vector<uint8_t> slotlessBytes;
        if (!slotlessPipeline.compileSourceToMoonIR(
                "package canonical.container.slotless; module effects;",
                "<slotless-owner>") ||
            !moon::ContainerModelCodec::encodeContainer(
                makeManifest(slotlessPipeline.moonModule()),
                slotlessPipeline.moonModule(), slotlessBytes, error))
            return fail("slotless owner artifact did not encode");
        if (luna::driver::loadVerifiedMoonSlotOwner(
                slotlessBytes, hostManifest.targetTriple,
                hostManifest.dataLayout, rejectedOwner, error) || rejectedOwner ||
            error.find("no Slot publication") == std::string::npos)
            return fail("artifact without a local Slot issued an owner proof");
        luna::driver::VerifiedMoonSlotOwner ownerProof;
        if (!luna::driver::loadVerifiedMoonSlotOwner(
                hostBytes, hostManifest.targetTriple,
                hostManifest.dataLayout, ownerProof, error)) {
            std::cerr << error << '\n';
            return fail("private Ref consumer owner loader did not issue proof");
        }
        const uint64_t ownerGenerationId = ownerProof.generationId();
        if (!ownerGenerationId ||
            luna::driver::loadVerifiedMoonSlotOwner(
                hostBytes, hostManifest.targetTriple,
                hostManifest.dataLayout, ownerProof, error) ||
            ownerProof.generationId() != ownerGenerationId)
            return fail("private Slot owner proof was empty or replaced");
        const auto* importedOwnerRecord = preparedRefModule.findDeclaration(
            refSource->controlFlow->runtimeRefApplies.front().slot);
        luna::runtime::MoonRuntime::PinnedGeneration exportedOwnerPin;
        if (!importedOwnerRecord ||
            !ownerProof.pinImportedSlot(
                *importedOwnerRecord, hostManifest.targetTriple,
                hostManifest.dataLayout, exportedOwnerPin, error) ||
            exportedOwnerPin.generationId() != ownerGenerationId)
            return fail("verified owner proof did not pin its imported Slot");
        if (ownerProof.pinImportedSlot(
                *importedOwnerRecord, hostManifest.targetTriple,
                hostManifest.dataLayout, exportedOwnerPin, error) ||
            exportedOwnerPin.generationId() != ownerGenerationId)
            return fail("verified owner proof replaced an existing output pin");
        luna::runtime::MoonRuntime::PinnedGeneration rejectedSlotPin;
        auto changedImportedRecord = *importedOwnerRecord;
        changedImportedRecord.contractId.value += "-wrong";
        if (ownerProof.pinImportedSlot(
                changedImportedRecord, hostManifest.targetTriple,
                hostManifest.dataLayout, rejectedSlotPin, error) || rejectedSlotPin)
            return fail("verified owner proof accepted a changed Slot contract");
        changedImportedRecord = *importedOwnerRecord;
        changedImportedRecord.type.value += "-wrong";
        if (ownerProof.pinImportedSlot(
                changedImportedRecord, hostManifest.targetTriple,
                hostManifest.dataLayout, rejectedSlotPin, error) || rejectedSlotPin)
            return fail("verified owner proof accepted a changed Slot type");
        if (ownerProof.pinImportedSlot(
                *importedOwnerRecord, hostManifest.targetTriple + "-wrong",
                hostManifest.dataLayout, rejectedSlotPin, error) || rejectedSlotPin ||
            rejectedOwner.pinImportedSlot(
                *importedOwnerRecord, hostManifest.targetTriple,
                hostManifest.dataLayout, rejectedSlotPin, error) || rejectedSlotPin)
            return fail("verified owner proof accepted a wrong target or empty proof");
        exportedOwnerPin = {};
        luna::driver::VerifiedMoonRefUnitPreparation rejectedPreparation;
        if (luna::driver::loadVerifiedMoonRefUnitPreparation(
                refBytes, rejectedOwner, refManifest.targetTriple,
                refManifest.dataLayout, rejectedPreparation, error) ||
            rejectedPreparation ||
            luna::driver::loadVerifiedMoonRefUnitPreparation(
                refBytes, ownerProof, refManifest.targetTriple + "-wrong",
                refManifest.dataLayout, rejectedPreparation, error) ||
            rejectedPreparation)
            return fail("unissued or wrong-target owner prepared a Ref consumer");
        auto corruptedConsumer = refBytes;
        corruptedConsumer.back() ^= 1;
        if (luna::driver::loadVerifiedMoonRefUnitPreparation(
                corruptedConsumer, ownerProof, refManifest.targetTriple,
                refManifest.dataLayout, rejectedPreparation, error) ||
            rejectedPreparation)
            return fail("corrupt Ref consumer issued a preparation proof");
        luna::driver::VerifiedMoonRefUnitPreparation consumerPreparation;
        if (!luna::driver::loadVerifiedMoonRefUnitPreparation(
                refBytes, ownerProof, refManifest.targetTriple,
                refManifest.dataLayout, consumerPreparation, error) ||
            consumerPreparation.ownerGenerationId() != ownerGenerationId ||
            luna::driver::loadVerifiedMoonRefUnitPreparation(
                refBytes, ownerProof, refManifest.targetTriple,
                refManifest.dataLayout, consumerPreparation, error) ||
            consumerPreparation.ownerGenerationId() != ownerGenerationId)
            return fail("verified Ref consumer preparation lost or replaced its owner");
        if (luna::driver::loadVerifiedMoonRefUnitGenerationOnce(
                rejectedPreparation, verifiedConsumerGeneration, error) ||
            verifiedConsumerGeneration ||
            !luna::driver::loadVerifiedMoonRefUnitGenerationOnce(
                consumerPreparation, verifiedConsumerGeneration, error)) {
            std::cerr << error << '\n';
            return fail("verified Ref consumer did not load its inert generation");
        }
        const auto loadedFacts = verifiedConsumerGeneration.facts();
        if (!verifiedConsumerGeneration ||
            !verifiedConsumerGeneration.generationId() ||
            verifiedConsumerGeneration.ownerGenerationId() != ownerGenerationId ||
            !verifiedConsumerGeneration.matchesPreparation(consumerPreparation) ||
            !loadedFacts ||
            loadedFacts->functionSymbolId != refSource->symbolId.value ||
            loadedFacts->functionContractId != refSource->contractId.value ||
            luna::driver::loadVerifiedMoonRefUnitGenerationOnce(
                consumerPreparation, verifiedConsumerGeneration, error) ||
            !verifiedConsumerGeneration.matchesPreparation(consumerPreparation))
            return fail("verified Ref consumer generation lost or replaced its proof");
        luna::driver::VerifiedMoonRefUnitPreparation sameArtifactOtherProof;
        if (!luna::driver::loadVerifiedMoonRefUnitPreparation(
                refBytes, ownerProof, refManifest.targetTriple,
                refManifest.dataLayout, sameArtifactOtherProof, error)) {
            std::cerr << error << '\n';
            return fail("same Ref artifact did not issue a separate preparation");
        }
        if (verifiedConsumerGeneration.matchesPreparation(sameArtifactOtherProof) ||
            verifiedConsumerGeneration.matchesPreparation(rejectedPreparation))
            return fail("verified Ref consumer borrowed another preparation lease");
        const luna::runtime::RuntimeSlotRequirement verifiedSlot{
            refSource->controlFlow->runtimeRefApplies.front().slot.symbol.value,
            refSource->controlFlow->runtimeRefApplies.front().slot.contract.value};
        luna::driver::VerifiedMoonImportedSlotRefUnitProof rejectedImport;
        auto wrongVerifiedSlot = verifiedSlot;
        wrongVerifiedSlot.slotId += "-wrong";
        if (luna::driver::pinVerifiedMoonImportedSlotRefUnitProof(
                {}, refSource->symbolId.value, refSource->contractId.value,
                verifiedSlot, rejectedImport, error) || rejectedImport ||
            luna::driver::pinVerifiedMoonImportedSlotRefUnitProof(
                verifiedConsumerGeneration, refSource->symbolId.value + "-wrong",
                refSource->contractId.value, verifiedSlot,
                rejectedImport, error) || rejectedImport ||
            luna::driver::pinVerifiedMoonImportedSlotRefUnitProof(
                verifiedConsumerGeneration, refSource->symbolId.value,
                refSource->contractId.value + "-wrong", verifiedSlot,
                rejectedImport, error) || rejectedImport ||
            luna::driver::pinVerifiedMoonImportedSlotRefUnitProof(
                verifiedConsumerGeneration, refSource->symbolId.value,
                refSource->contractId.value, wrongVerifiedSlot,
                rejectedImport, error) || rejectedImport)
            return fail("verified imported-Slot proof accepted a mismatched identity");
        wrongVerifiedSlot = verifiedSlot;
        wrongVerifiedSlot.contractId += "-wrong";
        if (luna::driver::pinVerifiedMoonImportedSlotRefUnitProof(
                verifiedConsumerGeneration, refSource->symbolId.value,
                refSource->contractId.value, wrongVerifiedSlot,
                rejectedImport, error) || rejectedImport ||
            rejectedImport.candidateFacts())
            return fail("verified imported-Slot proof accepted a changed Slot contract");
        if (!luna::driver::pinVerifiedMoonImportedSlotRefUnitProof(
                verifiedConsumerGeneration, refSource->symbolId.value,
                refSource->contractId.value, verifiedSlot,
                verifiedImport, error)) {
            std::cerr << error << '\n';
            return fail("verified imported-Slot proof did not pin");
        }
        const auto importedCandidate = verifiedImport.candidateFacts();
        if (!verifiedImport ||
            verifiedImport.consumerGenerationId() !=
                verifiedConsumerGeneration.generationId() ||
            verifiedImport.ownerGenerationId() != ownerGenerationId ||
            !verifiedImport.matchesPreparation(consumerPreparation) ||
            verifiedImport.matchesPreparation(sameArtifactOtherProof) ||
            !verifiedImport.facts() ||
            !importedCandidate ||
            importedCandidate->symbolId != refSource->symbolId.value ||
            importedCandidate->contractId != refSource->contractId.value ||
            importedCandidate->linkageName !=
                "__luna_native_ref_unit_entry_v1_" + refSource->symbolId.value ||
            importedCandidate->linkageName == loadedFacts->sourceLinkageName ||
            importedCandidate->slotSymbolId != verifiedSlot.slotId ||
            importedCandidate->slotContractId != verifiedSlot.contractId ||
            importedCandidate->entryAbi !=
                luna::runtime::GenerationEntryAbiRefUnitV1 ||
            luna::driver::pinVerifiedMoonImportedSlotRefUnitProof(
                verifiedConsumerGeneration, refSource->symbolId.value,
                refSource->contractId.value, verifiedSlot,
                verifiedImport, error) ||
            !verifiedImport.matchesPreparation(consumerPreparation))
            return fail("verified imported-Slot proof lost or replaced its owner");
        if (rejectedImport.loadBindingOnce(verifiedSlotGeneration, error) ||
            verifiedSlotGeneration ||
            !verifiedImport.loadBindingOnce(verifiedSlotGeneration, error)) {
            std::cerr << error << '\n';
            return fail("verified imported-Slot binding did not load privately");
        }
        if (!verifiedSlotGeneration ||
            !verifiedSlotGeneration.generationId() ||
            !verifiedSlotGeneration.hasImportedSlotBinding() ||
            verifiedSlotGeneration.hasMetadataCandidate() ||
            !verifiedSlotGeneration.candidateFacts() ||
            verifiedImport.loadBindingOnce(verifiedSlotGeneration, error) ||
            !verifiedSlotGeneration.hasImportedSlotBinding())
            return fail("verified imported-Slot binding lost or replaced its pin");
        {
            auto oldProxy = verifiedSlotGeneration;
            if (!verifiedSlotGeneration.admitMetadataCandidateOnce(error)) {
                std::cerr << error << '\n';
                return fail("verified imported-Slot metadata candidate did not activate");
            }
            if (!verifiedSlotGeneration.hasImportedSlotBinding() ||
                !verifiedSlotGeneration.hasMetadataCandidate() ||
                verifiedSlotGeneration.admitMetadataCandidateOnce(error) ||
                !oldProxy.hasImportedSlotBinding() ||
                oldProxy.hasMetadataCandidate() ||
                oldProxy.admitMetadataCandidateOnce(error))
                return fail("verified imported-Slot candidate replaced or reused a pin");
        }
        if (luna::driver::loadVerifiedMoonImportedSlotRefUnitOnce(
                corruptedConsumer, ownerProof, refManifest.targetTriple,
                refManifest.dataLayout, refSource->symbolId.value,
                refSource->contractId.value, verifiedSlot,
                integratedRefGeneration, error) || integratedRefGeneration ||
            luna::driver::loadVerifiedMoonImportedSlotRefUnitOnce(
                refBytes, rejectedOwner, refManifest.targetTriple,
                refManifest.dataLayout, refSource->symbolId.value,
                refSource->contractId.value, verifiedSlot,
                integratedRefGeneration, error) || integratedRefGeneration ||
            luna::driver::loadVerifiedMoonImportedSlotRefUnitOnce(
                refBytes, ownerProof, refManifest.targetTriple,
                refManifest.dataLayout, refSource->symbolId.value + "-wrong",
                refSource->contractId.value, verifiedSlot,
                integratedRefGeneration, error) || integratedRefGeneration ||
            luna::driver::loadVerifiedMoonImportedSlotRefUnitOnce(
                refBytes, ownerProof, refManifest.targetTriple,
                refManifest.dataLayout, refSource->symbolId.value,
                refSource->contractId.value, wrongVerifiedSlot,
                integratedRefGeneration, error) || integratedRefGeneration)
            return fail("integrated verified Ref loader admitted invalid evidence");
        if (!luna::driver::loadVerifiedMoonImportedSlotRefUnitOnce(
                refBytes, ownerProof, refManifest.targetTriple,
                refManifest.dataLayout, refSource->symbolId.value,
                refSource->contractId.value, verifiedSlot,
                integratedRefGeneration, error)) {
            std::cerr << error << '\n';
            return fail("integrated verified Ref loader did not complete admission");
        }
        const uint64_t integratedGenerationId =
            integratedRefGeneration.generationId();
        if (!integratedRefGeneration.hasTypedCaller() ||
            !integratedRefGeneration.hasImportedSlotBinding() ||
            luna::driver::loadVerifiedMoonImportedSlotRefUnitOnce(
                refBytes, ownerProof, refManifest.targetTriple,
                refManifest.dataLayout, refSource->symbolId.value,
                refSource->contractId.value, verifiedSlot,
                integratedRefGeneration, error) ||
            integratedRefGeneration.generationId() != integratedGenerationId)
            return fail("integrated verified Ref loader replaced its admitted pin");
        sameArtifactOtherProof = {};
        auto compatibleOwnerManifest = hostManifest;
        compatibleOwnerManifest.packageVersion = "0.3.1";
        std::vector<uint8_t> compatibleOwnerBytes;
        luna::driver::VerifiedMoonSlotOwner compatibleOwner;
        if (!moon::ContainerModelCodec::encodeContainer(
                compatibleOwnerManifest, hostPipeline.moonModule(),
                compatibleOwnerBytes, error) ||
            compatibleOwnerBytes == hostBytes ||
            !luna::driver::loadVerifiedMoonSlotOwner(
                compatibleOwnerBytes, hostManifest.targetTriple,
                hostManifest.dataLayout, compatibleOwner, error)) {
            std::cerr << error << '\n';
            return fail("compatible owner revision did not issue a separate proof");
        }
        luna::runtime::MoonRuntime compatibleRuntime;
        luna::runtime::MoonRuntime::StagedGeneration compatibleStage;
        std::weak_ptr<LunaPrivateRefUnitApplyLoadedEntry> compatibleEntry;
        std::weak_ptr<luna::driver::PrivatePreparedRefGenerationForTest>
            compatiblePreparation;
        if (!luna::driver::stagePrivateNativeRefUnitPreparationForTest(
                compatibleRuntime, refBytes, compatibleOwner,
                refManifest.targetTriple, refManifest.dataLayout,
                compatibleStage, compatibleEntry, compatiblePreparation,
                error) || !compatibleStage || compatibleEntry.expired() ||
            compatiblePreparation.expired() ||
            compatibleRuntime.pin(refManifest.packageId)) {
            std::cerr << error << '\n';
            return fail("compatible owner Slot revision did not stage privately");
        }
        luna::runtime::MoonRuntime::StagedGeneration staged;
        if (luna::driver::stagePrivateNativeRefUnitPreparationForTest(
                runtime, refBytes, rejectedOwner, refManifest.targetTriple,
                refManifest.dataLayout, staged,
                refEntryWeak, refPreparationWeak, error) || staged ||
            !refEntryWeak.expired() || !refPreparationWeak.expired() ||
            runtime.pin(refManifest.packageId))
            return fail("private Ref consumer accepted an unissued owner proof");
        if (luna::driver::stagePrivateNativeRefUnitPreparationForTest(
                runtime, refBytes, ownerProof,
                refManifest.targetTriple + "-wrong", refManifest.dataLayout, staged,
                refEntryWeak, refPreparationWeak, error) || staged ||
            !refEntryWeak.expired() || !refPreparationWeak.expired() ||
            runtime.pin(refManifest.packageId))
            return fail("private Ref consumer staged with a wrong-target proof");
        if (!luna::driver::stagePrivateNativeRefUnitPreparationForTest(
                runtime, refBytes, ownerProof,
                refManifest.targetTriple, refManifest.dataLayout, staged,
                refEntryWeak, refPreparationWeak, error)) {
            std::cerr << error << '\n';
            return fail("private Ref consumer generation did not stage");
        }
        if (!staged || staged.moduleId() != refManifest.packageId ||
            staged.contentDigest().size() != 64 || refEntryWeak.expired() ||
            refPreparationWeak.expired() ||
            runtime.pin(refManifest.packageId))
            return fail("private Ref consumer staging published or lost its lease");
        if (!runtime.loadOnce(staged, refConsumerGeneration, error)) {
            std::cerr << error << '\n';
            return fail("private Ref consumer generation did not load");
        }
        if (!refConsumerGeneration ||
            refConsumerGeneration.find(
                refSource->symbolId.value, refSource->contractId.value) ||
            refConsumerGeneration.findRefUnitCandidate(
                refSource->symbolId.value, refSource->contractId.value))
            return fail("private Ref consumer generation published a callable");
        const auto preparedFacts = consumerPreparation.facts();
        if (!consumerPreparation.matchesConsumer(refConsumerGeneration) ||
            !preparedFacts ||
            preparedFacts->functionSymbolId != refSource->symbolId.value ||
            preparedFacts->functionContractId != refSource->contractId.value ||
            rejectedPreparation.matchesConsumer(refConsumerGeneration) ||
            rejectedPreparation.facts())
            return fail("verified Ref preparation lost its consumer identity");
        const luna::runtime::RuntimeSlotRequirement importedSlot{
            refSource->controlFlow->runtimeRefApplies.front().slot.symbol.value,
            refSource->controlFlow->runtimeRefApplies.front().slot.contract.value};
        if (luna::driver::pinPrivateImportedSlotRefUnitCandidateForTest(
                refConsumerGeneration, refPreparationWeak,
                refSource->symbolId.value + "-wrong",
                refSource->contractId.value, importedSlot,
                refCandidate, error) || refCandidate)
            return fail("private imported-Slot candidate accepted a changed source symbol");
        if (luna::driver::pinPrivateImportedSlotRefUnitCandidateForTest(
                refConsumerGeneration, refPreparationWeak,
                refSource->symbolId.value, refSource->contractId.value + "-wrong",
                importedSlot, refCandidate, error) || refCandidate)
            return fail("private imported-Slot candidate accepted a changed source contract");
        auto changedSlot = importedSlot;
        changedSlot.contractId += "-wrong";
        if (luna::driver::pinPrivateImportedSlotRefUnitCandidateForTest(
                refConsumerGeneration, refPreparationWeak,
                refSource->symbolId.value, refSource->contractId.value,
                changedSlot, refCandidate, error) || refCandidate)
            return fail("private imported-Slot candidate accepted a changed Slot contract");
        luna::runtime::MoonRuntime wrongConsumerRuntime;
        luna::runtime::GenerationStagingRequest wrongConsumerRequest;
        wrongConsumerRequest.moduleId = refConsumerGeneration.moduleId();
        wrongConsumerRequest.contentDigest =
            refConsumerGeneration.contentDigest();
        auto preparedLease = refPreparationWeak.lock();
        if (!preparedLease)
            return fail("private Ref preparation lease expired before binding");
        wrongConsumerRequest.moduleLease = std::shared_ptr<const void>(
            preparedLease.get(), [](const void*) {});
        luna::runtime::MoonRuntime::StagedGeneration wrongConsumerStage;
        luna::runtime::MoonRuntime::PinnedGeneration wrongConsumerGeneration;
        if (!wrongConsumerRuntime.stage(wrongConsumerRequest,
                [](const auto&, std::string&) { return true; },
                [](const auto&, auto&, std::string&) { return true; },
                {}, wrongConsumerStage, error) ||
            !wrongConsumerRuntime.loadOnce(
                wrongConsumerStage, wrongConsumerGeneration, error))
            return fail("wrong consumer generation fixture did not load");
        if (luna::driver::pinPrivateImportedSlotRefUnitCandidateForTest(
                wrongConsumerGeneration, refPreparationWeak,
                refSource->symbolId.value, refSource->contractId.value,
                importedSlot, refCandidate, error) || refCandidate)
            return fail("private imported-Slot candidate mixed lease ownership");
        preparedLease.reset();
        if (!luna::driver::pinPrivateImportedSlotRefUnitCandidateForTest(
                refConsumerGeneration, refPreparationWeak,
                refSource->symbolId.value, refSource->contractId.value,
                importedSlot, refCandidate, error)) {
            std::cerr << error << '\n';
            return fail("private imported-Slot candidate did not pin");
        }
        const auto refFacts = refCandidate.facts();
        const auto* sourceRecord = preparedRefModule.findDeclarationById(
            refSource->declarationId);
        const auto* importedRecord = preparedRefModule.findDeclaration(
            refSource->controlFlow->runtimeRefApplies.front().slot);
        if (!refFacts ||
            refFacts->functionSymbolId != refSource->symbolId.value ||
            refFacts->functionContractId != refSource->contractId.value ||
            refFacts->sourceLinkageName != refSource->generatedSymbolName ||
            !sourceRecord ||
            refFacts->functionTypeId != sourceRecord->type.value ||
            refFacts->refTypeId != refSource->params.front().type.value ||
            refFacts->slotSymbolId != importedSlot.slotId ||
            refFacts->slotContractId != importedSlot.contractId ||
            !importedRecord ||
            refFacts->slotTypeId != importedRecord->type.value ||
            refFacts->contextEffect != 1 ||
            refCandidate.consumerGenerationId() !=
                refConsumerGeneration.generationId() ||
            refCandidate.ownerGenerationId() != ownerGenerationId)
            return fail("private imported-Slot candidate lost verified identities");
        luna::runtime::MoonRuntime::PinnedGeneration rejectedPublished;
        if (luna::driver::loadVerifiedMoonGenerationOnce(runtime, refBytes,
                refManifest.targetTriple, refManifest.dataLayout,
                rejectedPublished, error, dependencies) || rejectedPublished)
            return fail("public load-once reused a private Ref consumer generation");
        {
            luna::runtime::MoonRuntime ownerHost;
            luna::runtime::MoonRuntime::StagedGeneration ownerStage;
            if (rejectedOwner.stageInRuntime(
                    ownerHost, ownerStage, error) || ownerStage ||
                !ownerProof.stageInRuntime(
                    ownerHost, ownerStage, error))
                return fail("verified owner proof did not stage independently");
            auto ownerPoint = ownerHost.safePoint();
            if (!ownerHost.activate(ownerStage, ownerPoint, error))
                return fail("independent verified owner did not activate");
            const auto activeOwner = ownerHost.pin(hostManifest.packageId);
            if (!activeOwner ||
                activeOwner.findAll(
                    static_cast<uint32_t>(moon::DeclarationKind::Function) + 1,
                    0).empty() ||
                ownerProof.stageInRuntime(
                    ownerHost, ownerStage, error) || ownerStage)
                return fail("independent verified owner lost its artifact bindings");
        }
        ownerProof = {};
        if (consumerPreparation.ownerGenerationId() != ownerGenerationId)
            return fail("verified Ref preparation lost its owner after loader teardown");
        consumerPreparation = {};
        if (!verifiedConsumerGeneration ||
            verifiedConsumerGeneration.ownerGenerationId() != ownerGenerationId ||
            !verifiedConsumerGeneration.facts())
            return fail("verified Ref consumer generation lost its proof after loader teardown");
        if (!verifiedImport ||
            verifiedImport.ownerGenerationId() != ownerGenerationId ||
            !verifiedImport.facts() ||
            !verifiedImport.candidateFacts())
            return fail("verified imported-Slot proof lost its owner after loader teardown");
        verifiedImport = {};
        verifiedConsumerGeneration = {};
        if (!verifiedSlotGeneration ||
            !verifiedSlotGeneration.hasImportedSlotBinding() ||
            !verifiedSlotGeneration.hasMetadataCandidate() ||
            !verifiedSlotGeneration.candidateFacts())
            return fail("verified imported-Slot binding lost its owner after loader teardown");
        if (refCandidate.ownerGenerationId() != ownerGenerationId)
            return fail("private Ref candidate lost its owner after loader teardown");
    }
    auto refEntry = refEntryWeak.lock();
    if (!refEntry)
        return fail("private Ref consumer generation lost its code after Runtime teardown");
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
    luna::runtime::MoonRuntime::PinnedGeneration ordinaryOwnerPin;
    luna::runtime::RuntimeFragmentRefHandle borrowedRef;
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
        ordinaryOwnerPin = hostGeneration;
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
        luna::runtime::RuntimeFragmentRef borrowedSource;
        if (!luna::runtime::makeOwnedRuntimeFragmentRef(*candidates.at(0),
                requirement, {"", nullptr}, reference, error) ||
            !luna::runtime::makeOwnedRuntimeFragmentRef(*candidates.at(0),
                requirement, {"", nullptr}, borrowedSource, error))
            return fail("host could not choose the verified plugin Fragment");
        if (!luna::runtime::makeRuntimeFragmentRefHandle(
                borrowedSource, requirement, borrowedRef, error))
            return fail("verified plugin Fragment could not enter borrowed Ref ingress");
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
    // Leave the borrowed Ref as the only remaining pin on the plugin generation.
    pluginBinding = {};
    oneContext = {};
    if (!borrowedRef ||
        refEntry->call(nullptr, const_cast<void*>(borrowedRef.opaque())) !=
            LUNA_PRIVATE_REF_UNIT_APPLY_INVALID_CONTEXT_V1_TEST ||
        refEntry->call(noneContext.opaque(), nullptr) !=
            LUNA_PRIVATE_REF_UNIT_APPLY_INVALID_HANDLE_V1_TEST)
        return fail("cross-package Ref/apply borrowed ingress accepted an invalid capability");
    for (int iteration = 0; iteration < 16; ++iteration)
        if (refEntry->call(noneContext.opaque(),
                const_cast<void*>(borrowedRef.opaque())) !=
                LUNA_PRIVATE_REF_UNIT_APPLY_SUCCESS_V1_TEST)
            return fail("cross-package Ref/apply lost its pinned borrowed call");
    refEntry.reset();
    if (!luna::driver::discardPrivateRefUnitTestEntryForTest(
            refPreparationWeak, error) || !refEntryWeak.expired())
        return fail("typed Ref candidate retained the separate test JIT entry");
    refConsumerGeneration = {};
    if (!refCandidate || refCandidate.ownerGenerationId() == 0 ||
        !refCandidate.facts())
        return fail("typed imported-Slot candidate lost its owner after Runtime teardown");
    int32_t candidateStatus = 777;
    luna::runtime::RuntimeFragmentExecutionContext invalidContext;
    luna::runtime::RuntimeFragmentRefHandle invalidRef;
    if (!refCandidate.call(invalidContext, borrowedRef,
            candidateStatus, error) ||
        candidateStatus != LUNA_PRIVATE_REF_UNIT_APPLY_INVALID_CONTEXT_V1_TEST ||
        !refCandidate.call(noneContext, invalidRef,
            candidateStatus, error) ||
        candidateStatus != LUNA_PRIVATE_REF_UNIT_APPLY_INVALID_HANDLE_V1_TEST)
        return fail("typed imported-Slot candidate accepted invalid ingress");
    for (int iteration = 0; iteration < 16; ++iteration)
        if (!refCandidate.call(noneContext, borrowedRef,
                candidateStatus, error) ||
            candidateStatus != LUNA_PRIVATE_REF_UNIT_APPLY_SUCCESS_V1_TEST)
            return fail("typed imported-Slot candidate lost its pinned call");
    if (!verifiedSlotGeneration.hasMetadataCandidate() ||
        verifiedSlotGeneration.hasTypedCaller())
        return fail("verified imported-Slot metadata generation changed before caller installation");
    auto oldMetadata = verifiedSlotGeneration;
    auto metadataPin = oldMetadata.pinCandidateForTest();
    candidateStatus = 777;
    if (!metadataPin ||
        metadataPin.call(noneContext, borrowedRef, candidateStatus, error) ||
        error != "pinned Ref/unit candidate has no typed call adapter" ||
        candidateStatus != 777)
        return fail("metadata-only candidate became callable before installation");
    if (!verifiedSlotGeneration.installTypedCallerOnce(error)) {
        std::cerr << error << '\n';
        return fail("verified imported-Slot typed caller did not activate");
    }
    if (!verifiedSlotGeneration.hasTypedCaller() ||
        verifiedSlotGeneration.installTypedCallerOnce(error) ||
        oldMetadata.installTypedCallerOnce(error) ||
        oldMetadata.hasTypedCaller() ||
        metadataPin.call(noneContext, borrowedRef, candidateStatus, error) ||
        candidateStatus != 777)
        return fail("verified imported-Slot caller reused or changed an old pin");
    luna::runtime::MoonRuntime rejectedHostRuntime;
    luna::runtime::MoonRuntime::StagedGeneration rejectedHostStage;
    if (oldMetadata.stageInRuntimeForTest(
            rejectedHostRuntime, rejectedHostStage, error) ||
        rejectedHostStage)
        return fail("metadata-only Ref proof staged in a host Runtime");
    auto typedPin = verifiedSlotGeneration.pinCandidateForTest();
    if (!typedPin ||
        typedPin.generationId() != verifiedSlotGeneration.generationId())
        return fail("verified imported-Slot caller lost its new generation pin");
    verifiedSlotGeneration = {};
    oldMetadata = {};
    metadataPin = {};
    candidateStatus = 777;
    if (typedPin.call(invalidContext, borrowedRef, candidateStatus, error) ||
        error != "verified Ref/unit parent context is not live" ||
        candidateStatus != 777 ||
        typedPin.call(noneContext, invalidRef, candidateStatus, error) ||
        candidateStatus != 777)
        return fail("verified imported-Slot caller accepted invalid ingress");
    for (int iteration = 0; iteration < 16; ++iteration)
        if (!typedPin.call(noneContext, borrowedRef, candidateStatus, error) ||
            candidateStatus != LUNA_PRIVATE_REF_UNIT_APPLY_SUCCESS_V1_TEST)
            return fail("verified imported-Slot caller lost its pinned call");
    auto integratedPin = integratedRefGeneration.pinCandidateForTest();
    if (!integratedPin ||
        integratedPin.generationId() != integratedRefGeneration.generationId())
        return fail("integrated verified Ref loader lost its candidate pin");
    {
        luna::runtime::MoonRuntime conflictingHost;
        luna::runtime::MoonRuntime::PinnedGeneration hostOwner;
        luna::runtime::MoonRuntime::StagedGeneration rejectedStage;
        if (!luna::driver::loadVerifiedMoonGenerationOnce(
                conflictingHost, hostBytes, hostManifest.targetTriple,
                hostManifest.dataLayout, hostOwner, error) ||
            integratedRefGeneration.stageInRuntimeForTest(
                conflictingHost, rejectedStage, error) || rejectedStage ||
            integratedRefGeneration.stageInRuntimeForTest(
                conflictingHost, rejectedStage, error, hostOwner) ||
            rejectedStage ||
            error != "private Ref/unit host staging requires its exact verified owner")
            return fail("private host Ref staging accepted a separate owner module");
    }
    {
        luna::runtime::MoonRuntime activationRaceHost;
        luna::runtime::MoonRuntime::StagedGeneration staged;
        luna::runtime::MoonRuntime::PinnedGeneration hostOwner;
        if (!integratedRefGeneration.stageInRuntimeForTest(
                activationRaceHost, staged, error) ||
            !luna::driver::loadVerifiedMoonGenerationOnce(
                activationRaceHost, hostBytes, hostManifest.targetTriple,
                hostManifest.dataLayout, hostOwner, error))
            return fail("private host Ref activation conflict fixture did not load");
        auto safePoint = activationRaceHost.safePoint();
        if (activationRaceHost.activate(staged, safePoint, error) ||
            error != "generation conflicts with an active module" ||
            !staged ||
            activationRaceHost.activeGenerationId(
                hostOwner.moduleId()) != hostOwner.generationId())
            return fail("private host Ref activation ignored a late owner module");
    }
    luna::runtime::MoonRuntime::PinnedRefUnitCandidate hostRuntimePin;
    {
        luna::runtime::MoonRuntime hostRuntime;
        luna::driver::VerifiedMoonImportedSlotGeneration emptyImport;
        luna::runtime::MoonRuntime::StagedGeneration staged;
        if (emptyImport.stageInRuntimeForTest(hostRuntime, staged, error) ||
            staged ||
            !integratedRefGeneration.stageInRuntimeForTest(
                hostRuntime, staged, error)) {
            std::cerr << error << '\n';
            return fail("verified Ref proof did not stage in a host Runtime");
        }
        luna::runtime::MoonRuntime::PinnedGeneration rejectedLoaded;
        if (hostRuntime.loadOnce(staged, rejectedLoaded, error) ||
            rejectedLoaded || !staged ||
            error != "callable Ref/unit candidate requires explicit activation")
            return fail("host Ref candidate bypassed explicit activation");
        auto safePoint = hostRuntime.safePoint();
        if (!hostRuntime.activate(staged, safePoint, error)) {
            std::cerr << error << '\n';
            return fail("verified host Ref candidate did not activate");
        }
        const auto hostFacts = integratedRefGeneration.candidateFacts();
        if (!hostFacts)
            return fail("verified host Ref candidate lost its source facts");
        const auto hostGeneration = hostRuntime.pin(hostFacts->packageId);
        const auto hostSlot = hostGeneration.find(
            hostFacts->slotSymbolId, hostFacts->slotContractId);
        hostRuntimePin = hostGeneration.findRefUnitCandidate(
            hostFacts->symbolId, hostFacts->contractId);
        if (!hostGeneration || !hostSlot || hostSlot.flags() != 0 ||
            !hostRuntimePin ||
            hostRuntimePin.generationId() != hostGeneration.generationId() ||
            integratedRefGeneration.stageInRuntimeForTest(
                hostRuntime, staged, error) || staged)
            return fail("host Ref admission lost its proof or allowed replacement");
        luna::runtime::MoonRuntime::PinnedGeneration rejectedHostOwner;
        if (luna::driver::loadVerifiedMoonGenerationOnce(
                hostRuntime, hostBytes, hostManifest.targetTriple,
                hostManifest.dataLayout, rejectedHostOwner, error) ||
            rejectedHostOwner ||
            error != "generation conflicts with an active module" ||
            hostRuntime.activeGenerationId(hostFacts->packageId) !=
                hostGeneration.generationId())
            return fail("private host owner loaded across an active Ref candidate");
        luna::runtime::GenerationStagingRequest inertRequest;
        inertRequest.moduleId = hostFacts->packageId;
        inertRequest.contentDigest = hostGeneration.contentDigest();
        inertRequest.materializationKey = hostGeneration.materializationKey();
        inertRequest.moduleLease = std::make_shared<int>(1);
        luna::runtime::MoonRuntime::StagedGeneration inertStage;
        if (!hostRuntime.stage(
                inertRequest,
                [](const auto&, std::string&) { return true; },
                [](const auto&, auto&, std::string&) { return true; },
                {}, inertStage, error))
            return fail("host Ref rollback fixture could not stage an inert generation");
        auto replacePoint = hostRuntime.safePoint();
        if (!hostRuntime.activate(inertStage, replacePoint, error))
            return fail("host Ref rollback fixture could not activate an inert generation");
        const auto inertActive = hostRuntime.pin(hostFacts->packageId);
        if (!inertActive ||
            inertActive.find(hostFacts->slotSymbolId,
                             hostFacts->slotContractId) ||
            inertActive.findRefUnitCandidate(
                hostFacts->symbolId, hostFacts->contractId) ||
            hostRuntimePin.generationId() == inertActive.generationId())
            return fail("host Ref replacement left the candidate active");
        candidateStatus = 777;
        if (!hostRuntimePin.call(noneContext, borrowedRef,
                                 candidateStatus, error) ||
            candidateStatus != LUNA_PRIVATE_REF_UNIT_APPLY_SUCCESS_V1_TEST)
            return fail("host Ref candidate pin failed across replacement");
        auto rollbackPoint = hostRuntime.safePoint();
        if (!hostRuntime.rollback(hostFacts->packageId,
                                  hostGeneration.generationId(),
                                  rollbackPoint, error))
            return fail("host Ref candidate generation did not roll back");
        const auto restored = hostRuntime.pin(hostFacts->packageId);
        const auto restoredCandidate = restored.findRefUnitCandidate(
            hostFacts->symbolId, hostFacts->contractId);
        if (!restored || !restoredCandidate ||
            !restored.find(hostFacts->slotSymbolId,
                           hostFacts->slotContractId) ||
            restored.generationId() != hostGeneration.generationId() ||
            restoredCandidate.generationId() != hostRuntimePin.generationId())
            return fail("host Ref rollback restored the wrong candidate");
        candidateStatus = 777;
        if (!restoredCandidate.call(noneContext, borrowedRef,
                                    candidateStatus, error) ||
            candidateStatus != LUNA_PRIVATE_REF_UNIT_APPLY_SUCCESS_V1_TEST)
            return fail("host Ref rollback did not restore the callable candidate");
    }
    luna::runtime::MoonRuntime::PinnedRefUnitCandidate pairedOwnerPin;
    {
        luna::runtime::MoonRuntime pairedHost;
        luna::runtime::MoonRuntime::StagedGeneration ownerStage;
        luna::runtime::MoonRuntime::StagedGeneration candidateStage;
        luna::runtime::MoonRuntime::StagedGeneration alternateOwner;
        if (!integratedRefGeneration.stageOwnerInRuntime(
                pairedHost, ownerStage, error))
            return fail("paired verified owner did not stage");
        if (!integratedRefGeneration.verifyStagedOwnerInRuntime(
                pairedHost, ownerStage, error))
            return fail("verified staged owner failed read-only preflight");
        {
            luna::runtime::MoonRuntime foreignHost;
            luna::runtime::MoonRuntime::StagedGeneration foreignOwner;
            if (!integratedRefGeneration.stageOwnerInRuntime(
                    foreignHost, foreignOwner, error) ||
                integratedRefGeneration.verifyStagedOwnerInRuntime(
                    pairedHost, foreignOwner, error) ||
                integratedRefGeneration.stageWithStagedOwnerInRuntimeForTest(
                    pairedHost, candidateStage, error, foreignOwner) ||
                candidateStage ||
                error != "private Ref/unit host staging requires its exact staged owner")
                return fail("foreign staged owner impersonated the paired owner");
        }
        {
            luna::driver::VerifiedMoonSlotOwner separateProof;
            if (!luna::driver::loadVerifiedMoonSlotOwner(
                    hostBytes, hostManifest.targetTriple,
                    hostManifest.dataLayout, separateProof, error) ||
                !separateProof.stageInRuntime(
                    pairedHost, alternateOwner, error) ||
                integratedRefGeneration.verifyStagedOwnerInRuntime(
                    pairedHost, alternateOwner, error) ||
                integratedRefGeneration.stageWithStagedOwnerInRuntimeForTest(
                    pairedHost, candidateStage, error, alternateOwner) ||
                candidateStage ||
                error != "private Ref/unit host staging requires its exact staged owner")
                return fail("separate staged owner proof impersonated the paired owner");
        }
        if (!integratedRefGeneration.stageWithStagedOwnerInRuntimeForTest(
                pairedHost, candidateStage, error, ownerStage))
            return fail("verified consumer did not stage against its staged owner");
        auto earlyPoint = pairedHost.safePoint();
        if (pairedHost.activate(candidateStage, earlyPoint, error) ||
            error != "generation requires a different active module generation" ||
            !candidateStage ||
            pairedHost.activeGenerationId(hostManifest.packageId) != 0)
            return fail("paired consumer activated before its owner");
        auto wrongPairPoint = pairedHost.safePoint();
        if (pairedHost.activateTogether(
                alternateOwner, candidateStage, wrongPairPoint, error) ||
            error != "generation requires a different active module generation" ||
            !alternateOwner || !candidateStage ||
            pairedHost.activeGenerationId(hostManifest.packageId) != 0)
            return fail("paired consumer activated with a different owner stage");
        const auto ownerId = ownerStage.generationId();
        const auto consumerId = candidateStage.generationId();
        auto pairPoint = pairedHost.safePoint();
        if (!pairedHost.activateTogether(
                ownerStage, candidateStage, pairPoint, error))
            return fail("verified owner and consumer did not activate together");
        const auto facts = integratedRefGeneration.candidateFacts();
        if (!facts)
            return fail("paired verified consumer lost its facts");
        const auto paired = pairedHost.pinTogether(
            hostManifest.packageId, facts->packageId);
        pairedOwnerPin = paired.second.findRefUnitCandidate(
            facts->symbolId, facts->contractId);
        if (!paired.first || !paired.second || !pairedOwnerPin ||
            paired.first.generationId() != ownerId ||
            paired.second.generationId() != consumerId ||
            !paired.first.find(facts->slotSymbolId,
                               facts->slotContractId))
            return fail("paired verified publication lost owner or candidate");
        candidateStatus = 777;
        if (!pairedOwnerPin.call(noneContext, borrowedRef,
                                 candidateStatus, error) ||
            candidateStatus != LUNA_PRIVATE_REF_UNIT_APPLY_SUCCESS_V1_TEST)
            return fail("paired verified Ref candidate lost its typed call");
    }
    candidateStatus = 777;
    if (!pairedOwnerPin.call(noneContext, borrowedRef,
                             candidateStatus, error) ||
        candidateStatus != LUNA_PRIVATE_REF_UNIT_APPLY_SUCCESS_V1_TEST)
        return fail("paired Ref candidate did not survive host teardown");
    pairedOwnerPin = {};
    luna::runtime::MoonRuntime::PinnedRefUnitCandidate ownerBoundPin;
    {
        luna::runtime::MoonRuntime projectedHost;
        luna::driver::VerifiedMoonImportedSlotGeneration emptyImport;
        luna::runtime::MoonRuntime::StagedGeneration ownerStage;
        if (emptyImport.stageOwnerInRuntime(
                projectedHost, ownerStage, error) || ownerStage ||
            !integratedRefGeneration.stageOwnerInRuntime(
                projectedHost, ownerStage, error))
            return fail("verified owner artifact did not stage privately");
        auto ownerPoint = projectedHost.safePoint();
        if (!projectedHost.activate(ownerStage, ownerPoint, error))
            return fail("verified owner artifact did not activate");
        const auto facts = integratedRefGeneration.candidateFacts();
        if (!facts)
            return fail("verified owner artifact lost its consumer facts");
        const auto hostOwner = projectedHost.pin(hostManifest.packageId);
        const auto hostSlot = hostOwner.find(
            facts->slotSymbolId, facts->slotContractId);
        if (!hostOwner || !hostSlot ||
            hostSlot.flags() !=
                luna::runtime::GenerationBindingPublicControl ||
            !integratedRefGeneration.verifyActiveOwnerInRuntime(
                projectedHost, hostOwner, error) ||
            emptyImport.verifyActiveOwnerInRuntime(
                projectedHost, hostOwner, error))
            return fail("verified owner artifact lost its public Slot");
        if (!integratedRefGeneration.verifyActiveOwnerCandidateMutationForTest(
                projectedHost, hostOwner, [](auto&) {}, error))
            return fail("exact host candidate request failed proof verification");
        if (integratedRefGeneration.verifyActiveOwnerCandidateMutationForTest(
                projectedHost, hostOwner,
                [](auto& request) {
                    request.refUnitCandidates.front().slotTypeId =
                        "type:forged-slot";
                }, error) ||
            error != "verified Ref/unit host candidate request lost its proof")
            return fail("forged Ref/unit facts passed host request verification");
        if (integratedRefGeneration.verifyActiveOwnerCandidateMutationForTest(
                projectedHost, hostOwner,
                [](auto& request) {
                    ++request.requiredActiveModules.front().generationId;
                }, error) ||
            error != "verified Ref/unit host candidate request lost its proof")
            return fail("forged owner dependency passed host request verification");
        if (integratedRefGeneration.verifyActiveOwnerCandidateMutationForTest(
                projectedHost, hostOwner,
                [](auto& request) {
                    request.refUnitCaller = [](
                        const std::string&, const std::string&,
                        const luna::runtime::RuntimeFragmentExecutionContext&,
                        const luna::runtime::RuntimeFragmentRefHandle&,
                        int32_t&, std::string&) { return true; };
                }, error) ||
            error != "verified Ref/unit host candidate request lost its proof" ||
            projectedHost.activeGenerationId(facts->packageId) != 0)
            return fail("forged Ref/unit caller passed host request verification");
        if (integratedRefGeneration.verifyAlternateHostCandidateCallerForTest(
                projectedHost, hostOwner, error) ||
            error != "verified Ref/unit host candidate request lost its proof" ||
            projectedHost.activeGenerationId(facts->packageId) != 0)
            return fail("alternate same-type Ref/unit caller passed host request verification");
        size_t nonSlotRows = 0;
        for (uint32_t kind = 1;
             kind <= static_cast<uint32_t>(moon::DeclarationKind::Slot) + 1;
             ++kind) {
            const auto originalRows = ordinaryOwnerPin.findAll(kind, 0);
            const auto hostRows = hostOwner.findAll(kind, 0);
            if (originalRows.size() != hostRows.size())
                return fail("verified host owner lost an artifact binding kind");
            for (const auto& original : originalRows) {
                const auto copied = hostOwner.find(
                    original.symbolId(), original.contractId());
                if (!copied || copied.declarationKind() != kind ||
                    copied.flags() != original.flags() ||
                    copied.entryAbi() != original.entryAbi() ||
                    static_cast<bool>(copied.implementation()) !=
                        static_cast<bool>(original.implementation()))
                    return fail("verified host owner changed an artifact binding");
                if (kind !=
                    static_cast<uint32_t>(moon::DeclarationKind::Slot) + 1)
                    ++nonSlotRows;
            }
        }
        if (nonSlotRows == 0)
            return fail("verified host owner fixture did not exercise full publication");
        const auto hostEntryBinding = hostOwner.find(
            hostEntry->symbolId.value, hostEntry->contractId.value);
        if (!hostEntryBinding ||
            invokeContextEntry(hostEntryBinding.implementation(),
                               noneContext.opaque()) != 42)
            return fail("verified host owner lost its published callable entry");
        luna::runtime::MoonRuntime::StagedGeneration candidateStage;
        {
            luna::driver::VerifiedMoonSlotOwner separateProof;
            luna::runtime::MoonRuntime separateHost;
            luna::runtime::MoonRuntime::StagedGeneration separateStage;
            if (!luna::driver::loadVerifiedMoonSlotOwner(
                    hostBytes, hostManifest.targetTriple,
                    hostManifest.dataLayout, separateProof, error) ||
                !separateProof.stageInRuntime(
                    separateHost, separateStage, error))
                return fail("separate verified owner proof did not stage");
            auto separatePoint = separateHost.safePoint();
            if (!separateHost.activate(separateStage, separatePoint, error))
                return fail("separate verified owner proof did not activate");
            const auto separateOwner = separateHost.pin(hostManifest.packageId);
            if (!separateOwner ||
                separateOwner.contentDigest() != hostOwner.contentDigest() ||
                separateOwner.materializationKey() !=
                    hostOwner.materializationKey() ||
                integratedRefGeneration.verifyActiveOwnerInRuntime(
                    separateHost, separateOwner, error) ||
                integratedRefGeneration.stageInRuntimeForTest(
                    separateHost, candidateStage, error, separateOwner) ||
                candidateStage ||
                error !=
                    "private Ref/unit host staging requires its exact verified owner")
                return fail("separate verified owner proof impersonated consumer owner");
        }
        {
            luna::runtime::MoonRuntime foreignHost;
            luna::runtime::MoonRuntime::StagedGeneration foreignStage;
            if (!integratedRefGeneration.stageOwnerInRuntime(
                    foreignHost, foreignStage, error))
                return fail("second verified owner artifact did not stage");
            auto foreignPoint = foreignHost.safePoint();
            if (!foreignHost.activate(foreignStage, foreignPoint, error))
                return fail("second verified owner artifact did not activate");
            const auto foreignOwner = foreignHost.pin(hostManifest.packageId);
            if (!foreignOwner ||
                foreignOwner.generationId() != hostOwner.generationId() ||
                foreignOwner.sameGeneration(hostOwner) ||
                foreignOwner.contentDigest() != hostOwner.contentDigest() ||
                foreignOwner.materializationKey() !=
                    hostOwner.materializationKey() ||
                integratedRefGeneration.verifyActiveOwnerInRuntime(
                    projectedHost, foreignOwner, error) ||
                integratedRefGeneration.stageInRuntimeForTest(
                    projectedHost, candidateStage, error, foreignOwner) ||
                candidateStage ||
                error != "private Ref/unit host staging requires its exact verified owner")
                return fail("foreign host pin impersonated the active verified owner");
        }
        if (integratedRefGeneration.stageInRuntimeForTest(
                projectedHost, candidateStage, error) || candidateStage ||
            !integratedRefGeneration.stageInRuntimeForTest(
                projectedHost, candidateStage, error, hostOwner))
            return fail("exact host owner did not admit the private Ref candidate");
        luna::runtime::GenerationStagingRequest alternateOwner;
        alternateOwner.moduleId = hostOwner.moduleId();
        alternateOwner.contentDigest = hostOwner.contentDigest();
        alternateOwner.materializationKey = "fixture:alternate-owner";
        alternateOwner.moduleLease = std::make_shared<int>(2);
        luna::runtime::MoonRuntime::StagedGeneration alternateStage;
        if (!projectedHost.stage(
                alternateOwner,
                [](const auto&, std::string&) { return true; },
                [](const auto&, auto&, std::string&) { return true; },
                {}, alternateStage, error))
            return fail("private owner replacement fixture did not stage");
        const auto alternateId = alternateStage.generationId();
        auto replacePoint = projectedHost.safePoint();
        if (!projectedHost.activate(alternateStage, replacePoint, error))
            return fail("private owner replacement fixture did not activate");
        if (integratedRefGeneration.verifyActiveOwnerInRuntime(
                projectedHost, hostOwner, error) ||
            error != "verified Ref/unit owner preflight requires its exact active owner")
            return fail("stale host owner passed read-only preflight");
        auto blockedCandidatePoint = projectedHost.safePoint();
        if (projectedHost.activate(
                candidateStage, blockedCandidatePoint, error) ||
            error != "generation requires a different active module generation" ||
            !candidateStage ||
            projectedHost.activeGenerationId(facts->packageId) != 0)
            return fail("private Ref candidate ignored a changed host owner");
        auto restoreOwnerPoint = projectedHost.safePoint();
        if (!projectedHost.rollback(hostOwner.moduleId(),
                                    hostOwner.generationId(),
                                    restoreOwnerPoint, error))
            return fail("private host owner did not roll back for Ref activation");
        if (!integratedRefGeneration.verifyActiveOwnerInRuntime(
                projectedHost, hostOwner, error))
            return fail("restored host owner failed read-only preflight");
        auto candidatePoint = projectedHost.safePoint();
        if (!projectedHost.activate(candidateStage, candidatePoint, error))
            return fail("private Ref candidate did not activate with exact host owner");
        ownerBoundPin = projectedHost.pin(facts->packageId)
            .findRefUnitCandidate(facts->symbolId, facts->contractId);
        if (!ownerBoundPin)
            return fail("owner-bound private Ref candidate was not pinned");
        auto blockedOwnerPoint = projectedHost.safePoint();
        if (projectedHost.rollback(hostOwner.moduleId(), alternateId,
                                   blockedOwnerPoint, error) ||
            error != "generation replacement breaks an active module dependency" ||
            projectedHost.activeGenerationId(hostOwner.moduleId()) !=
                hostOwner.generationId())
            return fail("private host owner switched under an active Ref candidate");
        candidateStatus = 777;
        if (!ownerBoundPin.call(noneContext, borrowedRef,
                                 candidateStatus, error) ||
            candidateStatus != LUNA_PRIVATE_REF_UNIT_APPLY_SUCCESS_V1_TEST)
            return fail("owner-bound private Ref candidate lost its typed call");
    }
    integratedRefGeneration = {};
    candidateStatus = 777;
    if (integratedPin.call(invalidContext, borrowedRef,
            candidateStatus, error) ||
        error != "verified Ref/unit parent context is not live" ||
        candidateStatus != 777 ||
        integratedPin.call(noneContext, invalidRef,
            candidateStatus, error) || candidateStatus != 777)
        return fail("integrated verified Ref candidate accepted invalid ingress");
    for (int iteration = 0; iteration < 16; ++iteration)
        if (!integratedPin.call(noneContext, borrowedRef,
                candidateStatus, error) ||
            candidateStatus != LUNA_PRIVATE_REF_UNIT_APPLY_SUCCESS_V1_TEST)
            return fail("integrated verified Ref candidate lost its pinned call");
    integratedPin = {};
    candidateStatus = 777;
    if (!ownerBoundPin.call(noneContext, borrowedRef,
                             candidateStatus, error) ||
        candidateStatus != LUNA_PRIVATE_REF_UNIT_APPLY_SUCCESS_V1_TEST)
        return fail("owner-bound Ref pin lost its caller after host teardown");
    ownerBoundPin = {};
    candidateStatus = 777;
    if (hostRuntimePin.call(invalidContext, borrowedRef,
            candidateStatus, error) ||
        error != "verified Ref/unit parent context is not live" ||
        candidateStatus != 777 ||
        hostRuntimePin.call(noneContext, invalidRef,
            candidateStatus, error) || candidateStatus != 777)
        return fail("host Runtime Ref candidate accepted invalid ingress");
    for (int iteration = 0; iteration < 16; ++iteration)
        if (!hostRuntimePin.call(noneContext, borrowedRef,
                candidateStatus, error) ||
            candidateStatus != LUNA_PRIVATE_REF_UNIT_APPLY_SUCCESS_V1_TEST)
            return fail("host Runtime Ref candidate lost its pinned call");
    refCandidate = {};
    if (!refEntryWeak.expired() || !refPreparationWeak.expired())
        return fail("private Ref consumer candidate retained code after pin release");
    return 0;
}

} // namespace canonical_test
