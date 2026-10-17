#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace luna::runtime {

class RuntimeFragmentBindingSet;
struct RuntimeFragmentBindingSetState;
class RuntimeFragmentExecutionContext;
class RuntimeFragmentRefHandle;

enum GenerationBindingFlag : uint32_t {
    GenerationBindingCallable = 1u << 0,
    GenerationBindingFragmentExecutable = 1u << 1,
    // The binding names a host-visible exported Slot/Fragment control.
    // Publication alone does not imply that a Fragment has executable glue.
    GenerationBindingPublicControl = 1u << 2,
    // Callable implementation takes a leading opaque Fragment execution
    // context before its declared source parameters.
    GenerationBindingFragmentContext = 1u << 3,
};

enum GenerationEntryAbi : uint32_t {
    GenerationEntryAbiUnprofiled = 0,
    GenerationEntryAbiCI32NoArgsV1 = 1,
    GenerationEntryAbiRefUnitV1 = 3,
    GenerationEntryAbiAny = UINT32_MAX,
};

// Pointer-free, loader-verified facts for a v4 Ref/unit candidate. These stay
// separate from ordinary callable GenerationBinding rows; a trusted adapter
// owns the typed call and its generation lease.
struct GenerationRefUnitCandidate {
    std::string symbolId;
    std::string contractId;
    std::string linkageName;
    std::string moduleName;
    std::string packageId;
    std::string functionTypeId;
    std::string refTypeId;
    std::string slotSymbolId;
    std::string slotContractId;
    std::string slotTypeId;
    uint32_t entryAbi = 0;
    uint32_t refMode = 0;
    uint32_t resultMode = 0;
    uint32_t statusDomain = 0;
};

using GenerationRefUnitCaller = std::function<bool(
    const std::string&, const std::string&,
    const RuntimeFragmentExecutionContext&, const RuntimeFragmentRefHandle&,
    int32_t&, std::string&)>;

struct GenerationActiveModuleRequirement {
    std::string moduleId;
    uint64_t generationId = 0;

    bool operator==(const GenerationActiveModuleRequirement& other) const {
        return moduleId == other.moduleId &&
            generationId == other.generationId;
    }
};

// Public 0.3 C++ host control-plane input. The module lease keeps the verified
// code and descriptor storage alive for every pinned reference.
struct GenerationStagingRequest {
    std::string moduleId;
    std::string contentDigest;
    std::shared_ptr<const void> moduleLease;
    // Optional loader-owned code-generation identity, separate from the
    // artifact digest. Empty preserves the original unprofiled host behavior.
    std::string materializationKey = {};
    std::vector<GenerationRefUnitCandidate> refUnitCandidates = {};
    // Trusted loader adapter. Kept apart from pointer-free candidate facts
    // and ordinary callable bindings. The witness must be the exact module
    // lease (address and shared ownership); Runtime retains it with the caller.
    GenerationRefUnitCaller refUnitCaller = {};
    std::shared_ptr<const void> refUnitCallerLease = {};
    // Active generations of these modules are incompatible with this
    // generation. Runtime checks both sides of the exclusion at publication.
    std::vector<std::string> requiredAbsentModuleIds = {};
    // Exact active generations required by this generation. The trusted
    // loader must separately authenticate and retain their code leases.
    std::vector<GenerationActiveModuleRequirement> requiredActiveModules = {};
};

struct GenerationBinding {
    std::string symbolId;
    std::string contractId;
    const void* implementation = nullptr;
    uint32_t declarationKind = 0;
    uint32_t flags = 0;
    uint32_t entryAbi = GenerationEntryAbiUnprofiled;
};

// A runtime typed-reference requirement. ContractId carries the canonical
// callable/value contract while declarationKind, requiredFlags and entryAbi
// prevent a loader adapter from changing the representation under that identity.
struct GenerationBindingRequirement {
    std::string symbolId;
    std::string contractId;
    uint32_t declarationKind = 0;
    uint32_t requiredFlags = 0;
    // Existing four-field requirements accept any profile; explicit zero
    // requires an unprofiled binding.
    uint32_t entryAbi = GenerationEntryAbiAny;
};

using GenerationVerifier = std::function<bool(
    const GenerationStagingRequest&, std::string&)>;
using GenerationResolver = std::function<bool(
    const GenerationStagingRequest&, std::vector<GenerationBinding>&,
    std::string&)>;
using GenerationInitializer = std::function<bool(
    const GenerationStagingRequest&, const std::vector<GenerationBinding>&,
    std::string&)>;

