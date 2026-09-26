#include "ContainerModel.h"
#include "ContainerModelInternal.h"
#include "Verifier.h"

#include <algorithm>
#include <map>
#include <unordered_set>
#include <utility>

namespace moon {

using namespace container_detail;

namespace {
bool validateSlotDependencies(const ContainerManifest& manifest, const Module& module,
                              const SlotPublicationDependencies& dependencies,
                              std::string& error, const ContainerLimits& limits) {
    if (dependencies.size() > limits.maximumTableRows) {
        error = "Moon Container Slot evidence exceeds the configured row limit";
        return false;
    }
    std::unordered_set<std::string> owners;
    for (const auto& evidence : dependencies) {
        if (!evidence || !owners.insert(evidence->ownerPackageId()).second) {
            error = "Moon Container Slot evidence is null or has duplicate owners";
            return false;
        }
        if (evidence->targetTriple() != manifest.targetTriple ||
            evidence->dataLayout() != manifest.dataLayout) {
            error = "Moon Container Slot evidence target does not match the consumer";
            return false;
        }
        if (evidence->ownerPackageId() == module.name ||
            !std::any_of(module.packageUses.begin(), module.packageUses.end(),
                [&](const auto& use) {
                    return use.ownerPackageId == module.name &&
                        use.packageId == evidence->ownerPackageId();
                })) {
            error = "Moon Container Slot evidence owner is not a direct dependency";
            return false;
        }
    }
    return true;
}
} // namespace

bool ContainerModelCodec::encodeContainer(
    const ContainerManifest& manifest, const Module& module,
    std::vector<uint8_t>& output, std::string& error,
    const ContainerLimits& limits) {
    output.clear();
    if (module.name != manifest.packageId ||
        featureBits(module.features) != featureBits(manifest.features)) {
        error = "Moon Container manifest disagrees with its MoonIR module";
        return false;
    }
    Verifier verifier;
    if (!verifier.verify(module)) {
        error = verifier.errors().empty()
            ? "Moon Container MoonIR verification failed"
            : "Moon Container MoonIR verification failed: " +
                verifier.errors().front().message;
        return false;
    }

    Module projection;
    if (!buildConcreteProjection(
            manifest, module, projection, error)) return false;
    // Source composites still have compiler-authoritative SlotDecls. This
    // evidence is used only while verifying the concrete export projection;
    // artifact consumers must obtain it from independently verified owners.
    std::map<std::string, std::vector<DeclarationRecord>> sourceSlots;
    for (const auto& executable : module.declarations) {
        const auto* slot = dynamic_cast<const SlotDecl*>(executable.get());
        if (!slot || !slot->isExported || slot->packageId == module.name) continue;
        if (!std::any_of(module.packageUses.begin(), module.packageUses.end(),
                [&](const auto& use) {
                    return use.ownerPackageId == module.name &&
                        use.packageId == slot->packageId;
                })) continue;
        const auto* record = projection.findDeclaration(
            DeclarationRef{slot->symbolId, slot->contractId});
        if (record && record->id.rfind(slot->packageId + "::", 0) == 0)
            sourceSlots[slot->packageId].push_back(*record);
    }
    for (auto& group : sourceSlots) {
        const auto& owner = group.first;
        auto& slots = group.second;
        if (std::any_of(projection.dependencySlotPublications.begin(),
                projection.dependencySlotPublications.end(), [&](const auto& evidence) {
                    return evidence && evidence->ownerPackageId() == owner;
                })) continue;
        projection.dependencySlotPublications.push_back(
            std::shared_ptr<const SlotPublicationEvidence>(new SlotPublicationEvidence(
                owner, manifest.targetTriple, manifest.dataLayout, std::move(slots))));
    }
    if (!validateSlotDependencies(manifest, projection,
            projection.dependencySlotPublications, error, limits)) return false;
    const auto* entry = manifest.entrypoint.empty()
        ? nullptr : findDeclarationRecord(projection, manifest.entrypoint);
    if ((manifest.packageKind == ContainerPackageKind::Application &&
         (!entry || entry->kind != DeclarationKind::Function)) ||
        (manifest.packageKind == ContainerPackageKind::Library && entry)) {
        error = "Moon Container entrypoint does not survive the concrete projection; "
                "generic recipes are not executable container entries";
        return false;
    }
    Verifier projectionVerifier;
    if (!projectionVerifier.verify(projection)) {
        error = projectionVerifier.errors().empty()
            ? "Moon Container concrete projection verification failed"
            : "Moon Container concrete projection verification failed: " +
                projectionVerifier.errors().front().message;
        return false;
    }

    std::vector<ContainerSection> sections;
    const auto append = [&](ContainerSectionId id,
                            auto encode) -> bool {
        ContainerSection section;
        section.id = static_cast<uint32_t>(id);
        if (!encode(section.payload)) return false;
        sections.push_back(std::move(section));
        return true;
    };
    if (!append(ContainerSectionId::Manifest, [&](auto& payload) {
            return encodeManifest(manifest, payload, error, limits);
        }) ||
        !append(ContainerSectionId::Type, [&](auto& payload) {
            return encodeTypes(projection, payload, error, limits);
        }) ||
        !append(ContainerSectionId::Symbol, [&](auto& payload) {
            return encodeSymbols(projection, payload, error, limits);
        }) ||
        !append(ContainerSectionId::Contract, [&](auto& payload) {
            return encodeContracts(projection, payload, error, limits);
        }) ||
        !append(ContainerSectionId::Code, [&](auto& payload) {
            return encodeCodeRows(
                module, projection, payload, error, limits);
        }) ||
        !append(ContainerSectionId::Imports, [&](auto& payload) {
            return encodeImports(projection, payload, error, limits);
        }) ||
        !append(ContainerSectionId::Exports, [&](auto& payload) {
            return encodeExports(projection, payload, error, limits);
        }) ||
        !append(ContainerSectionId::Sysmeta, [&](auto& payload) {
            return encodeSysmeta(projection, payload, error, limits);
        })) {
        output.clear();
        return false;
    }
    if (!ContainerWriter::encode(std::move(sections), output, error, limits))
        return false;
    // Self-verify while the already-verified source projection's transient
    // dependency facts are still available. They never escape this encoding
    // operation; runtime consumers still need independent owner artifacts.
    ContainerManifest verifiedManifest;
    Module verifiedModule;
    if (!decodeContainerForTarget(output, manifest.targetTriple, manifest.dataLayout,
            verifiedManifest, verifiedModule, error, limits,
            projection.dependencySlotPublications)) {
        error = "generated Moon Container failed self-verification: " + error;
        output.clear();
        return false;
    }
    return true;
}

bool ContainerModelCodec::decodeContainer(
    const std::vector<uint8_t>& input, ContainerManifest& manifest,
    Module& module, std::string& error, const ContainerLimits& limits,
    const SlotPublicationDependencies& dependencies) {
    ContainerReader reader;
    if (!reader.parse(input, error, limits)) return false;
    const auto section = [&](ContainerSectionId id)
        -> const std::vector<uint8_t>* {
        const auto* found = reader.find(static_cast<uint32_t>(id));
        return found ? &found->payload : nullptr;
    };
    const auto* manifestBytes = section(ContainerSectionId::Manifest);
    const auto* typeBytes = section(ContainerSectionId::Type);
    const auto* symbolBytes = section(ContainerSectionId::Symbol);
    const auto* contractBytes = section(ContainerSectionId::Contract);
    const auto* codeBytes = section(ContainerSectionId::Code);
    const auto* importBytes = section(ContainerSectionId::Imports);
    const auto* exportBytes = section(ContainerSectionId::Exports);
    const auto* sysmetaBytes = section(ContainerSectionId::Sysmeta);
    if (!manifestBytes || !typeBytes || !symbolBytes || !contractBytes ||
        !codeBytes || !importBytes || !exportBytes || !sysmetaBytes) {
        error = "Moon Container is missing a required model section";
        return false;
    }

    ContainerManifest decodedManifest;
    Module decodedModule;
    if (!decodeManifest(
            *manifestBytes, decodedManifest, error, limits)) return false;
    decodedModule.formatMajor = reader.formatMajor();
    decodedModule.formatMinor = reader.formatMinor();
    decodedModule.name = decodedManifest.packageId;
    decodedModule.isPackage = true;
    decodedModule.features = decodedManifest.features;
    if (!decodeTypes(*typeBytes, decodedModule, error, limits) ||
        !decodeDeclarations(
            *symbolBytes, *contractBytes, *sysmetaBytes,
            decodedModule, error, limits) ||
        !decodeInterfaces(
            *importBytes, *exportBytes, decodedModule, error, limits) ||
        !decodeCode(*codeBytes, decodedModule, error, limits)) return false;
    if (!validateSlotDependencies(decodedManifest, decodedModule, dependencies,
            error, limits)) return false;
    decodedModule.dependencySlotPublications = dependencies;
    Module decodedProjection;
    if (!buildConcreteProjection(
            decodedManifest, decodedModule, decodedProjection, error))
        return false;
    if (containsGenericRecipe(decodedModule)) {
        error = "Moon Container cannot carry a generic or unresolved type recipe";
        return false;
    }

    const auto* entry = decodedManifest.entrypoint.empty()
        ? nullptr
        : findDeclarationRecord(decodedModule, decodedManifest.entrypoint);
    if ((decodedManifest.packageKind == ContainerPackageKind::Application &&
         (!entry || entry->kind != DeclarationKind::Function)) ||
        (decodedManifest.packageKind == ContainerPackageKind::Library && entry)) {
        error = "Moon Container entrypoint does not match its declaration table";
        return false;
    }
    if (entry) {
        const bool executable = std::any_of(
            decodedModule.declarations.begin(),
            decodedModule.declarations.end(), [&](const auto& declaration) {
                const auto* function = dynamic_cast<const FunctionDecl*>(
                    declaration.get());
                return function &&
                    function->symbolId == decodedManifest.entrypoint.symbol &&
                    function->contractId == decodedManifest.entrypoint.contract &&
                    !function->isExtern && function->controlFlow;
            });
        if (!executable) {
            error = "Moon Container application entrypoint is not executable";
            return false;
        }
    }
    Verifier verifier;
    if (!verifier.verify(decodedModule)) {
        error = verifier.errors().empty()
            ? "Moon Container MoonIR verification failed"
            : "Moon Container MoonIR verification failed: " +
                verifier.errors().front().message;
        return false;
    }

    std::vector<DeclarationRecord> publicSlots;
    for (const auto& exported : decodedModule.exports) {
        if (exported.kind != DeclarationKind::Slot) continue;
        if (const auto* record = decodedModule.findDeclaration(exported.declaration))
            publicSlots.push_back(*record);
    }
    decodedModule.localSlotPublication =
        std::shared_ptr<const SlotPublicationEvidence>(new SlotPublicationEvidence(
            decodedManifest.packageId, decodedManifest.targetTriple,
            decodedManifest.dataLayout, std::move(publicSlots)));
    manifest = std::move(decodedManifest);
    module = std::move(decodedModule);
    module.rebuildIndexes();
    error.clear();
    return true;
}

bool ContainerModelCodec::decodeContainerForTarget(
    const std::vector<uint8_t>& input,
    const std::string& expectedTargetTriple,
    const std::string& expectedDataLayout,
    ContainerManifest& manifest, Module& module,
    std::string& error, const ContainerLimits& limits,
    const SlotPublicationDependencies& dependencies) {
    if (expectedTargetTriple.empty() || expectedDataLayout.empty()) {
        error = "Moon Container executable target expectation is incomplete";
        return false;
    }
    ContainerManifest decodedManifest;
    Module decodedModule;
    if (!decodeContainer(
            input, decodedManifest, decodedModule, error, limits, dependencies)) return false;
    if (decodedManifest.targetTriple != expectedTargetTriple ||
        decodedManifest.dataLayout != expectedDataLayout) {
        error = "Moon Container target triple or data layout does not match the host";
        return false;
    }
    manifest = std::move(decodedManifest);
    module = std::move(decodedModule);
    module.rebuildIndexes();
    error.clear();
    return true;
}

} // namespace moon
