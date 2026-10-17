#include "driver/NativeGeneration.h"

#include "driver/NativeArtifact.h"
#include "runtime/RuntimeDescriptorABI.h"

#include <algorithm>
#include <memory>
#include <mutex>
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

struct VerifiedNativeLeaseRegistry {
    std::mutex mutex;
    std::vector<std::weak_ptr<const VerifiedNativeLibrary>> libraries;
};

VerifiedNativeLeaseRegistry& nativeLeaseRegistry() {
    static VerifiedNativeLeaseRegistry registry;
    return registry;
}

bool sameNativeProof(const NativeProofInfo& left,
                     const NativeProofInfo& right) {
    return left.packageId == right.packageId &&
        left.packageVersion == right.packageVersion &&
        left.targetAbi == right.targetAbi &&
        left.compilerIdentity == right.compilerIdentity &&
        left.artifactDigest == right.artifactDigest &&
        left.exportDigest == right.exportDigest &&
        left.dependencyDigest == right.dependencyDigest;
}

void rememberNativeLibrary(const NativeLibraryPin& library) {
    auto& registry = nativeLeaseRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.libraries.erase(std::remove_if(
        registry.libraries.begin(), registry.libraries.end(),
        [](const auto& weak) { return weak.expired(); }),
        registry.libraries.end());
    registry.libraries.push_back(library);
}

NativeLibraryPin findNativeLibraryLease(
    const luna::runtime::MoonRuntime::PinnedGeneration& active,
    const NativeProofInfo& proof) {
    auto& registry = nativeLeaseRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    for (auto it = registry.libraries.begin();
         it != registry.libraries.end();) {
        const auto library = it->lock();
        if (!library) {
            it = registry.libraries.erase(it);
            continue;
        }
        if (active.matchesModuleLease(library) &&
            sameNativeProof(library->proof(), proof))
            return library;
        ++it;
    }
    return {};
}

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

bool resolveNativeBindings(
    const NativeLibraryPin& library,
    std::vector<luna::runtime::GenerationBinding>& bindings,
    std::string& error) {
    if (!bindings.empty()) {
        error = "verified Native resolution cannot replace bindings";
        return false;
    }
    bindings.reserve(static_cast<size_t>(library->exportCount()));
    for (uint64_t index = 0; index < library->exportCount(); ++index) {
        const auto* exported = library->exportAt(index);
        if (!exported) {
            error = "verified Native registry changed during resolution";
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
}

bool matchesVerifiedNativeSnapshot(
    const luna::runtime::MoonRuntime& runtime,
    const luna::runtime::MoonRuntime::PinnedGeneration& active,
    const luna::runtime::GenerationStagingRequest& expected,
    const std::vector<luna::runtime::GenerationBinding>& bindings) {
    if (!expected.refUnitCaller)
        return runtime.matchesActiveGenerationSnapshot(
            active, expected, bindings);
    const auto* trustedCaller =
        expected.refUnitCaller.target<VerifiedNativeRefUnitCaller>();
    if (!trustedCaller || !trustedCaller->library) return false;
    const auto library = trustedCaller->library;
    return runtime.matchesActiveGenerationSnapshot(
        active, expected, bindings,
        [library](const luna::runtime::GenerationRefUnitCaller& actual) {
            const auto* caller = actual.target<VerifiedNativeRefUnitCaller>();
            return caller && sameLibraryLease(caller->library, library);
        });
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
    if (!prepareNativeRequest(library, request, error)) return false;
    rememberNativeLibrary(library);
    return true;
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
            return resolveNativeBindings(
                library, bindings, resolutionError);
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

namespace {

using NativeBeforePublication = std::function<bool(
    const luna::runtime::GenerationStagingRequest&,
    const std::vector<luna::runtime::GenerationBinding>&,
    std::string&)>;

bool loadVerifiedNativeGenerationOnceImpl(
    luna::runtime::MoonRuntime& runtime,
    const std::string& artifactPath, const std::string& trustStorePath,
    luna::runtime::MoonRuntime::PinnedGeneration& loaded,
    std::string& error,
    const NativeBeforePublication& beforePublication) {
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
        const auto library = findNativeLibraryLease(existing, proof);
        if (!library) {
            error = "same-content Native generation has no verified loader lease";
            return false;
        }
        luna::runtime::GenerationStagingRequest expected;
        std::vector<luna::runtime::GenerationBinding> bindings;
        if (!prepareNativeRequest(library, expected, error) ||
            !resolveNativeBindings(library, bindings, error))
            return false;
        if (!matchesVerifiedNativeSnapshot(
                runtime, existing, expected, bindings)) {
            error = "same-content Native generation changed its verified snapshot";
            return false;
        }
        loaded = std::move(existing);
        return true;
    }
    luna::runtime::MoonRuntime::StagedGeneration staged;
    luna::runtime::GenerationStagingRequest stagedRequest;
    std::vector<luna::runtime::GenerationBinding> stagedBindings;
    if (!stageVerifiedNativeGeneration(
            runtime, artifactPath, trustStorePath,
            [&](const auto& request, const auto& bindings, auto&) {
                stagedRequest = request;
                stagedBindings = bindings;
                return true;
            }, staged, error))
        return false;
    if (beforePublication &&
        !beforePublication(stagedRequest, stagedBindings, error)) {
        if (error.empty()) error = "Native pre-publication probe failed";
        return false;
    }
    if (staged.hasRefUnitCaller()) {
        const uint64_t stagedId = staged.generationId();
        auto safePoint = runtime.safePoint();
        if (!runtime.activate(staged, safePoint, error)) return false;
        auto activated = runtime.pin(proof.packageId);
        if (!activated || activated.generationId() != stagedId ||
            activated.contentDigest() != nativeDigestHex(proof.artifactDigest) ||
            !matchesVerifiedNativeSnapshot(
                runtime, activated, stagedRequest, stagedBindings)) {
            error = "verified Native Ref/unit activation lost its proof";
            return false;
        }
        loaded = std::move(activated);
        return true;
    }
    if (!runtime.loadOnce(staged, loaded, error)) return false;
    if (!matchesVerifiedNativeSnapshot(
            runtime, loaded, stagedRequest, stagedBindings)) {
        loaded = {};
        error = "verified Native load-once changed its staged snapshot";
        return false;
    }
    return true;
}

} // namespace

bool loadVerifiedNativeGenerationOnce(
    luna::runtime::MoonRuntime& runtime,
    const std::string& artifactPath, const std::string& trustStorePath,
    luna::runtime::MoonRuntime::PinnedGeneration& loaded,
    std::string& error) {
    return loadVerifiedNativeGenerationOnceImpl(
        runtime, artifactPath, trustStorePath, loaded, error, {});
}

#ifdef LUNA_NATIVE_ADMISSION_TEST
bool loadVerifiedNativeGenerationOnceWithPrePublishHookForTest(
    luna::runtime::MoonRuntime& runtime,
    const std::string& artifactPath, const std::string& trustStorePath,
    luna::runtime::MoonRuntime::PinnedGeneration& loaded,
    std::string& error,
    const std::function<bool(
        const luna::runtime::GenerationStagingRequest&,
        const std::vector<luna::runtime::GenerationBinding>&,
        std::string&)>& beforePublication) {
    return loadVerifiedNativeGenerationOnceImpl(
        runtime, artifactPath, trustStorePath, loaded, error,
        beforePublication);
}
#endif

} // namespace luna::driver
