#include "ContainerModel.h"
#include "ContainerModelInternal.h"
#include "Verifier.h"

#include <algorithm>
#include <map>
#include <unordered_set>
#include <utility>

namespace moon {

using namespace container_detail;

std::shared_ptr<const SlotPublicationEvidence>
ContainerModelCodec::makeSlotPublicationEvidence(
    std::string owner, std::string target, std::string dataLayout,
    std::vector<DeclarationRecord> slots) {
    return std::shared_ptr<const SlotPublicationEvidence>(
        new SlotPublicationEvidence(std::move(owner), std::move(target),
            std::move(dataLayout), std::move(slots)));
}

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

bool ContainerModelCodec::encodeContainerWithPolicy(
    const ContainerManifest& manifest, const Module& module,
    std::vector<uint8_t>& output, std::string& error,
    const ContainerLimits& limits, uint32_t formatMinor,
    bool nativeRefUnitPreparation) {
    const auto policy = nativeRefUnitPreparation
        ? VerificationPolicy::NativeRefUnitPreparation
        : VerificationPolicy::Published;
    output.clear();
    if (formatMinor != FormatMinor && formatMinor != RefWireFormatMinor) {
        error = "Moon Container format version is unsupported";
        return false;
    }
    if (module.name != manifest.packageId ||
        featureBits(module.features) != featureBits(manifest.features)) {
        error = "Moon Container manifest disagrees with its MoonIR module";
        return false;
    }
    Verifier verifier(policy);
    if (!verifier.verify(module)) {
        error = verifier.errors().empty()
            ? "Moon Container MoonIR verification failed"
            : "Moon Container MoonIR verification failed: " +
                verifier.errors().front().message;
        return false;
    }
    if (nativeRefUnitPreparation) {
        for (const auto& type : module.typeTable) {
            if (type.kind != TypeKind::RuntimeFragmentRef) continue;
            const auto target = module.resolveRuntimeFragmentRefTarget(type.id);
            const auto* slot = target
                ? module.findDeclaration(*target) : nullptr;
            if (!slot) {
                error = "Native Ref/unit preparation preview has no exact Slot target";
                return false;
            }
            if (slot->id.rfind(module.name + "::", 0) == 0) continue;
            const bool evidenced = std::any_of(
                module.dependencySlotPublications.begin(),
                module.dependencySlotPublications.end(), [&](const auto& owner) {
                    return owner && owner->matches(*slot) &&
                        slot->id.rfind(owner->ownerPackageId() + "::", 0) == 0 &&
                        std::any_of(module.packageUses.begin(),
                            module.packageUses.end(), [&](const auto& use) {
                                return use.ownerPackageId == module.name &&
                                    use.packageId == owner->ownerPackageId();
                            });
                });
            if (!evidenced) {
                error = "Native Ref/unit preparation preview requires verified owner Slot evidence";
                return false;
            }
        }
    }

    Module projection;
    if (!buildConcreteProjection(
            manifest, module, projection, error)) return false;
    projection.formatMinor = formatMinor;
    // Published source composites still have compiler-authoritative SlotDecls.
    // The private Ref preparation preview requires independently decoded owner
    // evidence and never synthesizes it from the consumer's declarations.
    std::map<std::string, std::vector<DeclarationRecord>> sourceSlots;
    if (!nativeRefUnitPreparation) {
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
                ContainerModelCodec::makeSlotPublicationEvidence(
                    owner, manifest.targetTriple, manifest.dataLayout,
                    std::move(slots)));
        }
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
    if (!nativeRefUnitPreparation) {
        Verifier projectionVerifier(policy);
        if (!projectionVerifier.verify(projection)) {
            error = projectionVerifier.errors().empty()
                ? "Moon Container concrete projection verification failed"
                : "Moon Container concrete projection verification failed: " +
                    projectionVerifier.errors().front().message;
            return false;
        }
    }
    // The concrete projection keeps frozen records but not executable
    // FunctionDecl bodies. Its Native candidate check therefore cannot see
    // the Ref/unit source function. The source module was verified above and
    // the complete decoded executable module is verified below.

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
            return encodeTypes(projection, payload, error, limits,
                               formatMinor);
        }) ||
        !append(ContainerSectionId::Symbol, [&](auto& payload) {
            return encodeSymbols(projection, payload, error, limits);
        }) ||
        !append(ContainerSectionId::Contract, [&](auto& payload) {
            return encodeContracts(projection, payload, error, limits);
        }) ||
        !append(ContainerSectionId::Code, [&](auto& payload) {
            return encodeCodeRows(
                module, projection, payload, error, limits, formatMinor);
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
    if (!ContainerWriter::encode(
            std::move(sections), output, error, limits, formatMinor))
        return false;
    // Self-verify while the already-verified source projection's transient
    // dependency facts are still available. They never escape this encoding
    // operation; runtime consumers still need independent owner artifacts.
    ContainerManifest verifiedManifest;
    Module verifiedModule;
    if (!decodeContainerWithPolicy(output, verifiedManifest, verifiedModule,
            error, limits, projection.dependencySlotPublications,
            nativeRefUnitPreparation)) {
        error = "generated Moon Container failed self-verification: " + error;
        output.clear();
        return false;
    }
    if (verifiedManifest.targetTriple != manifest.targetTriple ||
        verifiedManifest.dataLayout != manifest.dataLayout) {
        error = "generated Moon Container target differs from its source manifest";
        output.clear();
        return false;
    }
    return true;
}

bool ContainerModelCodec::decodeContainerWithPolicy(
    const std::vector<uint8_t>& input, ContainerManifest& manifest,
    Module& module, std::string& error, const ContainerLimits& limits,
    const SlotPublicationDependencies& dependencies,
    bool nativeRefUnitPreparation) {
    const auto policy = nativeRefUnitPreparation
        ? VerificationPolicy::NativeRefUnitPreparation
        : VerificationPolicy::Published;
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
    if (!decodeTypes(*typeBytes, decodedModule, error, limits,
                     reader.formatMinor()) ||
        !decodeDeclarations(
            *symbolBytes, *contractBytes, *sysmetaBytes,
            decodedModule, error, limits) ||
        !decodeInterfaces(
            *importBytes, *exportBytes, decodedModule, error, limits) ||
        !decodeCode(*codeBytes, decodedModule, error, limits,
                    reader.formatMinor())) return false;
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
    Verifier verifier(policy);
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
        ContainerModelCodec::makeSlotPublicationEvidence(
            decodedManifest.packageId, decodedManifest.targetTriple,
            decodedManifest.dataLayout, std::move(publicSlots));
    manifest = std::move(decodedManifest);
    module = std::move(decodedModule);
    module.rebuildIndexes();
    error.clear();
    return true;
}

bool ContainerModelCodec::encodeContainer(
    const ContainerManifest& manifest, const Module& module,
    std::vector<uint8_t>& output, std::string& error,
    const ContainerLimits& limits, uint32_t formatMinor) {
    return encodeContainerWithPolicy(
        manifest, module, output, error, limits, formatMinor,
        false);
}

bool ContainerModelCodec::decodeContainer(
    const std::vector<uint8_t>& input, ContainerManifest& manifest,
    Module& module, std::string& error, const ContainerLimits& limits,
    const SlotPublicationDependencies& dependencies) {
    return decodeContainerWithPolicy(input, manifest, module, error,
        limits, dependencies, false);
}

bool ContainerModelCodec::verifyNativeRefUnitPreparationRoundTrip(
    const ContainerManifest& manifest, const Module& module,
    std::string& error, const ContainerLimits& limits) {
    return verifyNativeRefUnitPreparationRoundTripImpl(
        manifest, module, nullptr, error, limits);
}

#ifdef LUNA_PRIVATE_REF_JIT_TEST
bool ContainerModelCodec::encodeNativeRefUnitPreparationForTest(
    const ContainerManifest& manifest, const Module& source,
    std::vector<uint8_t>& encoded, std::string& error,
    const ContainerLimits& limits) {
    encoded.clear();
    return verifyNativeRefUnitPreparationRoundTripImpl(
        manifest, source, &encoded, error, limits);
}

bool ContainerModelCodec::decodeNativeRefUnitPreparationForTargetForTest(
    const std::vector<uint8_t>& input,
    const std::string& expectedTargetTriple,
    const std::string& expectedDataLayout,
    const SlotPublicationDependencies& ownerEvidence,
    ContainerManifest& manifest, Module& module, std::string& error,
    const ContainerLimits& limits) {
    if (expectedTargetTriple.empty() || expectedDataLayout.empty()) {
        error = "Native Ref/unit preparation target expectation is incomplete";
        return false;
    }
    ContainerManifest decodedManifest;
    Module decodedModule;
    if (!decodeContainerWithPolicy(input, decodedManifest, decodedModule,
            error, limits, ownerEvidence, true))
        return false;
    if (decodedModule.formatMinor != RefWireFormatMinor ||
        decodedManifest.targetTriple != expectedTargetTriple ||
        decodedManifest.dataLayout != expectedDataLayout) {
        error = "Native Ref/unit preparation container target or format does not match the host";
        return false;
    }
    std::vector<uint8_t> canonical;
    if (!verifyNativeRefUnitPreparationRoundTripImpl(
            decodedManifest, decodedModule, &canonical, error, limits))
        return false;
    if (canonical != input) {
        error = "Native Ref/unit preparation container is not byte-canonical";
        return false;
    }
    manifest = std::move(decodedManifest);
    module = std::move(decodedModule);
    module.rebuildIndexes();
    error.clear();
    return true;
}
#endif

bool ContainerModelCodec::verifyNativeRefUnitPreparationRoundTripImpl(
    const ContainerManifest& manifest, const Module& module,
    std::vector<uint8_t>* encoded, std::string& error,
    const ContainerLimits& limits) {
    const TypeRecord* refType = nullptr;
    for (const auto& type : module.typeTable) {
        if (type.kind != TypeKind::RuntimeFragmentRef) continue;
        if (refType) {
            error = "Native Ref/unit preparation requires one Ref type";
            return false;
        }
        refType = &type;
    }
    const FunctionDecl* applySource = nullptr;
    for (const auto& declaration : module.declarations) {
        const auto* function = dynamic_cast<const FunctionDecl*>(
            declaration.get());
        if (!function || !function->controlFlow ||
            function->controlFlow->runtimeRefApplies.empty()) continue;
        if (applySource) {
            error = "Native Ref/unit preparation requires one Apply function";
            return false;
        }
        applySource = function;
    }
    if (!refType || !applySource) {
        error = "Native Ref/unit preparation preview requires one Ref type and Apply function";
        return false;
    }
    const auto usesRef = [&](const FunctionDecl& function) {
        return function.returnType == refType->id ||
            std::any_of(function.params.begin(), function.params.end(),
                [&](const auto& parameter) {
                    return parameter.type == refType->id;
                }) ||
            (function.controlFlow &&
             std::any_of(function.controlFlow->locals.begin(),
                 function.controlFlow->locals.end(), [&](const auto& local) {
                     return local.type == refType->id;
                 }));
    };
    for (const auto& declaration : module.declarations) {
        if (const auto* function = dynamic_cast<const FunctionDecl*>(
                declaration.get())) {
            if (function != applySource && usesRef(*function)) {
                error = "Native Ref/unit preparation has another Ref callable";
                return false;
            }
        } else if (const auto* implementation = dynamic_cast<const ImplDecl*>(
                       declaration.get())) {
            for (const auto& method : implementation->methods)
                if (method && usesRef(*method)) {
                    error = "Native Ref/unit preparation has another Ref callable";
                    return false;
                }
        }
    }
    for (const auto& record : module.declarationTable) {
        if (record.kind != DeclarationKind::Function) continue;
        const auto* signature = module.findType(record.type);
        if (!signature || signature->kind != TypeKind::Function) continue;
        const bool referencesRef = signature->returnTypeId == refType->id ||
            std::find(signature->parameterTypeIds.begin(),
                signature->parameterTypeIds.end(), refType->id) !=
                signature->parameterTypeIds.end();
        if (referencesRef &&
            (record.symbolId != applySource->symbolId ||
             record.contractId != applySource->contractId)) {
            error = "Native Ref/unit preparation has another Ref callable";
            return false;
        }
    }
    std::vector<uint8_t> first;
    if (!encodeContainerWithPolicy(manifest, module, first, error, limits,
            RefWireFormatMinor,
            true))
        return false;
    ContainerManifest decodedManifest;
    Module decodedModule;
    if (!decodeContainerWithPolicy(first, decodedManifest, decodedModule,
            error, limits, module.dependencySlotPublications,
            true))
        return false;
    std::vector<uint8_t> repeated;
    if (!encodeContainerWithPolicy(decodedManifest, decodedModule, repeated,
            error, limits, RefWireFormatMinor,
            true))
        return false;
    if (repeated != first) {
        error = "Native Ref/unit preparation container is not byte-canonical";
        return false;
    }
    if (encoded) *encoded = std::move(first);
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
