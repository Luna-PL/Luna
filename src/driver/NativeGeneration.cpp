#include "driver/NativeGeneration.h"

#include "driver/NativeArtifact.h"
#include "runtime/RuntimeDescriptorABI.h"

#include <memory>
#include <utility>
#include <vector>

namespace luna::driver {

static_assert(
    static_cast<uint32_t>(luna::runtime::GenerationBindingCallable) ==
        static_cast<uint32_t>(LUNA_NATIVE_EXPORT_CALLABLE_V1),
    "Native and internal generation callable flags must agree");
static_assert(
    static_cast<uint32_t>(luna::runtime::GenerationEntryAbiCI32NoArgsV1) ==
        static_cast<uint32_t>(LUNA_NATIVE_ENTRY_ABI_C_I32_NOARGS_V1),
    "Native and internal generation entry profiles must agree");
static_assert(
    static_cast<uint32_t>(luna::runtime::GenerationEntryAbiRefUnitV1) ==
        static_cast<uint32_t>(LUNA_NATIVE_ENTRY_ABI_REF_UNIT_V1),
    "Native and internal Ref/unit candidate profiles must agree");
static_assert(
    static_cast<uint32_t>(LUNA_NATIVE_DECLARATION_FUNCTION_V1) ==
        static_cast<uint32_t>(LUNA_RUNTIME_DECLARATION_FUNCTION_V1) &&
    static_cast<uint32_t>(LUNA_NATIVE_DECLARATION_FRAGMENT_V1) ==
        static_cast<uint32_t>(LUNA_RUNTIME_DECLARATION_FRAGMENT_V1) &&
    static_cast<uint32_t>(LUNA_NATIVE_DECLARATION_STRUCT_V1) ==
        static_cast<uint32_t>(LUNA_RUNTIME_DECLARATION_STRUCT_V1) &&
    static_cast<uint32_t>(LUNA_NATIVE_DECLARATION_ENUM_V1) ==
        static_cast<uint32_t>(LUNA_RUNTIME_DECLARATION_ENUM_V1) &&
    static_cast<uint32_t>(LUNA_NATIVE_DECLARATION_TRAIT_V1) ==
        static_cast<uint32_t>(LUNA_RUNTIME_DECLARATION_TRAIT_V1) &&
    static_cast<uint32_t>(LUNA_NATIVE_DECLARATION_IMPLEMENTATION_V1) ==
        static_cast<uint32_t>(LUNA_RUNTIME_DECLARATION_IMPLEMENTATION_V1) &&
    static_cast<uint32_t>(LUNA_NATIVE_DECLARATION_METADATA_SCHEMA_V1) ==
        static_cast<uint32_t>(LUNA_RUNTIME_DECLARATION_METADATA_SCHEMA_V1) &&
    static_cast<uint32_t>(LUNA_NATIVE_DECLARATION_SLOT_V1) ==
        static_cast<uint32_t>(LUNA_RUNTIME_DECLARATION_SLOT_V1),
    "Native and Runtime descriptor declaration kinds must agree");
static_assert(
    static_cast<uint32_t>(LUNA_NATIVE_EXPORT_CALLABLE_V1) ==
        static_cast<uint32_t>(LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1),
    "Native and Runtime descriptor callable flags must agree");

namespace {

using NativeLibraryPin = std::shared_ptr<const VerifiedNativeLibrary>;

bool sameLibraryLease(
    const std::shared_ptr<const void>& actual,
    const NativeLibraryPin& expected) {
    return actual && actual.get() == expected.get() &&
        !actual.owner_before(expected) && !expected.owner_before(actual);
}

bool sameRefUnitFacts(
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

struct VerifiedNativeRefUnitCaller {
    NativeLibraryPin library;

    bool operator()(
        const std::string& symbolId, const std::string& contractId,
        const luna::runtime::RuntimeFragmentExecutionContext& parent,
        const luna::runtime::RuntimeFragmentRefHandle& reference,
        int32_t& status, std::string& error) const {
        return callVerifiedNativeRefUnit(
            library, symbolId, contractId, parent, reference, status, error);
    }
};

bool prepareNativeRequest(
    const NativeLibraryPin& library,
    luna::runtime::GenerationStagingRequest& request,
    std::string& error) {
    request.moduleId = library->proof().packageId;
    request.contentDigest = nativeDigestHex(library->proof().artifactDigest);
    request.moduleLease = library;
    request.refUnitCandidates.reserve(
        static_cast<size_t>(library->refUnitCandidateCount()));
    for (uint64_t index = 0; index < library->refUnitCandidateCount(); ++index) {
        luna::runtime::GenerationRefUnitCandidate candidate;
        if (!library->refUnitCandidateAt(index, candidate)) {
            error = "verified Native Ref/unit registry changed during staging";
            return false;
        }
        request.refUnitCandidates.push_back(std::move(candidate));
    }
    if (!request.refUnitCandidates.empty()) {
        request.refUnitCaller = VerifiedNativeRefUnitCaller{library};
        request.refUnitCallerLease = library;
    }
    return true;
}

bool verifyNativeRequest(
    const luna::runtime::GenerationStagingRequest& candidate,
    const NativeLibraryPin& library, std::string& error) {
    if (!library || !*library) {
        error = "verified Native generation request lost its artifact proof";
        return false;
    }
    const bool hasCandidates = library->refUnitCandidateCount() != 0;
    const auto* caller =
        candidate.refUnitCaller.target<VerifiedNativeRefUnitCaller>();
    if (candidate.moduleId != library->proof().packageId ||
        candidate.contentDigest != nativeDigestHex(
            library->proof().artifactDigest) ||
        !sameLibraryLease(candidate.moduleLease, library) ||
        !candidate.materializationKey.empty() ||
        !candidate.requiredAbsentModuleIds.empty() ||
        !candidate.requiredActiveModules.empty() ||
        candidate.refUnitCandidates.size() !=
            library->refUnitCandidateCount() ||
        (hasCandidates &&
         (!caller || !sameLibraryLease(caller->library, library) ||
          !sameLibraryLease(candidate.refUnitCallerLease, library))) ||
        (!hasCandidates &&
         (candidate.refUnitCaller || candidate.refUnitCallerLease))) {
        error = "verified Native generation request lost its artifact proof";
        return false;
    }
    for (size_t index = 0; index < candidate.refUnitCandidates.size(); ++index) {
        luna::runtime::GenerationRefUnitCandidate expected;
        if (!library->refUnitCandidateAt(index, expected) ||
            !sameRefUnitFacts(candidate.refUnitCandidates[index], expected)) {
            error = "verified Native generation request lost its artifact proof";
            return false;
        }
    }
    return true;
}

bool loadNativeRequest(
    const std::string& artifactPath, const std::string& trustStorePath,
    NativeLibraryPin& library,
    luna::runtime::GenerationStagingRequest& request,
    std::string& error) {
    auto loaded = std::make_shared<VerifiedNativeLibrary>();
    if (!loadVerifiedNativeLibrary(
            artifactPath, trustStorePath, *loaded, error))
        return false;
    library = std::move(loaded);
    return prepareNativeRequest(library, request, error);
}

} // namespace

bool stageVerifiedNativeGeneration(
    luna::runtime::MoonRuntime& runtime,
    const std::string& artifactPath, const std::string& trustStorePath,
    const luna::runtime::GenerationInitializer& initializer,
    luna::runtime::MoonRuntime::StagedGeneration& staged,
    std::string& error) {
    NativeLibraryPin library;
    luna::runtime::GenerationStagingRequest request;
    if (!loadNativeRequest(
            artifactPath, trustStorePath, library, request, error))
        return false;
    return runtime.stage(
        request,
        [library](const auto& candidate, std::string& verificationError) {
            return verifyNativeRequest(
                candidate, library, verificationError);
        },
        [library](const auto&, auto& bindings, std::string& resolutionError) {
            bindings.reserve(static_cast<size_t>(library->exportCount()));
            for (uint64_t index = 0; index < library->exportCount(); ++index) {
                const auto* exported = library->exportAt(index);
                if (!exported) {
                    resolutionError =
                        "verified Native registry changed during resolution";
                    return false;
                }
                luna::runtime::GenerationBinding binding;
                binding.symbolId = exported->symbol_id;
                binding.contractId = exported->contract_id;
                binding.implementation = exported->entry
                    ? exported->entry : static_cast<const void*>(exported);
                binding.declarationKind = exported->declaration_kind;
                binding.flags = exported->flags;
                binding.entryAbi = library->entryAbiForExport(
                    binding.symbolId, binding.contractId);
                bindings.push_back(std::move(binding));
            }
            return true;
        },
        initializer, staged, error);
}

#ifdef LUNA_NATIVE_ADMISSION_TEST
bool verifyNativeRequestMutationForTest(
    const std::string& artifactPath, const std::string& trustStorePath,
    const std::function<void(luna::runtime::GenerationStagingRequest&)>&
        mutate,
    std::string& error) {
    error.clear();
    if (!mutate) {
        error = "Native request probe requires a mutation";
        return false;
    }
    NativeLibraryPin library;
    luna::runtime::GenerationStagingRequest request;
    if (!loadNativeRequest(
            artifactPath, trustStorePath, library, request, error))
        return false;
    mutate(request);
    return verifyNativeRequest(request, library, error);
}

bool verifyAlternateNativeCallerForTest(
    const std::string& artifactPath, const std::string& trustStorePath,
    std::string& error) {
    error.clear();
    NativeLibraryPin library;
    NativeLibraryPin alternateLibrary;
    luna::runtime::GenerationStagingRequest request;
    luna::runtime::GenerationStagingRequest alternate;
    if (!loadNativeRequest(
            artifactPath, trustStorePath, library, request, error) ||
        !loadNativeRequest(
            artifactPath, trustStorePath, alternateLibrary,
            alternate, error))
        return false;
    if (!request.refUnitCaller || !alternate.refUnitCaller) {
        error = "Native alternate caller probe requires a Ref/unit candidate";
        return false;
    }
    request.refUnitCaller = alternate.refUnitCaller;
    return verifyNativeRequest(request, library, error);
}
#endif

bool loadVerifiedNativeGenerationOnce(
    luna::runtime::MoonRuntime& runtime,
    const std::string& artifactPath, const std::string& trustStorePath,
    luna::runtime::MoonRuntime::PinnedGeneration& loaded,
    std::string& error) {
    if (loaded) {
        error = "Native load-once output already owns a generation";
        return false;
    }
    NativeProofInfo proof;
    if (!verifyNativeArtifact(artifactPath, trustStorePath, proof, error))
        return false;
    auto existing = runtime.pin(proof.packageId);
    if (existing) {
        const std::string digest = nativeDigestHex(proof.artifactDigest);
        if (existing.contentDigest() != digest) {
            error = "module is already loaded with different content";
            return false;
        }
        loaded = std::move(existing);
        return true;
    }
    luna::runtime::MoonRuntime::StagedGeneration staged;
    if (!stageVerifiedNativeGeneration(
            runtime, artifactPath, trustStorePath, {}, staged, error))
        return false;
    if (staged.hasRefUnitCaller()) {
        const uint64_t stagedId = staged.generationId();
        auto safePoint = runtime.safePoint();
        if (!runtime.activate(staged, safePoint, error)) return false;
        auto activated = runtime.pin(proof.packageId);
        if (!activated || activated.generationId() != stagedId ||
            activated.contentDigest() != nativeDigestHex(proof.artifactDigest)) {
            error = "verified Native Ref/unit activation lost its proof";
            return false;
        }
        loaded = std::move(activated);
        return true;
    }
    return runtime.loadOnce(staged, loaded, error);
}

} // namespace luna::driver
