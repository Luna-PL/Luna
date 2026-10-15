#include "driver/MoonGeneration.h"

#include "codegen/CodeGenerator.h"
#include "codegen/NativeRefUnitFacts.h"
#include "diagnostics/Diagnostic.h"
#include "moonir/ContainerModel.h"
#include "runtime/NativeArtifactABI.h"
#include "runtime/RuntimeDescriptor.h"
#include "runtime/RuntimeFragment.h"
#include "runtime/RuntimeFragmentCompilerBridge.h"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/Support/SHA256.h>

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <utility>

namespace luna::driver {

struct VerifiedMoonSlotOwnerState {
    luna::runtime::MoonRuntime runtime;
    luna::runtime::MoonRuntime::PinnedGeneration generation;
    moon::ContainerManifest manifest;
    moon::Module module;
    std::string contentDigest;
};

struct VerifiedMoonRefUnitPreparationState {
    moon::ContainerManifest manifest;
    moon::Module module;
    std::string contentDigest;
    luna::runtime::MoonRuntime::PinnedGeneration ownerGeneration;
    luna::codegen::NativeRefUnitSourceFacts facts;
    std::shared_ptr<LunaJitModule> codeLease;
    const void* codeEntry = nullptr;
    std::string codeLinkageName;
};

#ifdef LUNA_PRIVATE_REF_JIT_TEST
struct PrivatePreparedRefGenerationForTest {
    moon::ContainerManifest manifest;
    moon::Module module;
    std::string contentDigest;
    VerifiedMoonRefUnitPreparation verifiedPreparation;
    std::shared_ptr<LunaPrivateRefUnitApplyLoadedEntry> entry;
};
#endif

namespace {

bool sameRefUnitCandidateFacts(
    const luna::runtime::GenerationRefUnitCandidate& actual,
    const luna::runtime::GenerationRefUnitCandidate& expected) {
    return actual.symbolId == expected.symbolId &&
        actual.contractId == expected.contractId &&
        actual.linkageName == expected.linkageName &&
        actual.moduleName == expected.moduleName &&
        actual.packageId == expected.packageId &&
        actual.functionTypeId == expected.functionTypeId &&
        actual.refTypeId == expected.refTypeId &&
        actual.slotSymbolId == expected.slotSymbolId &&
        actual.slotContractId == expected.slotContractId &&
        actual.slotTypeId == expected.slotTypeId &&
        actual.entryAbi == expected.entryAbi &&
        actual.refMode == expected.refMode &&
        actual.resultMode == expected.resultMode &&
        actual.statusDomain == expected.statusDomain;
}

struct LoadedMoonGeneration {
    moon::ContainerManifest manifest;
    moon::Module module;
    std::string contentDigest;
    std::string materializationKey;
    std::shared_ptr<LunaJitModule> jitLease;
    luna::runtime::RuntimeDescriptorRegistryView runtimeDescriptors;
};

bool jitConfiguration(MoonJitOptimization optimization,
                      LunaOptimizationLevel& level, std::string& key,
                      std::string& error) {
    const char* name = nullptr;
    switch (optimization) {
    case MoonJitOptimization::O0: level = LunaOptimizationLevel::O0; name = "O0"; break;
    case MoonJitOptimization::O2: level = LunaOptimizationLevel::O2; name = "O2"; break;
    case MoonJitOptimization::O3: level = LunaOptimizationLevel::O3; name = "O3"; break;
    default: error = "invalid verified Moon JIT optimization profile"; return false;
    }
    key = std::string("luna.verified-moon-jit.v1:llvm=") + LLVM_VERSION_STRING +
        ":ir=" + name + ":orc=default";
    return true;
}

std::string containerDigest(const std::vector<uint8_t>& bytes) {
    llvm::SHA256 hash;
    hash.update(llvm::ArrayRef<uint8_t>(bytes));
    const auto digest = hash.final();
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (uint8_t byte : digest)
        result << std::setw(2) << static_cast<unsigned>(byte);
    return result.str();
}

bool hasRetainedMetadata(const moon::DeclarationRecord& record) {
    return std::any_of(
        record.metadata.begin(), record.metadata.end(),
        [](const moon::MetadataInstance& metadata) {
            return metadata.retention != moon::Retention::CompileTime;
        });
}

bool isExportedRuntimeControl(
    const moon::Module& module, const moon::DeclarationRecord& record) {
    if (record.kind != moon::DeclarationKind::Slot &&
        record.kind != moon::DeclarationKind::Fragment)
        return false;
    const moon::DeclarationRef reference{record.symbolId, record.contractId};
    return std::any_of(
        module.exports.begin(), module.exports.end(),
        [&](const moon::ExportRecord& exported) {
            return exported.declaration == reference;
        });
}

bool hasRuntimeDescriptor(
    const moon::Module& module, const moon::DeclarationRecord& record) {
    return record.retention != moon::Retention::CompileTime ||
        hasRetainedMetadata(record) ||
        isExportedRuntimeControl(module, record);
}

bool matchesRuntimeMetadataValue(
    const moon::ConstantValue& expected,
    const LunaRuntimeMetadataValueV1& actual) {
    if (const auto* integer = std::get_if<int64_t>(&expected)) {
        return actual.kind == LUNA_RUNTIME_METADATA_INTEGER_V1 &&
            actual.payload == static_cast<uint64_t>(*integer) &&
            actual.string_value == nullptr;
    }
    if (const auto* floating = std::get_if<double>(&expected)) {
        uint64_t bits = 0;
        static_assert(sizeof(bits) == sizeof(*floating));
        std::memcpy(&bits, floating, sizeof(bits));
        return actual.kind == LUNA_RUNTIME_METADATA_FLOAT_V1 &&
            actual.payload == bits && actual.string_value == nullptr;
    }
    if (const auto* boolean = std::get_if<bool>(&expected)) {
        return actual.kind == LUNA_RUNTIME_METADATA_BOOLEAN_V1 &&
            actual.payload == (*boolean ? 1u : 0u) &&
            actual.string_value == nullptr;
    }
    const auto& string = std::get<std::string>(expected);
    return actual.kind == LUNA_RUNTIME_METADATA_STRING_V1 &&
        actual.string_value && string == actual.string_value;
}

bool validateRuntimeDescriptors(
    LoadedMoonGeneration& loaded, std::string& error) {
    std::vector<const moon::DeclarationRecord*> expected;
    for (const auto& record : loaded.module.declarationTable) {
        if (hasRuntimeDescriptor(loaded.module, record))
            expected.push_back(&record);
    }
    if (expected.empty()) return true;
    std::sort(
        expected.begin(), expected.end(),
        [](const moon::DeclarationRecord* left,
           const moon::DeclarationRecord* right) {
            return left->symbolId.value < right->symbolId.value;
        });

    const std::string registrySymbol =
        luna::runtime::runtimeDescriptorRegistrySymbol(loaded.module.name);
    const void* address = loaded.jitLease->lookup(registrySymbol, error);
    if (!address) {
        error = "verified Moon module lost Runtime descriptor registry '" +
            registrySymbol + "': " + error;
        return false;
    }
    const auto* registry =
        static_cast<const LunaRuntimeDescriptorRegistryV1*>(address);
    if (!loaded.runtimeDescriptors.bind(registry, error)) {
        error = "verified Moon Runtime descriptor registry is invalid: " + error;
        return false;
    }
    if (loaded.runtimeDescriptors.moduleId() != loaded.module.name ||
        loaded.runtimeDescriptors.size() != expected.size()) {
        error = "verified Moon Runtime descriptor registry identity/count mismatch";
        return false;
    }
    for (size_t index = 0; index < expected.size(); ++index) {
        const auto& record = *expected[index];
        const auto* descriptor = loaded.runtimeDescriptors.at(index);
        if (!descriptor || record.symbolId.value != descriptor->symbol_id ||
            record.contractId.value != descriptor->contract_id ||
            record.type.value != descriptor->type_id ||
            record.linkageName != descriptor->linkage_name ||
            static_cast<uint32_t>(record.kind) + 1 !=
                descriptor->declaration_kind ||
            static_cast<uint32_t>(record.retention) != descriptor->retention) {
            error = "verified Moon Runtime descriptor disagrees with declaration '" +
                record.symbolId.value + "'";
            return false;
        }

        std::vector<const moon::MetadataInstance*> retainedMetadata;
        for (const auto& metadata : record.metadata) {
            if (metadata.retention != moon::Retention::CompileTime)
                retainedMetadata.push_back(&metadata);
        }
        if (descriptor->metadata_count != retainedMetadata.size()) {
            error = "verified Moon Runtime descriptor metadata count mismatch";
            return false;
        }
        for (size_t metadataIndex = 0;
             metadataIndex < retainedMetadata.size(); ++metadataIndex) {
            const auto& expectedMetadata = *retainedMetadata[metadataIndex];
            const auto& actualMetadata = descriptor->metadata[metadataIndex];
            if (expectedMetadata.schemaId != actualMetadata.schema_id ||
                static_cast<uint32_t>(expectedMetadata.retention) !=
                    actualMetadata.retention ||
                expectedMetadata.values.size() != actualMetadata.value_count) {
                error = "verified Moon Runtime descriptor metadata identity mismatch";
                return false;
            }
            for (size_t valueIndex = 0;
                 valueIndex < expectedMetadata.values.size(); ++valueIndex) {
                if (!matchesRuntimeMetadataValue(
                        expectedMetadata.values[valueIndex],
                        actualMetadata.values[valueIndex])) {
                    error = "verified Moon Runtime descriptor metadata value mismatch";
                    return false;
                }
            }
        }

        const auto function = loaded.module.functionsBySymbol.find(
            record.linkageName);
        const bool callable = record.kind == moon::DeclarationKind::Function &&
            function != loaded.module.functionsBySymbol.end() && function->second;
        const bool fragmentExecutable =
            record.kind == moon::DeclarationKind::Fragment &&
            record.runtimeEntry.complete();
        uint32_t expectedFlags = callable
            ? LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1 : 0;
        if (callable && function->second->requiresFragmentContext)
            expectedFlags |= LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1;
        if (fragmentExecutable)
            expectedFlags |= LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_EXECUTABLE_V1;
        if (isExportedRuntimeControl(loaded.module, record))
            expectedFlags |= LUNA_RUNTIME_DESCRIPTOR_PUBLIC_CONTROL_V1;
        if (descriptor->flags != expectedFlags) {
            error = "verified Moon Runtime descriptor callable flags mismatch";
            return false;
        }
        if (!callable && !fragmentExecutable && descriptor->entry) {
            error = "verified Moon non-function descriptor has a callable entry";
            return false;
        }
        if (fragmentExecutable) {
            const auto* fragment =
                static_cast<const LunaRuntimeFragmentDescriptorV1*>(
                    descriptor->entry);
            const auto* slot = loaded.module.findDeclaration(
                record.controlTarget);
            const auto* environment = loaded.module.findType(
                record.environmentType);
            const auto* arguments = loaded.module.findType(
                record.controlArgumentsType);
            std::string fragmentError;
            if (!fragment || !slot || !environment || !arguments ||
                !luna::runtime::validateRuntimeFragmentDescriptor(
                    *fragment, fragmentError) ||
                record.symbolId.value != fragment->fragment_id ||
                record.contractId.value != fragment->fragment_contract_id ||
                slot->symbolId.value != fragment->slot_id ||
                slot->contractId.value != fragment->slot_contract_id ||
                arguments->abiLayoutId.value !=
                    fragment->slot_arguments_layout_id ||
                arguments->valueSize != fragment->slot_arguments_size ||
                arguments->valueAlignment !=
                    fragment->slot_arguments_alignment ||
                environment->abiLayoutId.value !=
                    fragment->environment_layout_id) {
                error = "verified Moon runtime Fragment descriptor mismatch";
                if (!fragmentError.empty()) error += ": " + fragmentError;
                return false;
            }
        }
    }
    return true;
}

} // namespace

bool stageVerifiedMoonGeneration(
    luna::runtime::MoonRuntime& runtime,
    const std::vector<uint8_t>& containerBytes,
    const std::string& expectedTargetTriple,
    const std::string& expectedDataLayout,
    const luna::runtime::GenerationInitializer& initializer,
    luna::runtime::MoonRuntime::StagedGeneration& staged,
    std::string& error, const moon::SlotPublicationDependencies& dependencies,
    MoonJitOptimization optimization) {
    error.clear();
    LunaOptimizationLevel level;
    std::string materializationKey;
    if (!jitConfiguration(optimization, level, materializationKey, error)) return false;
    auto loaded = std::make_shared<LoadedMoonGeneration>();
    if (!moon::ContainerModelCodec::decodeContainerForTarget(
            containerBytes, expectedTargetTriple, expectedDataLayout,
            loaded->manifest, loaded->module, error, {}, dependencies))
        return false;
    loaded->contentDigest = containerDigest(containerBytes);
    loaded->materializationKey = materializationKey;

    CodeGenerator codeGenerator(
        loaded->manifest.packageId + "@" + loaded->contentDigest);
    codeGenerator.setOptimizationLevel(level);
    if (!codeGenerator.generate(&loaded->module)) {
        std::ostringstream details;
        details << "verified Moon generation failed LLVM lowering";
        for (const auto& diagnostic : codeGenerator.errors())
            details << "\n" << diagnostic::render(diagnostic);
        error = details.str();
        return false;
    }
    loaded->jitLease = codeGenerator.materializeJitModule(error);
    if (!loaded->jitLease) {
        error = "verified Moon generation failed JIT materialization: " + error;
        return false;
    }
    if (!validateRuntimeDescriptors(*loaded, error)) return false;

    luna::runtime::GenerationStagingRequest request;
    request.moduleId = loaded->manifest.packageId;
    request.contentDigest = loaded->contentDigest;
    request.materializationKey = loaded->materializationKey;
    request.moduleLease = loaded;
    return runtime.stage(
        request,
        [loaded](const auto& candidate, std::string& verificationError) {
            if (candidate.moduleId != loaded->manifest.packageId ||
                candidate.contentDigest != loaded->contentDigest ||
                candidate.materializationKey != loaded->materializationKey) {
                verificationError =
                    "verified Moon generation identity changed before staging";
                return false;
            }
            return true;
        },
        [loaded](const auto&, auto& bindings, std::string& resolutionError) {
            std::vector<moon::DeclarationRef> published;
            published.reserve(loaded->module.exports.size() + 1);
            for (const auto& exported : loaded->module.exports)
                published.push_back(exported.declaration);
            if (loaded->manifest.entrypoint.complete() &&
                std::find(published.begin(), published.end(),
                          loaded->manifest.entrypoint) == published.end())
                published.push_back(loaded->manifest.entrypoint);
            for (const auto& declaration : loaded->module.declarationTable) {
                if (!hasRuntimeDescriptor(loaded->module, declaration)) continue;
                const moon::DeclarationRef reference{
                    declaration.symbolId, declaration.contractId};
                if (std::find(published.begin(), published.end(), reference) ==
                    published.end())
                    published.push_back(reference);
            }

            bindings.reserve(published.size());
            for (const auto& reference : published) {
                const auto* declaration =
                    loaded->module.findDeclaration(reference);
                if (!declaration) {
                    resolutionError =
                        "verified Moon publication lost its declaration descriptor";
                    return false;
                }
                luna::runtime::GenerationBinding binding;
                binding.symbolId = declaration->symbolId.value;
                binding.contractId = declaration->contractId.value;
                binding.implementation = declaration;
                binding.declarationKind =
                    static_cast<uint32_t>(declaration->kind) + 1;
                binding.flags = 0;
                if (isExportedRuntimeControl(
                        loaded->module, *declaration))
                    binding.flags |=
                        luna::runtime::GenerationBindingPublicControl;
                if (declaration->kind == moon::DeclarationKind::Function) {
                    const auto* runtimeDescriptor =
                        loaded->runtimeDescriptors.find(
                            declaration->symbolId.value,
                            declaration->contractId.value,
                            static_cast<uint32_t>(declaration->kind) + 1,
                            LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1);
                    if (runtimeDescriptor) {
                        binding.implementation = runtimeDescriptor->entry;
                        binding.flags |=
                            luna::runtime::GenerationBindingCallable;
                        if ((runtimeDescriptor->flags &
                             LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1) != 0)
                            binding.flags |=
                                luna::runtime::GenerationBindingFragmentContext;
                        bindings.push_back(std::move(binding));
                        continue;
                    }
                    const auto function = loaded->module.functionsBySymbol.find(
                        declaration->linkageName);
                    if (function == loaded->module.functionsBySymbol.end() ||
                        !function->second) {
                        resolutionError =
                            "verified Moon function lost its executable body";
                        return false;
                    }
                    const std::string symbol = function->second->linkName.empty()
                        ? function->second->generatedSymbolName
                        : function->second->linkName;
                    binding.implementation =
                        loaded->jitLease->lookup(symbol, resolutionError);
                    if (!binding.implementation) {
                        resolutionError = "verified Moon function '" + symbol +
                            "' failed JIT lookup: " + resolutionError;
                        return false;
                    }
                    binding.flags |=
                        luna::runtime::GenerationBindingCallable;
                    if (function->second->requiresFragmentContext)
                        binding.flags |=
                            luna::runtime::GenerationBindingFragmentContext;
                } else if (declaration->kind ==
                               moon::DeclarationKind::Fragment &&
                           declaration->runtimeEntry.complete()) {
                    const auto* runtimeDescriptor =
                        loaded->runtimeDescriptors.find(
                            declaration->symbolId.value,
                            declaration->contractId.value,
                            static_cast<uint32_t>(declaration->kind) + 1,
                            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_EXECUTABLE_V1);
                    if (!runtimeDescriptor || !runtimeDescriptor->entry) {
                        resolutionError =
                            "verified Moon Fragment lost its runtime descriptor";
                        return false;
                    }
                    binding.implementation = runtimeDescriptor->entry;
                    binding.flags |=
                        luna::runtime::GenerationBindingFragmentExecutable;
                }
                bindings.push_back(std::move(binding));
            }
            return true;
        },
        initializer, staged, error);
}

uint64_t VerifiedMoonSlotOwner::generationId() const {
    return state_ ? state_->generation.generationId() : 0;
}

bool VerifiedMoonSlotOwner::pinImportedSlot(
    const moon::DeclarationRecord& importedSlot,
    const std::string& expectedTargetTriple,
    const std::string& expectedDataLayout,
    luna::runtime::MoonRuntime::PinnedGeneration& pinned,
    std::string& error) const {
    error.clear();
    if (pinned) {
        error = "verified Slot owner output already owns a generation";
        return false;
    }
    if (!state_ || !state_->generation ||
        state_->manifest.targetTriple != expectedTargetTriple ||
        state_->manifest.dataLayout != expectedDataLayout ||
        importedSlot.kind != moon::DeclarationKind::Slot ||
        !state_->module.localSlotPublication ||
        !state_->module.localSlotPublication->matches(importedSlot)) {
        error = "verified Slot owner does not match the imported Slot or target";
        return false;
    }
    const moon::DeclarationRef slotReference{
        importedSlot.symbolId, importedSlot.contractId};
    const auto* ownerRecord = state_->module.findDeclaration(slotReference);
    const auto ownerBinding = state_->generation.find(
        importedSlot.symbolId.value, importedSlot.contractId.value);
    if (!ownerRecord ||
        !state_->module.localSlotPublication->matches(*ownerRecord) ||
        !ownerBinding ||
        ownerBinding.declarationKind() != LUNA_RUNTIME_DECLARATION_SLOT_V1 ||
        ownerBinding.flags() !=
            luna::runtime::GenerationBindingPublicControl ||
        !ownerBinding.implementation()) {
        error = "verified Slot owner lost its exported runtime binding";
        return false;
    }
    pinned = state_->generation;
    return true;
}

bool loadVerifiedMoonSlotOwner(
    const std::vector<uint8_t>& ownerContainerBytes,
    const std::string& expectedTargetTriple,
    const std::string& expectedDataLayout,
    VerifiedMoonSlotOwner& owner,
    std::string& error) {
    error.clear();
    if (owner) {
        error = "verified Slot owner output already owns a generation";
        return false;
    }
    auto verified = std::make_shared<VerifiedMoonSlotOwnerState>();
    if (!moon::ContainerModelCodec::decodeContainerForTarget(
            ownerContainerBytes, expectedTargetTriple, expectedDataLayout,
            verified->manifest, verified->module, error))
        return false;
    if (!verified->module.localSlotPublication ||
        !std::any_of(verified->module.exports.begin(),
                     verified->module.exports.end(),
                     [](const moon::ExportRecord& exported) {
                         return exported.kind == moon::DeclarationKind::Slot;
                     })) {
        error = "verified Slot owner artifact has no Slot publication";
        return false;
    }
    verified->contentDigest = containerDigest(ownerContainerBytes);
    luna::runtime::MoonRuntime::StagedGeneration staged;
    if (!stageVerifiedMoonGeneration(
            verified->runtime, ownerContainerBytes, expectedTargetTriple,
            expectedDataLayout, {}, staged, error))
        return false;
    if (!staged || staged.moduleId() != verified->manifest.packageId ||
        staged.contentDigest() != verified->contentDigest ||
        !verified->runtime.loadOnce(staged, verified->generation, error)) {
        if (error.empty())
            error = "verified Slot owner staging changed its artifact";
        return false;
    }
    owner.state_ = std::move(verified);
    return true;
}

uint64_t VerifiedMoonRefUnitPreparation::ownerGenerationId() const {
    return state_ ? state_->ownerGeneration.generationId() : 0;
}

std::optional<luna::codegen::NativeRefUnitSourceFacts>
VerifiedMoonRefUnitPreparation::facts() const {
    return state_ ? std::optional{state_->facts} : std::nullopt;
}

bool VerifiedMoonRefUnitPreparation::matchesConsumer(
    const luna::runtime::MoonRuntime::PinnedGeneration& consumer) const {
    return state_ && state_->codeLease && state_->codeEntry && consumer &&
           consumer.moduleId() == state_->manifest.packageId &&
           consumer.contentDigest() == state_->contentDigest;
}

bool loadVerifiedMoonRefUnitPreparation(
    const std::vector<uint8_t>& consumerContainerBytes,
    const VerifiedMoonSlotOwner& owner,
    const std::string& expectedTargetTriple,
    const std::string& expectedDataLayout,
    VerifiedMoonRefUnitPreparation& preparation,
    std::string& error) {
    error.clear();
    if (preparation) {
        error = "verified Ref/unit preparation output already owns an artifact";
        return false;
    }
    if (!owner || !owner.state_->generation ||
        owner.state_->manifest.targetTriple != expectedTargetTriple ||
        owner.state_->manifest.dataLayout != expectedDataLayout) {
        error = "verified Ref/unit preparation requires an owner for the target";
        return false;
    }
    auto verified = std::make_shared<VerifiedMoonRefUnitPreparationState>();
    if (!moon::ContainerModelCodec::decodeNativeRefUnitPreparationForTarget(
            consumerContainerBytes, expectedTargetTriple, expectedDataLayout,
            {owner.state_->module.localSlotPublication},
            verified->manifest, verified->module, error))
        return false;
    if (verified->manifest.packageId == owner.state_->manifest.packageId) {
        error = "verified Ref/unit consumer must have a separate owner package";
        return false;
    }
    moon::FunctionDecl* source = nullptr;
    for (auto& declaration : verified->module.declarations) {
        auto* function = dynamic_cast<moon::FunctionDecl*>(declaration.get());
        if (!function || !function->controlFlow ||
            function->controlFlow->runtimeRefApplies.empty()) continue;
        source = function;
        break;
    }
    if (!source) {
        error = "verified Ref/unit consumer lost its Apply source";
        return false;
    }
    if (!luna::codegen::deriveNativeRefUnitSourceFacts(
            verified->module, *source, verified->facts, error))
        return false;
    const moon::DeclarationRef slotReference{
        luna::identity::SymbolId{verified->facts.slotSymbolId},
        luna::identity::ContractId{verified->facts.slotContractId}};
    const auto* importedSlot = verified->module.findDeclaration(slotReference);
    if (!importedSlot ||
        importedSlot->type.value != verified->facts.slotTypeId ||
        !owner.pinImportedSlot(
            *importedSlot, expectedTargetTriple, expectedDataLayout,
            verified->ownerGeneration, error)) {
        error = "verified Ref/unit owner lost its exact imported Slot";
        return false;
    }
    verified->codeLease =
        CodeGenerator::materializeNativeRefUnitPreparationLowering(
            verified->module, *source, verified->codeEntry,
            verified->codeLinkageName, error);
    if (!verified->codeLease || !verified->codeEntry ||
        verified->codeLinkageName.empty())
        return false;
    verified->contentDigest = containerDigest(consumerContainerBytes);
    preparation.state_ = std::move(verified);
    return true;
}

uint64_t VerifiedMoonRefUnitGeneration::generationId() const {
    return consumer_.generationId();
}

uint64_t VerifiedMoonRefUnitGeneration::ownerGenerationId() const {
    return preparation_.ownerGenerationId();
}

std::optional<luna::codegen::NativeRefUnitSourceFacts>
VerifiedMoonRefUnitGeneration::facts() const {
    return preparation_.facts();
}

bool VerifiedMoonRefUnitGeneration::matchesPreparation(
    const VerifiedMoonRefUnitPreparation& preparation) const {
    return *this && preparation.state_ &&
           consumer_.matchesModuleLease(preparation.state_) &&
           preparation.matchesConsumer(consumer_);
}

bool loadVerifiedMoonRefUnitGenerationOnce(
    const VerifiedMoonRefUnitPreparation& preparation,
    VerifiedMoonRefUnitGeneration& loaded,
    std::string& error) {
    error.clear();
    if (loaded) {
        error = "verified Ref/unit generation output already owns a generation";
        return false;
    }
    if (!preparation.state_ || !preparation.state_->ownerGeneration ||
        !preparation.state_->codeLease || !preparation.state_->codeEntry ||
        preparation.state_->codeLinkageName.empty()) {
        error = "verified Ref/unit generation requires a complete preparation";
        return false;
    }
    const auto lease = std::shared_ptr<const void>(preparation.state_);
    luna::runtime::GenerationStagingRequest request;
    request.moduleId = preparation.state_->manifest.packageId;
    request.contentDigest = preparation.state_->contentDigest;
    request.materializationKey =
        std::string("luna.internal-ref-unit-preparation.v1:llvm=") +
        LLVM_VERSION_STRING + ":ir=O0:orc=default";
    request.moduleLease = lease;

    luna::runtime::MoonRuntime runtime;
    luna::runtime::MoonRuntime::StagedGeneration staged;
    const auto& state = *preparation.state_;
    if (!runtime.stage(
            request,
            [&state, &lease](const auto& candidate,
                             std::string& verificationError) {
                if (candidate.moduleId != state.manifest.packageId ||
                    candidate.contentDigest != state.contentDigest ||
                    candidate.moduleLease.get() != lease.get() ||
                    candidate.moduleLease.owner_before(lease) ||
                    lease.owner_before(candidate.moduleLease) ||
                    !candidate.refUnitCandidates.empty() ||
                    candidate.refUnitCaller) {
                    verificationError =
                        "verified Ref/unit generation changed before staging";
                    return false;
                }
                return true;
            },
            [](const auto&, auto& bindings, std::string& resolutionError) {
                if (!bindings.empty()) {
                    resolutionError =
                        "verified Ref/unit generation cannot publish bindings";
                    return false;
                }
                return true;
            },
            {}, staged, error))
        return false;
    if (!staged || staged.moduleId() != state.manifest.packageId ||
        staged.contentDigest() != state.contentDigest ||
        staged.materializationKey() != request.materializationKey) {
        error = "verified Ref/unit staging changed its artifact";
        return false;
    }
    luna::runtime::MoonRuntime::PinnedGeneration consumer;
    if (!runtime.loadOnce(staged, consumer, error)) return false;
    if (!consumer.matchesModuleLease(lease) ||
        !preparation.matchesConsumer(consumer) ||
        consumer.materializationKey() != request.materializationKey ||
        consumer.find(state.facts.functionSymbolId,
                      state.facts.functionContractId) ||
        consumer.findRefUnitCandidate(state.facts.functionSymbolId,
                                      state.facts.functionContractId)) {
        error = "verified Ref/unit generation lost its inert preparation lease";
        return false;
    }
    loaded.preparation_ = preparation;
    loaded.consumer_ = std::move(consumer);
    return true;
}

uint64_t VerifiedMoonImportedSlotRefUnitProof::consumerGenerationId() const {
    return consumer_.generationId();
}

uint64_t VerifiedMoonImportedSlotRefUnitProof::ownerGenerationId() const {
    return consumer_.ownerGenerationId();
}

std::optional<luna::codegen::NativeRefUnitSourceFacts>
VerifiedMoonImportedSlotRefUnitProof::facts() const {
    return consumer_.facts();
}

std::optional<luna::runtime::GenerationRefUnitCandidate>
VerifiedMoonImportedSlotRefUnitProof::candidateFacts() const {
    if (!*this) return std::nullopt;
    const auto source = consumer_.facts();
    if (!source) return std::nullopt;
    luna::runtime::GenerationRefUnitCandidate candidate;
    candidate.symbolId = source->functionSymbolId;
    candidate.contractId = source->functionContractId;
    candidate.linkageName =
        consumer_.preparation_.state_->codeLinkageName;
    candidate.moduleName = source->moduleName;
    candidate.packageId = source->packageId;
    candidate.functionTypeId = source->functionTypeId;
    candidate.refTypeId = source->refTypeId;
    candidate.slotSymbolId = source->slotSymbolId;
    candidate.slotContractId = source->slotContractId;
    candidate.slotTypeId = source->slotTypeId;
    candidate.entryAbi = luna::runtime::GenerationEntryAbiRefUnitV1;
    candidate.refMode = LUNA_NATIVE_REF_SHARED_BORROW_V1;
    candidate.resultMode = LUNA_NATIVE_RESULT_UNIT_V1;
    candidate.statusDomain = LUNA_NATIVE_STATUS_DOMAIN_REF_UNIT_V1;
    return candidate;
}

bool VerifiedMoonImportedSlotRefUnitProof::matchesPreparation(
    const VerifiedMoonRefUnitPreparation& preparation) const {
    return consumer_.matchesPreparation(preparation);
}

bool pinVerifiedMoonImportedSlotRefUnitProof(
    const VerifiedMoonRefUnitGeneration& consumer,
    const std::string& sourceSymbolId,
    const std::string& sourceContractId,
    const luna::runtime::RuntimeSlotRequirement& slot,
    VerifiedMoonImportedSlotRefUnitProof& proof,
    std::string& error) {
    error.clear();
    if (proof) {
        error = "verified imported-Slot Ref/unit proof output already owns a consumer";
        return false;
    }
    const auto facts = consumer.facts();
    if (!consumer ||
        !consumer.matchesPreparation(consumer.preparation_) ||
        !facts ||
        facts->functionSymbolId != sourceSymbolId ||
        facts->functionContractId != sourceContractId ||
        facts->slotSymbolId != slot.slotId ||
        facts->slotContractId != slot.contractId) {
        error = "verified imported-Slot Ref/unit source or consumer does not match";
        return false;
    }
    const auto& state = *consumer.preparation_.state_;
    const moon::DeclarationRef slotReference{
        luna::identity::SymbolId{slot.slotId},
        luna::identity::ContractId{slot.contractId}};
    const auto* imported = state.module.findDeclaration(slotReference);
    const auto ownerBinding = state.ownerGeneration.find(
        slot.slotId, slot.contractId);
    if (!imported || imported->kind != moon::DeclarationKind::Slot ||
        imported->type.value != facts->slotTypeId ||
        !ownerBinding ||
        ownerBinding.declarationKind() != LUNA_RUNTIME_DECLARATION_SLOT_V1 ||
        ownerBinding.flags() !=
            luna::runtime::GenerationBindingPublicControl ||
        !ownerBinding.implementation()) {
        error = "verified imported-Slot Ref/unit owner binding does not match";
        return false;
    }
    proof.consumer_ = consumer;
    return true;
}

uint64_t VerifiedMoonImportedSlotGeneration::generationId() const {
    return generation_.generationId();
}

bool VerifiedMoonImportedSlotGeneration::hasImportedSlotBinding() const {
    if (!*this) return false;
    const auto candidate = proof_->candidateFacts();
    if (!candidate) return false;
    const auto binding = generation_.find(
        candidate->slotSymbolId, candidate->slotContractId);
    return binding &&
        binding.declarationKind() == LUNA_RUNTIME_DECLARATION_SLOT_V1 &&
        binding.flags() == 0 && binding.implementation() &&
        !generation_.find(candidate->symbolId, candidate->contractId);
}

bool VerifiedMoonImportedSlotGeneration::hasMetadataCandidate() const {
    if (!hasImportedSlotBinding()) return false;
    const auto expected = proof_->candidateFacts();
    if (!expected) return false;
    const auto pinned = generation_.findRefUnitCandidate(
        expected->symbolId, expected->contractId);
    const auto actual = pinned.facts();
    return actual && sameRefUnitCandidateFacts(*actual, *expected);
}

bool VerifiedMoonImportedSlotGeneration::hasTypedCaller() const {
    return typedCallerInstalled_ && hasMetadataCandidate();
}

std::optional<luna::runtime::GenerationRefUnitCandidate>
VerifiedMoonImportedSlotGeneration::candidateFacts() const {
    return *this ? proof_->candidateFacts() : std::nullopt;
}

bool VerifiedMoonImportedSlotGeneration::admitMetadataCandidateOnce(
    std::string& error) {
    error.clear();
    if (!hasImportedSlotBinding() || hasMetadataCandidate() ||
        !generation_.matchesModuleLease(proof_) ||
        runtime_->activeGenerationId(generation_.moduleId()) !=
            generation_.generationId()) {
        error = "verified imported-Slot candidate requires the active proxy generation";
        return false;
    }
    const auto expected = proof_->candidateFacts();
    const auto slotBinding = generation_.find(
        expected->slotSymbolId, expected->slotContractId);
    luna::runtime::GenerationStagingRequest request;
    request.moduleId = generation_.moduleId();
    request.contentDigest = generation_.contentDigest();
    request.materializationKey = generation_.materializationKey();
    request.moduleLease = proof_;
    request.refUnitCandidates.push_back(*expected);
    luna::runtime::MoonRuntime::StagedGeneration staged;
    if (!runtime_->stage(
            request,
            [this, &request, &expected](const auto& candidate,
                                        std::string& verificationError) {
                if (candidate.moduleId != request.moduleId ||
                    candidate.contentDigest != request.contentDigest ||
                    candidate.materializationKey != request.materializationKey ||
                    !generation_.matchesModuleLease(candidate.moduleLease) ||
                    candidate.refUnitCandidates.size() != 1 ||
                    !sameRefUnitCandidateFacts(
                        candidate.refUnitCandidates.front(), *expected) ||
                    candidate.refUnitCaller) {
                    verificationError =
                        "verified imported-Slot candidate changed before staging";
                    return false;
                }
                return true;
            },
            [expected, slotBinding](const auto&, auto& bindings,
                                    std::string& resolutionError) {
                if (!bindings.empty()) {
                    resolutionError =
                        "verified imported-Slot candidate cannot replace bindings";
                    return false;
                }
                luna::runtime::GenerationBinding binding;
                binding.symbolId = expected->slotSymbolId;
                binding.contractId = expected->slotContractId;
                binding.implementation = slotBinding.implementation();
                binding.declarationKind = LUNA_RUNTIME_DECLARATION_SLOT_V1;
                bindings.push_back(std::move(binding));
                return true;
            },
            {}, staged, error))
        return false;
    const auto previous = generation_;
    auto safePoint = runtime_->safePoint();
    if (!runtime_->activate(staged, safePoint, error)) return false;
    auto activated = runtime_->pin(request.moduleId);
    const auto activeSlot = activated.find(
        expected->slotSymbolId, expected->slotContractId);
    const auto activeCandidate = activated.findRefUnitCandidate(
        expected->symbolId, expected->contractId);
    const bool validActivation = activated &&
        runtime_->activeGenerationId(request.moduleId) ==
            activated.generationId() &&
        activated.matchesModuleLease(proof_) &&
        activeSlot && activeSlot.flags() == 0 &&
        activeSlot.implementation() == slotBinding.implementation() &&
        !activated.find(expected->symbolId, expected->contractId) &&
        activeCandidate;
    if (validActivation) generation_ = std::move(activated);
    if (!validActivation || !hasMetadataCandidate()) {
        generation_ = previous;
        auto rollbackPoint = runtime_->safePoint();
        std::string rollbackError;
        runtime_->rollback(request.moduleId, previous.generationId(),
                           rollbackPoint, rollbackError);
        error = "verified imported-Slot candidate activation lost its proof";
        if (!rollbackError.empty()) error += ": " + rollbackError;
        return false;
    }
    return true;
}

bool VerifiedMoonImportedSlotGeneration::installTypedCallerOnce(
    std::string& error) {
    error.clear();
    if (!hasMetadataCandidate() || typedCallerInstalled_ ||
        !generation_.matchesModuleLease(proof_) ||
        runtime_->activeGenerationId(generation_.moduleId()) !=
            generation_.generationId()) {
        error = "verified imported-Slot caller requires the active metadata generation";
        return false;
    }
    const auto expected = proof_->candidateFacts();
    const auto& consumer = proof_->consumer_;
    if (!expected || !consumer.matchesPreparation(consumer.preparation_) ||
        !consumer.preparation_.state_) {
        error = "verified imported-Slot caller lost its preparation proof";
        return false;
    }
    const auto& state = *consumer.preparation_.state_;
    const auto slotBinding = generation_.find(
        expected->slotSymbolId, expected->slotContractId);
    const auto ownerBinding = state.ownerGeneration.find(
        expected->slotSymbolId, expected->slotContractId);
    if (!state.codeLease || !state.codeEntry ||
        state.codeLinkageName != expected->linkageName ||
        !slotBinding || !ownerBinding ||
        ownerBinding.declarationKind() != LUNA_RUNTIME_DECLARATION_SLOT_V1 ||
        ownerBinding.flags() !=
            luna::runtime::GenerationBindingPublicControl ||
        ownerBinding.implementation() != slotBinding.implementation()) {
        error = "verified imported-Slot caller lost its code or owner lease";
        return false;
    }

    const auto proofLease = proof_;
    luna::runtime::GenerationStagingRequest request;
    request.moduleId = generation_.moduleId();
    request.contentDigest = generation_.contentDigest();
    request.materializationKey = generation_.materializationKey();
    request.moduleLease = proofLease;
    request.refUnitCandidates.push_back(*expected);
    request.refUnitCaller = [proofLease, facts = *expected](
        const std::string& symbolId, const std::string& contractId,
        const luna::runtime::RuntimeFragmentExecutionContext& parent,
        const luna::runtime::RuntimeFragmentRefHandle& reference,
        int32_t& status, std::string& callError) {
        callError.clear();
        const auto& pinnedConsumer = proofLease->consumer_;
        if (symbolId != facts.symbolId || contractId != facts.contractId ||
            !pinnedConsumer.matchesPreparation(pinnedConsumer.preparation_) ||
            !pinnedConsumer.preparation_.state_) {
            callError = "verified imported-Slot caller lost its candidate proof";
            return false;
        }
        const auto& pinnedState = *pinnedConsumer.preparation_.state_;
        if (!pinnedState.codeLease || !pinnedState.codeEntry ||
            pinnedState.codeLinkageName != facts.linkageName) {
            callError = "verified imported-Slot caller lost its code lease";
            return false;
        }
        const auto pinnedParent = parent;
        if (!pinnedParent ||
            luna_compiler_fragment_context_check(pinnedParent.opaque()) !=
                LUNA_COMPILER_FRAGMENT_OVERRIDE_SUCCESS) {
            callError = "verified Ref/unit parent context is not live";
            return false;
        }
        luna::runtime::RuntimeFragmentRefHandle pinnedReference;
        if (!luna::runtime::pinRuntimeFragmentRefHandle(
                reference, pinnedReference, callError))
            return false;
        if (luna_runtime_fragment_ref_check_v1(
                pinnedReference.opaque(), facts.slotSymbolId.c_str(),
                facts.slotContractId.c_str()) !=
            LUNA_RUNTIME_FRAGMENT_REF_SUCCESS_V1) {
            callError = "verified Ref/unit reference does not match its declared Slot";
            return false;
        }
        using Entry = LunaNativeRefUnitEntryV1;
        static_assert(sizeof(Entry) == sizeof(pinnedState.codeEntry),
                      "host cannot represent a verified Ref/unit function pointer");
        Entry entry = nullptr;
        std::memcpy(&entry, &pinnedState.codeEntry, sizeof(entry));
        const int32_t returned =
            entry(pinnedParent.opaque(), pinnedReference.opaque());
        if (returned < LUNA_NATIVE_REF_UNIT_SUCCESS_V1 ||
            returned > LUNA_NATIVE_REF_UNIT_EXECUTION_FAILURE_V1) {
            callError = "verified Ref/unit entry returned an unknown status";
            return false;
        }
        status = returned;
        return true;
    };
    request.refUnitCallerLease = proofLease;
    luna::runtime::MoonRuntime::StagedGeneration staged;
    if (!runtime_->stage(
            request,
            [this, &request, &expected](const auto& candidate,
                                        std::string& verificationError) {
                if (candidate.moduleId != request.moduleId ||
                    candidate.contentDigest != request.contentDigest ||
                    candidate.materializationKey != request.materializationKey ||
                    !generation_.matchesModuleLease(candidate.moduleLease) ||
                    candidate.refUnitCandidates.size() != 1 ||
                    !sameRefUnitCandidateFacts(
                        candidate.refUnitCandidates.front(), *expected) ||
                    !candidate.refUnitCaller ||
                    !generation_.matchesModuleLease(
                        candidate.refUnitCallerLease)) {
                    verificationError =
                        "verified imported-Slot caller changed before staging";
                    return false;
                }
                return true;
            },
            [expected, slotBinding](const auto&, auto& bindings,
                                    std::string& resolutionError) {
                if (!bindings.empty()) {
                    resolutionError =
                        "verified imported-Slot caller cannot replace bindings";
                    return false;
                }
                luna::runtime::GenerationBinding binding;
                binding.symbolId = expected->slotSymbolId;
                binding.contractId = expected->slotContractId;
                binding.implementation = slotBinding.implementation();
                binding.declarationKind = LUNA_RUNTIME_DECLARATION_SLOT_V1;
                bindings.push_back(std::move(binding));
                return true;
            },
            {}, staged, error))
        return false;
    const auto previous = generation_;
    auto safePoint = runtime_->safePoint();
    if (!runtime_->activate(staged, safePoint, error)) return false;
    auto activated = runtime_->pin(request.moduleId);
    const auto activeSlot = activated.find(
        expected->slotSymbolId, expected->slotContractId);
    const auto activeCandidate = activated.findRefUnitCandidate(
        expected->symbolId, expected->contractId);
    luna::runtime::RuntimeFragmentExecutionContext emptyParent;
    luna::runtime::RuntimeFragmentRefHandle emptyReference;
    int32_t untouchedStatus = 777;
    std::string probeError;
    const bool validActivation = activated &&
        runtime_->activeGenerationId(request.moduleId) ==
            activated.generationId() &&
        activated.matchesModuleLease(proof_) &&
        activeSlot && activeSlot.flags() == 0 &&
        activeSlot.implementation() == slotBinding.implementation() &&
        !activated.find(expected->symbolId, expected->contractId) &&
        activeCandidate &&
        !activeCandidate.call(emptyParent, emptyReference,
                              untouchedStatus, probeError) &&
        probeError == "verified Ref/unit parent context is not live" &&
        untouchedStatus == 777;
    if (validActivation) generation_ = std::move(activated);
    if (!validActivation || !hasMetadataCandidate()) {
        generation_ = previous;
        auto rollbackPoint = runtime_->safePoint();
        std::string rollbackError;
        runtime_->rollback(request.moduleId, previous.generationId(),
                           rollbackPoint, rollbackError);
        error = "verified imported-Slot caller activation lost its proof";
        if (!rollbackError.empty()) error += ": " + rollbackError;
        return false;
    }
    typedCallerInstalled_ = true;
    return true;
}

#ifdef LUNA_PRIVATE_REF_JIT_TEST
luna::runtime::MoonRuntime::PinnedRefUnitCandidate
VerifiedMoonImportedSlotGeneration::pinCandidateForTest() const {
    if (!hasMetadataCandidate()) return {};
    const auto expected = proof_->candidateFacts();
    return generation_.findRefUnitCandidate(
        expected->symbolId, expected->contractId);
}

bool VerifiedMoonImportedSlotGeneration::stageInRuntimeForTest(
    luna::runtime::MoonRuntime& target,
    luna::runtime::MoonRuntime::StagedGeneration& staged,
    std::string& error) const {
    error.clear();
    if (staged || !hasTypedCaller() || &target == runtime_.get() ||
        !generation_.matchesModuleLease(proof_) ||
        runtime_->activeGenerationId(generation_.moduleId()) !=
            generation_.generationId() ||
        target.activeGenerationId(generation_.moduleId()) != 0) {
        error = "private Ref/unit host staging requires a fresh verified caller";
        return false;
    }
    const auto expected = proof_->candidateFacts();
    const auto& consumer = proof_->consumer_;
    if (!expected || !consumer.matchesPreparation(consumer.preparation_) ||
        !consumer.preparation_.state_) {
        error = "private Ref/unit host staging lost its consumer proof";
        return false;
    }
    const auto& state = *consumer.preparation_.state_;
    if (target.activeGenerationId(state.ownerGeneration.moduleId()) != 0) {
        error = "private Ref/unit host staging conflicts with an owner module";
        return false;
    }
    const auto ownerBinding = state.ownerGeneration.find(
        expected->slotSymbolId, expected->slotContractId);
    const auto proxyBinding = generation_.find(
        expected->slotSymbolId, expected->slotContractId);
    const auto pinnedCaller = generation_.findRefUnitCandidate(
        expected->symbolId, expected->contractId);
    if (!state.codeLease || !state.codeEntry ||
        state.codeLinkageName != expected->linkageName ||
        !ownerBinding || !proxyBinding || !pinnedCaller ||
        ownerBinding.declarationKind() != LUNA_RUNTIME_DECLARATION_SLOT_V1 ||
        ownerBinding.flags() !=
            luna::runtime::GenerationBindingPublicControl ||
        proxyBinding.flags() != 0 ||
        ownerBinding.implementation() != proxyBinding.implementation() ||
        pinnedCaller.generationId() != generation_.generationId()) {
        error = "private Ref/unit host staging lost its owner or caller pin";
        return false;
    }
    const auto proofLease = proof_;
    luna::runtime::GenerationStagingRequest request;
    request.moduleId = generation_.moduleId();
    request.contentDigest = generation_.contentDigest();
    request.materializationKey =
        std::string("luna.private-ref-host-admission.v1:llvm=") +
        LLVM_VERSION_STRING + ":ir=O0:orc=default:owner=" +
        state.ownerGeneration.contentDigest();
    request.moduleLease = proofLease;
    request.refUnitCandidates.push_back(*expected);
    request.refUnitCaller = [pinnedCaller, facts = *expected](
        const std::string& symbolId, const std::string& contractId,
        const luna::runtime::RuntimeFragmentExecutionContext& parent,
        const luna::runtime::RuntimeFragmentRefHandle& reference,
        int32_t& status, std::string& callError) {
        if (symbolId != facts.symbolId || contractId != facts.contractId) {
            callError = "private Ref/unit host candidate changed identity";
            return false;
        }
        return pinnedCaller.call(parent, reference, status, callError);
    };
    request.refUnitCallerLease = proofLease;
    if (!target.stage(
            request,
            [this, &request, &expected](const auto& candidate,
                                        std::string& verificationError) {
                if (candidate.moduleId != request.moduleId ||
                    candidate.contentDigest != request.contentDigest ||
                    candidate.materializationKey != request.materializationKey ||
                    !generation_.matchesModuleLease(candidate.moduleLease) ||
                    !generation_.matchesModuleLease(
                        candidate.refUnitCallerLease) ||
                    candidate.refUnitCandidates.size() != 1 ||
                    !sameRefUnitCandidateFacts(
                        candidate.refUnitCandidates.front(), *expected) ||
                    !candidate.refUnitCaller) {
                    verificationError =
                        "private Ref/unit host candidate changed before staging";
                    return false;
                }
                return true;
            },
            [expected, ownerBinding](const auto&, auto& bindings,
                                     std::string& resolutionError) {
                if (!bindings.empty()) {
                    resolutionError =
                        "private Ref/unit host staging cannot replace bindings";
                    return false;
                }
                luna::runtime::GenerationBinding binding;
                binding.symbolId = expected->slotSymbolId;
                binding.contractId = expected->slotContractId;
                binding.implementation = ownerBinding.implementation();
                binding.declarationKind = LUNA_RUNTIME_DECLARATION_SLOT_V1;
                bindings.push_back(std::move(binding));
                return true;
            },
            {}, staged, error))
        return false;
    if (staged.moduleId() != request.moduleId ||
        staged.contentDigest() != request.contentDigest ||
        staged.materializationKey() != request.materializationKey ||
        target.activeGenerationId(request.moduleId) != 0) {
        staged = {};
        error = "private Ref/unit host staging changed its verified artifact";
        return false;
    }
    return true;
}
#endif

bool VerifiedMoonImportedSlotRefUnitProof::loadBindingOnce(
    VerifiedMoonImportedSlotGeneration& loaded,
    std::string& error) const {
    error.clear();
    if (loaded) {
        error = "verified imported-Slot generation output already owns a binding";
        return false;
    }
    const auto candidateFacts = this->candidateFacts();
    if (!*this || !candidateFacts ||
        !consumer_.matchesPreparation(consumer_.preparation_)) {
        error = "verified imported-Slot generation requires an issued proof";
        return false;
    }
    const auto& state = *consumer_.preparation_.state_;
    const moon::DeclarationRef slotReference{
        luna::identity::SymbolId{candidateFacts->slotSymbolId},
        luna::identity::ContractId{candidateFacts->slotContractId}};
    const auto* imported = state.module.findDeclaration(slotReference);
    const auto ownerBinding = state.ownerGeneration.find(
        candidateFacts->slotSymbolId, candidateFacts->slotContractId);
    if (!imported || imported->kind != moon::DeclarationKind::Slot ||
        imported->type.value != candidateFacts->slotTypeId ||
        !ownerBinding ||
        ownerBinding.declarationKind() != LUNA_RUNTIME_DECLARATION_SLOT_V1 ||
        ownerBinding.flags() !=
            luna::runtime::GenerationBindingPublicControl ||
        !ownerBinding.implementation()) {
        error = "verified imported-Slot generation lost its owner binding";
        return false;
    }
    auto proofLease =
        std::make_shared<const VerifiedMoonImportedSlotRefUnitProof>(*this);
    const std::shared_ptr<const void> lease = proofLease;
    luna::runtime::GenerationStagingRequest request;
    request.moduleId = candidateFacts->packageId;
    request.contentDigest = state.contentDigest;
    request.materializationKey =
        std::string("luna.internal-ref-import-slot.v1:llvm=") +
        LLVM_VERSION_STRING + ":ir=O0:orc=default:owner=" +
        state.ownerGeneration.contentDigest();
    request.moduleLease = lease;

    auto runtime = std::make_shared<luna::runtime::MoonRuntime>();
    luna::runtime::MoonRuntime::StagedGeneration staged;
    if (!runtime->stage(
            request,
            [lease, &request](const auto& candidate,
                              std::string& verificationError) {
                if (candidate.moduleId != request.moduleId ||
                    candidate.contentDigest != request.contentDigest ||
                    candidate.materializationKey != request.materializationKey ||
                    candidate.moduleLease.get() != lease.get() ||
                    candidate.moduleLease.owner_before(lease) ||
                    lease.owner_before(candidate.moduleLease) ||
                    !candidate.refUnitCandidates.empty() ||
                    candidate.refUnitCaller) {
                    verificationError =
                        "verified imported-Slot generation changed before staging";
                    return false;
                }
                return true;
            },
            [candidateFacts, ownerBinding](const auto&, auto& bindings,
                                           std::string& resolutionError) {
                if (!bindings.empty()) {
                    resolutionError =
                        "verified imported-Slot generation cannot replace bindings";
                    return false;
                }
                luna::runtime::GenerationBinding binding;
                binding.symbolId = candidateFacts->slotSymbolId;
                binding.contractId = candidateFacts->slotContractId;
                binding.implementation = ownerBinding.implementation();
                binding.declarationKind = LUNA_RUNTIME_DECLARATION_SLOT_V1;
                bindings.push_back(std::move(binding));
                return true;
            },
            {}, staged, error))
        return false;
    if (!staged || staged.moduleId() != request.moduleId ||
        staged.contentDigest() != request.contentDigest ||
        staged.materializationKey() != request.materializationKey) {
        error = "verified imported-Slot staging changed its artifact";
        return false;
    }
    luna::runtime::MoonRuntime::PinnedGeneration generation;
    if (!runtime->loadOnce(staged, generation, error)) return false;
    const auto binding = generation.find(
        candidateFacts->slotSymbolId, candidateFacts->slotContractId);
    if (!generation.matchesModuleLease(lease) || !binding ||
        binding.declarationKind() != LUNA_RUNTIME_DECLARATION_SLOT_V1 ||
        binding.flags() != 0 ||
        binding.implementation() != ownerBinding.implementation() ||
        generation.find(candidateFacts->symbolId, candidateFacts->contractId) ||
        generation.findRefUnitCandidate(
            candidateFacts->symbolId, candidateFacts->contractId)) {
        error = "verified imported-Slot generation published an invalid binding";
        return false;
    }
    loaded.proof_ = std::move(proofLease);
    loaded.runtime_ = std::move(runtime);
    loaded.generation_ = std::move(generation);
    return true;
}

bool loadVerifiedMoonImportedSlotRefUnitOnce(
    const std::vector<uint8_t>& consumerContainerBytes,
    const VerifiedMoonSlotOwner& owner,
    const std::string& expectedTargetTriple,
    const std::string& expectedDataLayout,
    const std::string& sourceSymbolId,
    const std::string& sourceContractId,
    const luna::runtime::RuntimeSlotRequirement& slot,
    VerifiedMoonImportedSlotGeneration& loaded,
    std::string& error) {
    error.clear();
    if (loaded) {
        error = "verified imported-Slot Ref/unit output already owns a generation";
        return false;
    }
    VerifiedMoonRefUnitPreparation preparation;
    if (!loadVerifiedMoonRefUnitPreparation(
            consumerContainerBytes, owner, expectedTargetTriple,
            expectedDataLayout, preparation, error))
        return false;
    VerifiedMoonRefUnitGeneration consumer;
    if (!loadVerifiedMoonRefUnitGenerationOnce(
            preparation, consumer, error))
        return false;
    VerifiedMoonImportedSlotRefUnitProof proof;
    if (!pinVerifiedMoonImportedSlotRefUnitProof(
            consumer, sourceSymbolId, sourceContractId,
            slot, proof, error))
        return false;
    VerifiedMoonImportedSlotGeneration internal;
    if (!proof.loadBindingOnce(internal, error) ||
        !internal.admitMetadataCandidateOnce(error) ||
        !internal.installTypedCallerOnce(error))
        return false;
    const auto facts = internal.candidateFacts();
    if (!internal.hasTypedCaller() || !facts ||
        facts->symbolId != sourceSymbolId ||
        facts->contractId != sourceContractId ||
        facts->slotSymbolId != slot.slotId ||
        facts->slotContractId != slot.contractId) {
        error = "verified imported-Slot Ref/unit load lost its admitted candidate";
        return false;
    }
    loaded = std::move(internal);
    return true;
}

#ifdef LUNA_PRIVATE_REF_JIT_TEST
bool stagePrivateNativeRefUnitPreparationForTest(
    luna::runtime::MoonRuntime& runtime,
    const std::vector<uint8_t>& containerBytes,
    const VerifiedMoonSlotOwner& owner,
    const std::string& expectedTargetTriple,
    const std::string& expectedDataLayout,
    luna::runtime::MoonRuntime::StagedGeneration& staged,
    std::weak_ptr<LunaPrivateRefUnitApplyLoadedEntry>& entry,
    std::weak_ptr<PrivatePreparedRefGenerationForTest>& preparation,
    std::string& error) {
    error.clear();
    if (staged || !entry.expired() || !preparation.expired()) {
        error = "private Ref/unit staging output already owns a generation";
        return false;
    }
    VerifiedMoonRefUnitPreparation verifiedPreparation;
    if (!loadVerifiedMoonRefUnitPreparation(
            containerBytes, owner, expectedTargetTriple,
            expectedDataLayout, verifiedPreparation, error))
        return false;
    const auto& ownerModule = owner.state_->module;
    auto loaded = std::make_shared<PrivatePreparedRefGenerationForTest>();
    if (!moon::ContainerModelCodec::decodeNativeRefUnitPreparationForTarget(
            containerBytes, expectedTargetTriple, expectedDataLayout,
            {ownerModule.localSlotPublication}, loaded->manifest,
            loaded->module, error))
        return false;
    if (loaded->manifest.packageId !=
            verifiedPreparation.state_->manifest.packageId ||
        containerDigest(containerBytes) !=
            verifiedPreparation.state_->contentDigest) {
        error = "private Ref/unit consumer changed after preparation";
        return false;
    }
    moon::FunctionDecl* source = nullptr;
    for (auto& declaration : loaded->module.declarations) {
        auto* function = dynamic_cast<moon::FunctionDecl*>(declaration.get());
        if (!function || !function->controlFlow ||
            function->controlFlow->runtimeRefApplies.empty()) continue;
        source = function;
        break;
    }
    if (!source) {
        error = "private Ref/unit generation lost its Apply source";
        return false;
    }
    luna::codegen::NativeRefUnitSourceFacts sourceFacts;
    if (!luna::codegen::deriveNativeRefUnitSourceFacts(
            loaded->module, *source, sourceFacts, error))
        return false;
    if (sourceFacts.identities() !=
            verifiedPreparation.state_->facts.identities() ||
        sourceFacts.contextEffect !=
            verifiedPreparation.state_->facts.contextEffect) {
        error = "private Ref/unit source changed after preparation";
        return false;
    }
    loaded->verifiedPreparation = std::move(verifiedPreparation);
    std::vector<uint8_t> entryRecord;
    auto executable = CodeGenerator::materializePrivateRuntimeFragmentRefApplyForTest(
        loaded->module, *source, error, &entryRecord);
    if (!executable) return false;
    auto loadedEntry = CodeGenerator::loadPrivateRuntimeFragmentRefApplyEntryForTest(
        loaded->module, *source, entryRecord, std::move(executable), error);
    if (!loadedEntry) return false;
    loaded->entry = std::move(loadedEntry);
    loaded->contentDigest = loaded->verifiedPreparation.state_->contentDigest;

    luna::runtime::GenerationStagingRequest request;
    request.moduleId = loaded->manifest.packageId;
    request.contentDigest = loaded->contentDigest;
    request.materializationKey =
        std::string("luna.private-ref-unit-preparation.v1:llvm=") +
        LLVM_VERSION_STRING + ":ir=O0:orc=default";
    request.moduleLease = loaded;
    if (!runtime.stage(
            request,
            [loaded](const auto& candidate, std::string& verificationError) {
                if (candidate.moduleId != loaded->manifest.packageId ||
                    candidate.contentDigest != loaded->contentDigest ||
                    candidate.moduleLease.get() != loaded.get() ||
                    !candidate.refUnitCandidates.empty() || candidate.refUnitCaller) {
                    verificationError =
                        "private Ref/unit generation identity changed before staging";
                    return false;
                }
                return true;
            },
            [](const auto&, auto& bindings, std::string& resolutionError) {
                if (!bindings.empty()) {
                    resolutionError =
                        "private Ref/unit generation cannot publish bindings";
                    return false;
                }
                return true;
            },
            {}, staged, error))
        return false;
    entry = loaded->entry;
    preparation = loaded;
    return true;
}

bool discardPrivateRefUnitTestEntryForTest(
    const std::weak_ptr<PrivatePreparedRefGenerationForTest>& preparation,
    std::string& error) {
    const auto loaded = preparation.lock();
    if (!loaded || !loaded->entry) {
        error = "private Ref/unit test entry is already unavailable";
        return false;
    }
    loaded->entry.reset();
    error.clear();
    return true;
}

uint64_t PrivateImportedSlotRefUnitCandidateForTest::consumerGenerationId() const {
    return consumer_.generationId();
}

uint64_t PrivateImportedSlotRefUnitCandidateForTest::ownerGenerationId() const {
    const auto prepared = preparation_.lock();
    return prepared ? prepared->verifiedPreparation.ownerGenerationId() : 0;
}

std::optional<luna::codegen::NativeRefUnitSourceFacts>
PrivateImportedSlotRefUnitCandidateForTest::facts() const {
    const auto prepared = preparation_.lock();
    if (!prepared || !consumer_.matchesModuleLease(prepared) ||
        !prepared->verifiedPreparation.matchesConsumer(consumer_))
        return std::nullopt;
    return prepared->verifiedPreparation.facts();
}

bool PrivateImportedSlotRefUnitCandidateForTest::call(
    const luna::runtime::RuntimeFragmentExecutionContext& parent,
    const luna::runtime::RuntimeFragmentRefHandle& reference,
    int32_t& status, std::string& error) const {
    const auto prepared = preparation_.lock();
    if (!prepared || !consumer_.matchesModuleLease(prepared) ||
        !prepared->verifiedPreparation.matchesConsumer(consumer_) ||
        !prepared->verifiedPreparation.ownerGenerationId()) {
        error = "private imported-Slot Ref/unit candidate lost its generation lease";
        return false;
    }
    static_assert(sizeof(LunaNativeRefUnitEntryV1) == sizeof(const void*));
    LunaNativeRefUnitEntryV1 entry = nullptr;
    const void* codeEntry = prepared->verifiedPreparation.state_->codeEntry;
    std::memcpy(&entry, &codeEntry, sizeof(entry));
    status = entry(parent.opaque(), reference.opaque());
    error.clear();
    return true;
}

bool pinPrivateImportedSlotRefUnitCandidateForTest(
    const luna::runtime::MoonRuntime::PinnedGeneration& consumerGeneration,
    const std::weak_ptr<PrivatePreparedRefGenerationForTest>& preparation,
    const std::string& sourceSymbolId, const std::string& sourceContractId,
    const luna::runtime::RuntimeSlotRequirement& slot,
    PrivateImportedSlotRefUnitCandidateForTest& candidate,
    std::string& error) {
    error.clear();
    const auto prepared = preparation.lock();
    const auto facts = prepared ? prepared->verifiedPreparation.facts()
                                : std::nullopt;
    if (candidate || !prepared || !consumerGeneration ||
        !consumerGeneration.matchesModuleLease(prepared) ||
        consumerGeneration.moduleId() != prepared->manifest.packageId ||
        consumerGeneration.contentDigest() != prepared->contentDigest ||
        !prepared->verifiedPreparation.matchesConsumer(consumerGeneration) ||
        !prepared->verifiedPreparation.ownerGenerationId() ||
        !facts ||
        facts->functionSymbolId != sourceSymbolId ||
        facts->functionContractId != sourceContractId ||
        facts->slotSymbolId != slot.slotId ||
        facts->slotContractId != slot.contractId) {
        error = "private imported-Slot Ref/unit candidate identity does not match its generation";
        return false;
    }
    candidate.consumer_ = consumerGeneration;
    candidate.preparation_ = preparation;
    return true;
}
#endif

bool loadVerifiedMoonGenerationOnce(
    luna::runtime::MoonRuntime& runtime,
    const std::vector<uint8_t>& containerBytes,
    const std::string& expectedTargetTriple,
    const std::string& expectedDataLayout,
    luna::runtime::MoonRuntime::PinnedGeneration& loaded,
    std::string& error, const moon::SlotPublicationDependencies& dependencies,
    MoonJitOptimization optimization) {
    error.clear();
    if (loaded) {
        error = "Moon load-once output already owns a generation";
        return false;
    }
    LunaOptimizationLevel level;
    std::string materializationKey;
    if (!jitConfiguration(optimization, level, materializationKey, error)) return false;
    moon::ContainerManifest manifest;
    moon::Module module;
    if (!moon::ContainerModelCodec::decodeContainerForTarget(
            containerBytes, expectedTargetTriple, expectedDataLayout,
            manifest, module, error, {}, dependencies))
        return false;
    const std::string digest = containerDigest(containerBytes);
    auto existing = runtime.pin(manifest.packageId);
    if (existing) {
        if (existing.contentDigest() != digest) {
            error = "module is already loaded with different content";
            return false;
        }
        if (existing.materializationKey() != materializationKey) {
            error = "module is already loaded with different materialization configuration";
            return false;
        }
        loaded = std::move(existing);
        return true;
    }
    luna::runtime::MoonRuntime::StagedGeneration staged;
    if (!stageVerifiedMoonGeneration(
            runtime, containerBytes, expectedTargetTriple,
            expectedDataLayout, {}, staged, error, dependencies, optimization))
        return false;
    return runtime.loadOnce(staged, loaded, error);
}

} // namespace luna::driver