class MoonRuntime {
    struct GenerationState;
    struct ModuleState;

public:
    class PinnedGeneration;
    class PinnedRefUnitCandidate {
    public:
        explicit operator bool() const { return candidate_ != nullptr; }
        uint64_t generationId() const;
        std::optional<GenerationRefUnitCandidate> facts() const;
        bool call(const RuntimeFragmentExecutionContext& parent,
                  const RuntimeFragmentRefHandle& reference,
                  int32_t& status, std::string& error) const;

    private:
        friend class PinnedGeneration;
        std::shared_ptr<const GenerationState> generation_;
        const GenerationRefUnitCandidate* candidate_ = nullptr;
    };

    class SafePoint {
    public:
        SafePoint(const SafePoint&) = delete;
        SafePoint& operator=(const SafePoint&) = delete;
        SafePoint(SafePoint&& other) noexcept;
        SafePoint& operator=(SafePoint&& other) noexcept;

    private:
        friend class MoonRuntime;
        explicit SafePoint(const MoonRuntime* owner) : owner_(owner) {}
        const MoonRuntime* owner_ = nullptr;
        bool consumed_ = false;
    };

    class StagedGeneration {
    public:
        StagedGeneration() = default;
        StagedGeneration(const StagedGeneration&) = delete;
        StagedGeneration& operator=(const StagedGeneration&) = delete;
        StagedGeneration(StagedGeneration&&) noexcept = default;
        StagedGeneration& operator=(StagedGeneration&&) noexcept = default;

        explicit operator bool() const { return generation_ != nullptr; }
        uint64_t generationId() const;
        const std::string& moduleId() const;
        const std::string& contentDigest() const;
        const std::string& materializationKey() const;
        bool hasRefUnitCaller() const;

    private:
        friend class MoonRuntime;
        const MoonRuntime* owner_ = nullptr;
        std::shared_ptr<const GenerationState> generation_;
    };

    class PinnedBinding {
    public:
        explicit operator bool() const { return binding_ != nullptr; }
        uint64_t generationId() const;
        const std::string& symbolId() const;
        const std::string& contractId() const;
        const void* implementation() const;
        uint32_t declarationKind() const;
        uint32_t flags() const;
        uint32_t entryAbi() const;
        // Keeps the generation's module lease alive across this typed call.
        bool callI32NoArgs(int32_t& result) const;

    private:
        friend class MoonRuntime;
        friend class PinnedGeneration;
        friend class SwitchableBinding;
        std::shared_ptr<const GenerationState> generation_;
        const GenerationBinding* binding_ = nullptr;
    };

    class PinnedGeneration {
    public:
        explicit operator bool() const { return generation_ != nullptr; }
        uint64_t generationId() const;
        const std::string& moduleId() const;
        const std::string& contentDigest() const;
        const std::string& materializationKey() const;
        // Exact retained generation object, including Runtime provenance;
        // generation IDs alone are scoped to one Runtime.
        bool sameGeneration(const PinnedGeneration& other) const {
            return generation_ && generation_ == other.generation_;
        }
        // Requires both the retained address and its shared ownership to
        // match. Trusted adapters can bind decoded facts to this exact lease.
        bool matchesModuleLease(const std::shared_ptr<const void>& lease) const;
        PinnedBinding find(const std::string& symbolId,
                           const std::string& contractId) const;
        PinnedBinding find(
            const GenerationBindingRequirement& requirement) const;
        PinnedRefUnitCandidate findRefUnitCandidate(
            const std::string& symbolId, const std::string& contractId) const;
        std::vector<PinnedBinding> findAll(
            uint32_t declarationKind, uint32_t requiredFlags) const;

    private:
        friend class MoonRuntime;
        std::shared_ptr<const GenerationState> generation_;
    };

    class SwitchableBinding {
    public:
        explicit operator bool() const { return module_ != nullptr; }
        PinnedBinding pin() const;
        const std::string& symbolId() const { return symbolId_; }
        const std::string& contractId() const { return contractId_; }

    private:
        friend class MoonRuntime;
        std::shared_ptr<ModuleState> module_;
        std::string symbolId_;
        std::string contractId_;
        uint32_t declarationKind_ = 0;
        uint32_t requiredFlags_ = 0;
        uint32_t entryAbi_ = GenerationEntryAbiUnprofiled;
    };

    // A host attestation that the process is at an application-defined point
    // where one generation publication transition is permitted. A transition
    // may publish one module or a paired set. Tokens are single-use and belong
    // to the runtime that created them.
    SafePoint safePoint() const { return SafePoint(this); }

