#include "driver/MoonGeneration.h"

#include "codegen/CodeGenerator.h"
#include "diagnostics/Diagnostic.h"
#include "moonir/ContainerModel.h"
#include "runtime/RuntimeDescriptor.h"
#include "runtime/RuntimeFragment.h"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/SHA256.h>

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <utility>

namespace luna::driver {
namespace {

struct LoadedMoonGeneration {
    moon::ContainerManifest manifest;
    moon::Module module;
    std::string contentDigest;
    std::shared_ptr<LunaJitModule> jitLease;
    luna::runtime::RuntimeDescriptorRegistryView runtimeDescriptors;
};

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
    std::string& error) {
    auto loaded = std::make_shared<LoadedMoonGeneration>();
    if (!moon::ContainerModelCodec::decodeContainerForTarget(
            containerBytes, expectedTargetTriple, expectedDataLayout,
            loaded->manifest, loaded->module, error))
        return false;
    loaded->contentDigest = containerDigest(containerBytes);

    CodeGenerator codeGenerator(
        loaded->manifest.packageId + "@" + loaded->contentDigest);
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
    request.moduleLease = loaded;
    return runtime.stage(
        request,
        [loaded](const auto& candidate, std::string& verificationError) {
            if (candidate.moduleId != loaded->manifest.packageId ||
                candidate.contentDigest != loaded->contentDigest) {
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

bool loadVerifiedMoonGenerationOnce(
    luna::runtime::MoonRuntime& runtime,
    const std::vector<uint8_t>& containerBytes,
    const std::string& expectedTargetTriple,
    const std::string& expectedDataLayout,
    luna::runtime::MoonRuntime::PinnedGeneration& loaded,
    std::string& error) {
    if (loaded) {
        error = "Moon load-once output already owns a generation";
        return false;
    }
    moon::ContainerManifest manifest;
    moon::Module module;
    if (!moon::ContainerModelCodec::decodeContainerForTarget(
            containerBytes, expectedTargetTriple, expectedDataLayout,
            manifest, module, error))
        return false;
    const std::string digest = containerDigest(containerBytes);
    auto existing = runtime.pin(manifest.packageId);
    if (existing) {
        if (existing.contentDigest() != digest) {
            error = "module is already loaded with different content";
            return false;
        }
        loaded = std::move(existing);
        return true;
    }
    luna::runtime::MoonRuntime::StagedGeneration staged;
    if (!stageVerifiedMoonGeneration(
            runtime, containerBytes, expectedTargetTriple,
            expectedDataLayout, {}, staged, error))
        return false;
    return runtime.loadOnce(staged, loaded, error);
}

} // namespace luna::driver
