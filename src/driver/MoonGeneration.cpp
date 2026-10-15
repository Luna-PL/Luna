#include "driver/MoonGeneration.h"

#include "codegen/CodeGenerator.h"
#include "codegen/NativeRefUnitFacts.h"
#include "diagnostics/Diagnostic.h"
#include "moonir/ContainerModel.h"
#include "runtime/NativeArtifactABI.h"
#include "runtime/RuntimeDescriptor.h"
#include "runtime/RuntimeFragment.h"

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
            verified->module, *source, verified->codeEntry, error);
    if (!verified->codeLease || !verified->codeEntry)
        return false;
    verified->contentDigest = containerDigest(consumerContainerBytes);
    preparation.state_ = std::move(verified);
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