    bool stage(const GenerationStagingRequest& request,
               const GenerationVerifier& verifier,
               const GenerationResolver& resolver,
               const GenerationInitializer& initializer,
               StagedGeneration& staged, std::string& error);
    bool activate(StagedGeneration& staged, SafePoint& safePoint,
                  std::string& error);
    // Publishes two staged modules at one safe point after validating the
    // complete resulting module set. Readers needing a coherent pair must use
    // pinTogether(); individual pin()/switchable reads remain per-module.
    bool activateTogether(StagedGeneration& first,
                          StagedGeneration& second,
                          SafePoint& safePoint, std::string& error);
    // Publishes the first generation for a module. Repeating the same content,
    // materialization key and module requirements returns the active generation;
    // different content/configuration is rejected and must use activate().
    bool loadOnce(StagedGeneration& staged, PinnedGeneration& loaded,
                  std::string& error);
    bool rollback(const std::string& moduleId, uint64_t generationId,
                  SafePoint& safePoint, std::string& error);

    // Atomically publishes a host-selected immutable Fragment binding set.
    // Selection and metadata policy happen before this boundary; activation
    // requires the same single-use safe-point protocol as generation changes.
    bool activateFragmentBindings(
        const RuntimeFragmentBindingSet& bindings,
        SafePoint& safePoint, std::string& error);
    RuntimeFragmentBindingSet pinFragmentBindings() const;

    PinnedGeneration pin(const std::string& moduleId) const;
    std::pair<PinnedGeneration, PinnedGeneration> pinTogether(
        const std::string& firstModuleId,
        const std::string& secondModuleId) const;
    // Read-only attestation of an inert owner's full staged binding set.
    // The rows must come from independently verified artifact evidence. It
    // checks Runtime and exact lease ownership/address, and excludes candidate
    // callbacks and module relations without exposing an unactivated pin.
    bool matchesStagedOwnerSnapshot(
        const StagedGeneration& staged,
        const std::shared_ptr<const void>& lease,
        const std::vector<GenerationBinding>& bindings) const;
    // Also requires the pinned owner to be the currently active generation
    // of this Runtime. A retained pin from an older publication does not pass.
    bool matchesActiveOwnerSnapshot(
        const PinnedGeneration& active,
        const std::shared_ptr<const void>& lease,
        const std::vector<GenerationBinding>& bindings) const;
    // Read-only attestation of the complete active generation. The caller
    // supplies independently verified rows and must authenticate an opaque
    // Ref/unit callback through callerMatches when one is expected.
    bool matchesActiveGenerationSnapshot(
        const PinnedGeneration& active,
        const GenerationStagingRequest& expected,
        const std::vector<GenerationBinding>& bindings,
        const std::function<bool(const GenerationRefUnitCaller&)>&
            callerMatches = {}) const;
    // Read-only structural attestation for one staged typed Ref/unit candidate.
    // The expected request and Slot row must come from a trusted loader; this
    // cannot authenticate the behavior of an opaque std::function callback.
    bool matchesStagedRefUnitCandidateSnapshot(
        const StagedGeneration& staged,
        const GenerationStagingRequest& expected,
        const GenerationBinding& slotBinding) const;
    bool makeSwitchable(const std::string& moduleId,
                        const std::string& symbolId,
                        const std::string& contractId,
                        SwitchableBinding& binding, std::string& error);
    bool makeSwitchable(const std::string& moduleId,
                        const GenerationBindingRequirement& requirement,
                        SwitchableBinding& binding, std::string& error);
    uint64_t activeGenerationId(const std::string& moduleId) const;
    size_t retainedGenerationCount(const std::string& moduleId) const;

private:
    static bool matchesOwnerSnapshot(
        const GenerationState& generation,
        const std::shared_ptr<const void>& lease,
        const std::vector<GenerationBinding>& bindings);
    static const GenerationBinding* findBinding(
        const GenerationState& generation, const std::string& symbolId,
        const std::string& contractId);
    static PinnedBinding makePinnedBinding(
        std::shared_ptr<const GenerationState> generation,
        const std::string& symbolId, const std::string& contractId);
    static bool validatesRequirements(
        const ModuleState& module, const GenerationState& generation,
        std::string& error);
    // Caller holds mutex_. Checks the candidate and all currently active
    // generations so publication cannot invalidate a module relation.
    bool validatesModuleRelations(
        const GenerationState& generation, std::string& error) const;
    bool validatesPairedModuleRelations(
        const std::shared_ptr<const GenerationState>& first,
        const std::shared_ptr<const GenerationState>& second,
        std::string& error) const;
    bool consumeSafePoint(SafePoint& safePoint, std::string& error) const;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<ModuleState>> modules_;
#if defined(__cpp_lib_atomic_shared_ptr) && \
    __cpp_lib_atomic_shared_ptr >= 201711L
    std::atomic<std::shared_ptr<const RuntimeFragmentBindingSetState>>
        activeFragmentBindings_;
#else
    std::shared_ptr<const RuntimeFragmentBindingSetState>
        activeFragmentBindings_;
#endif
    uint64_t nextGenerationId_ = 1;
};

} // namespace luna::runtime
