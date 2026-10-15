#include "MoonRuntime.h"

#include "RuntimeDescriptorABI.h"
#include "NativeArtifactABI.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <exception>
#include <utility>

namespace luna::runtime {

static_assert(
    static_cast<uint32_t>(GenerationBindingPublicControl) ==
        LUNA_RUNTIME_DESCRIPTOR_PUBLIC_CONTROL_V1,
    "generation and descriptor public-control flags must agree");
static_assert(
    static_cast<uint32_t>(GenerationBindingFragmentContext) ==
        LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1,
    "generation and descriptor fragment-context flags must agree");

struct MoonRuntime::GenerationState {
    uint64_t generationId = 0;
    std::string moduleId;
    std::string contentDigest;
    std::string materializationKey;
    std::shared_ptr<const void> moduleLease;
    std::vector<GenerationBinding> bindings;
    std::vector<GenerationRefUnitCandidate> refUnitCandidates;
    GenerationRefUnitCaller refUnitCaller;
    std::shared_ptr<const void> refUnitCallerLease;
    std::vector<std::string> requiredAbsentModuleIds;
    std::vector<GenerationActiveModuleRequirement> requiredActiveModules;
};

struct MoonRuntime::ModuleState {
    struct ActiveGeneration {
        std::shared_ptr<const GenerationState> load() const noexcept {
#if defined(__cpp_lib_atomic_shared_ptr) && \
    __cpp_lib_atomic_shared_ptr >= 201711L
            return value.load(std::memory_order_acquire);
#else
            return std::atomic_load_explicit(
                &value, std::memory_order_acquire);
#endif
        }

        void store(
            std::shared_ptr<const GenerationState> generation) noexcept {
#if defined(__cpp_lib_atomic_shared_ptr) && \
    __cpp_lib_atomic_shared_ptr >= 201711L
            value.store(std::move(generation), std::memory_order_release);
#else
            std::atomic_store_explicit(
                &value, std::move(generation), std::memory_order_release);
#endif
        }

    private:
#if defined(__cpp_lib_atomic_shared_ptr) && \
    __cpp_lib_atomic_shared_ptr >= 201711L
        std::atomic<std::shared_ptr<const GenerationState>> value;
#else
        std::shared_ptr<const GenerationState> value;
#endif
    } active;
    std::vector<std::shared_ptr<const GenerationState>> history;
    std::vector<GenerationBindingRequirement> switchableRequirements;
};

namespace {

const std::string& emptyString() {
    static const std::string empty;
    return empty;
}

bool validIdentity(const std::string& value) {
    // IDs must survive conversion to null-terminated C ABI names unchanged.
    return !value.empty() &&
        value.find_first_of("\r\n\t\0", 0, 4) == std::string::npos;
}

bool validDigest(const std::string& digest) {
    return digest.size() == 64 && std::all_of(
        digest.begin(), digest.end(), [](unsigned char character) {
            return std::isdigit(character) != 0 ||
                (character >= 'a' && character <= 'f');
        });
}

} // namespace

MoonRuntime::SafePoint::SafePoint(SafePoint&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      consumed_(std::exchange(other.consumed_, true)) {}

MoonRuntime::SafePoint& MoonRuntime::SafePoint::operator=(
    SafePoint&& other) noexcept {
    if (this == &other) return *this;
    owner_ = std::exchange(other.owner_, nullptr);
    consumed_ = std::exchange(other.consumed_, true);
    return *this;
}

uint64_t MoonRuntime::StagedGeneration::generationId() const {
    return generation_ ? generation_->generationId : 0;
}

const std::string& MoonRuntime::StagedGeneration::moduleId() const {
    return generation_ ? generation_->moduleId : emptyString();
}

const std::string& MoonRuntime::StagedGeneration::contentDigest() const {
    return generation_ ? generation_->contentDigest : emptyString();
}

const std::string& MoonRuntime::StagedGeneration::materializationKey() const {
    return generation_ ? generation_->materializationKey : emptyString();
}

bool MoonRuntime::StagedGeneration::hasRefUnitCaller() const {
    return generation_ && static_cast<bool>(generation_->refUnitCaller);
}

uint64_t MoonRuntime::PinnedBinding::generationId() const {
    return generation_ ? generation_->generationId : 0;
}

uint64_t MoonRuntime::PinnedRefUnitCandidate::generationId() const {
    return generation_ ? generation_->generationId : 0;
}

std::optional<GenerationRefUnitCandidate>
MoonRuntime::PinnedRefUnitCandidate::facts() const {
    if (!generation_ || !candidate_) return std::nullopt;
    return *candidate_;
}

bool MoonRuntime::PinnedRefUnitCandidate::call(
    const RuntimeFragmentExecutionContext& parent,
    const RuntimeFragmentRefHandle& reference,
    int32_t& status, std::string& error) const {
    const auto pinned = generation_;
    if (!pinned || !candidate_ || !pinned->refUnitCaller) {
        error = "pinned Ref/unit candidate has no typed call adapter";
        return false;
    }
    return pinned->refUnitCaller(
        candidate_->symbolId, candidate_->contractId,
        parent, reference, status, error);
}

const std::string& MoonRuntime::PinnedBinding::symbolId() const {
    return binding_ ? binding_->symbolId : emptyString();
}

const std::string& MoonRuntime::PinnedBinding::contractId() const {
    return binding_ ? binding_->contractId : emptyString();
}

const void* MoonRuntime::PinnedBinding::implementation() const {
    return binding_ ? binding_->implementation : nullptr;
}

uint32_t MoonRuntime::PinnedBinding::declarationKind() const {
    return binding_ ? binding_->declarationKind : 0;
}

uint32_t MoonRuntime::PinnedBinding::flags() const {
    return binding_ ? binding_->flags : 0;
}

uint32_t MoonRuntime::PinnedBinding::entryAbi() const {
    return binding_ ? binding_->entryAbi : GenerationEntryAbiUnprofiled;
}

bool MoonRuntime::PinnedBinding::callI32NoArgs(int32_t& result) const {
    if (!generation_ || !binding_ ||
        binding_->entryAbi != GenerationEntryAbiCI32NoArgsV1 ||
        binding_->declarationKind != LUNA_RUNTIME_DECLARATION_FUNCTION_V1 ||
        (binding_->flags & GenerationBindingCallable) == 0 ||
        !binding_->implementation)
        return false;
    using Entry = int32_t (*)();
    static_assert(sizeof(Entry) == sizeof(binding_->implementation),
                  "host cannot represent a generation function pointer");
    Entry function = nullptr;
    std::memcpy(&function, &binding_->implementation, sizeof(function));
    result = function();
    return true;
}

uint64_t MoonRuntime::PinnedGeneration::generationId() const {
    return generation_ ? generation_->generationId : 0;
}

const std::string& MoonRuntime::PinnedGeneration::moduleId() const {
    return generation_ ? generation_->moduleId : emptyString();
}

const std::string& MoonRuntime::PinnedGeneration::contentDigest() const {
    return generation_ ? generation_->contentDigest : emptyString();
}

const std::string& MoonRuntime::PinnedGeneration::materializationKey() const {
    return generation_ ? generation_->materializationKey : emptyString();
}

bool MoonRuntime::PinnedGeneration::matchesModuleLease(
    const std::shared_ptr<const void>& lease) const {
    if (!generation_ || !lease) return false;
    const auto& retained = generation_->moduleLease;
    return retained.get() == lease.get() &&
        !retained.owner_before(lease) && !lease.owner_before(retained);
}

const GenerationBinding* MoonRuntime::findBinding(
    const GenerationState& generation, const std::string& symbolId,
    const std::string& contractId) {
    const auto found = std::lower_bound(
        generation.bindings.begin(), generation.bindings.end(), symbolId,
        [](const GenerationBinding& binding, const std::string& identity) {
            return binding.symbolId < identity;
        });
    if (found == generation.bindings.end() || found->symbolId != symbolId ||
        found->contractId != contractId)
        return nullptr;
    return &*found;
}

MoonRuntime::PinnedBinding MoonRuntime::PinnedGeneration::find(
    const std::string& symbolId, const std::string& contractId) const {
    return MoonRuntime::makePinnedBinding(
        generation_, symbolId, contractId);
}

MoonRuntime::PinnedBinding MoonRuntime::PinnedGeneration::find(
    const GenerationBindingRequirement& requirement) const {
    auto binding = MoonRuntime::makePinnedBinding(
        generation_, requirement.symbolId, requirement.contractId);
    if (!binding || binding.declarationKind() != requirement.declarationKind ||
        (binding.flags() & requirement.requiredFlags) !=
            requirement.requiredFlags ||
        (requirement.entryAbi != GenerationEntryAbiAny &&
         binding.entryAbi() != requirement.entryAbi))
        return {};
    return binding;
}

MoonRuntime::PinnedRefUnitCandidate
MoonRuntime::PinnedGeneration::findRefUnitCandidate(
    const std::string& symbolId, const std::string& contractId) const {
    PinnedRefUnitCandidate result;
    if (!generation_) return result;
    const auto& candidates = generation_->refUnitCandidates;
    const auto found = std::lower_bound(
        candidates.begin(), candidates.end(), symbolId,
        [](const GenerationRefUnitCandidate& candidate,
           const std::string& identity) {
            return candidate.symbolId < identity;
        });
    if (found == candidates.end() || found->symbolId != symbolId ||
        found->contractId != contractId)
        return result;
    result.candidate_ = &*found;
    result.generation_ = generation_;
    return result;
}

std::vector<MoonRuntime::PinnedBinding>
MoonRuntime::PinnedGeneration::findAll(
    uint32_t declarationKind, uint32_t requiredFlags) const {
    std::vector<PinnedBinding> result;
    if (!generation_ || declarationKind == 0) return result;
    for (const auto& binding : generation_->bindings) {
        if (binding.declarationKind != declarationKind ||
            (binding.flags & requiredFlags) != requiredFlags)
            continue;
        PinnedBinding pinned;
        pinned.generation_ = generation_;
        pinned.binding_ = &binding;
        result.push_back(std::move(pinned));
    }
    return result;
}

MoonRuntime::PinnedBinding MoonRuntime::SwitchableBinding::pin() const {
    if (!module_) return {};
    auto generation = module_->active.load();
    auto binding = MoonRuntime::makePinnedBinding(
        std::move(generation), symbolId_, contractId_);
    if (!binding || binding.declarationKind() != declarationKind_ ||
        (binding.flags() & requiredFlags_) != requiredFlags_ ||
        binding.entryAbi() != entryAbi_)
        return {};
    return binding;
}

MoonRuntime::PinnedBinding MoonRuntime::makePinnedBinding(
    std::shared_ptr<const GenerationState> generation,
    const std::string& symbolId, const std::string& contractId) {
    PinnedBinding result;
    if (!generation) return result;
    result.binding_ = findBinding(*generation, symbolId, contractId);
    if (result.binding_) result.generation_ = std::move(generation);
    return result;
}

bool MoonRuntime::stage(
    const GenerationStagingRequest& request,
    const GenerationVerifier& verifier,
    const GenerationResolver& resolver,
    const GenerationInitializer& initializer,
    StagedGeneration& staged, std::string& error) {
    error.clear();
    if (staged.generation_) {
        error = "staging output already owns a generation";
        return false;
    }
    if (!validIdentity(request.moduleId) ||
        !validDigest(request.contentDigest) || !request.moduleLease ||
        (!request.materializationKey.empty() &&
         !validIdentity(request.materializationKey))) {
        error = "generation staging identity or module lease is invalid";
        return false;
    }
    if (!verifier || !resolver) {
        error = "generation staging requires verification and resolution";
        return false;
    }
    if (request.refUnitCaller && request.refUnitCandidates.empty()) {
        error = "generation Ref/unit call adapter has no candidate";
        return false;
    }
    if (static_cast<bool>(request.refUnitCaller) !=
            static_cast<bool>(request.refUnitCallerLease) ||
        (request.refUnitCallerLease &&
         (request.refUnitCallerLease.get() != request.moduleLease.get() ||
          request.refUnitCallerLease.owner_before(request.moduleLease) ||
          request.moduleLease.owner_before(request.refUnitCallerLease)))) {
        error = "generation Ref/unit call adapter has no exact module lease";
        return false;
    }
    auto requiredAbsentModuleIds = request.requiredAbsentModuleIds;
    std::sort(requiredAbsentModuleIds.begin(), requiredAbsentModuleIds.end());
    if (std::any_of(requiredAbsentModuleIds.begin(),
                    requiredAbsentModuleIds.end(),
                    [&](const std::string& moduleId) {
                        return !validIdentity(moduleId) ||
                            moduleId == request.moduleId;
                    }) ||
        std::adjacent_find(requiredAbsentModuleIds.begin(),
                           requiredAbsentModuleIds.end()) !=
            requiredAbsentModuleIds.end()) {
        error = "generation required-absent module list is invalid";
        return false;
    }
    auto requiredActiveModules = request.requiredActiveModules;
    std::sort(requiredActiveModules.begin(), requiredActiveModules.end(),
              [](const auto& left, const auto& right) {
                  return left.moduleId < right.moduleId;
              });
    if (std::any_of(requiredActiveModules.begin(),
                    requiredActiveModules.end(),
                    [&](const auto& requirement) {
                        return !validIdentity(requirement.moduleId) ||
                            requirement.moduleId == request.moduleId ||
                            requirement.generationId == 0 ||
                            std::binary_search(requiredAbsentModuleIds.begin(),
                                               requiredAbsentModuleIds.end(),
                                               requirement.moduleId);
                    }) ||
        std::adjacent_find(requiredActiveModules.begin(),
                           requiredActiveModules.end(),
                           [](const auto& left, const auto& right) {
                               return left.moduleId == right.moduleId;
                           }) != requiredActiveModules.end()) {
        error = "generation required-active module list is invalid";
        return false;
    }
    std::vector<GenerationBinding> bindings;
    try {
        if (!verifier(request, error)) {
            if (error.empty()) error = "generation verification failed";
            return false;
        }
        if (!resolver(request, bindings, error)) {
            if (error.empty()) error = "generation resolution failed";
            return false;
        }
    } catch (const std::exception& exception) {
        error = "generation verification or resolution threw: " +
            std::string(exception.what());
        return false;
    } catch (...) {
        error = "generation verification or resolution threw";
        return false;
    }
    std::sort(bindings.begin(), bindings.end(),
              [](const GenerationBinding& left,
                 const GenerationBinding& right) {
                  return left.symbolId < right.symbolId;
              });
    for (size_t index = 0; index < bindings.size(); ++index) {
        const auto& binding = bindings[index];
        const uint32_t knownFlags =
            GenerationBindingCallable |
            GenerationBindingFragmentExecutable |
            GenerationBindingPublicControl |
            GenerationBindingFragmentContext;
        const bool callable =
            (binding.flags & GenerationBindingCallable) != 0;
        const bool fragmentExecutable =
            (binding.flags & GenerationBindingFragmentExecutable) != 0;
        const bool publicControl =
            (binding.flags & GenerationBindingPublicControl) != 0;
        const bool fragmentContext =
            (binding.flags & GenerationBindingFragmentContext) != 0;
        const bool profiled =
            binding.entryAbi != GenerationEntryAbiUnprofiled;
        if (!validIdentity(binding.symbolId) ||
            !validIdentity(binding.contractId) || !binding.implementation ||
            (binding.flags & ~knownFlags) != 0 ||
            (callable && fragmentExecutable) ||
            (callable && binding.declarationKind !=
                LUNA_RUNTIME_DECLARATION_FUNCTION_V1) ||
            (fragmentExecutable && binding.declarationKind !=
                LUNA_RUNTIME_DECLARATION_FRAGMENT_V1) ||
            (fragmentContext &&
             (!callable || binding.declarationKind !=
                 LUNA_RUNTIME_DECLARATION_FUNCTION_V1)) ||
            (profiled &&
             (binding.entryAbi != GenerationEntryAbiCI32NoArgsV1 ||
              !callable || fragmentContext)) ||
            (publicControl &&
             binding.declarationKind != LUNA_RUNTIME_DECLARATION_FRAGMENT_V1 &&
             binding.declarationKind != LUNA_RUNTIME_DECLARATION_SLOT_V1) ||
            (index != 0 && bindings[index - 1].symbolId == binding.symbolId)) {
            error = "generation resolution produced an invalid or duplicate binding";
            return false;
        }
    }
    auto refUnitCandidates = request.refUnitCandidates;
    std::sort(refUnitCandidates.begin(), refUnitCandidates.end(),
              [](const GenerationRefUnitCandidate& left,
                 const GenerationRefUnitCandidate& right) {
                  return left.symbolId < right.symbolId;
              });
    for (size_t index = 0; index < refUnitCandidates.size(); ++index) {
        const auto& candidate = refUnitCandidates[index];
        const auto slot = std::lower_bound(
            bindings.begin(), bindings.end(), candidate.slotSymbolId,
            [](const GenerationBinding& binding, const std::string& identity) {
                return binding.symbolId < identity;
            });
        const auto alias = std::lower_bound(
            bindings.begin(), bindings.end(), candidate.symbolId,
            [](const GenerationBinding& binding, const std::string& identity) {
                return binding.symbolId < identity;
            });
        if (!validIdentity(candidate.symbolId) ||
            !validIdentity(candidate.contractId) ||
            !validIdentity(candidate.linkageName) ||
            !validIdentity(candidate.moduleName) ||
            !validIdentity(candidate.packageId) ||
            !validIdentity(candidate.functionTypeId) ||
            !validIdentity(candidate.refTypeId) ||
            !validIdentity(candidate.slotSymbolId) ||
            !validIdentity(candidate.slotContractId) ||
            !validIdentity(candidate.slotTypeId) ||
            candidate.moduleName != request.moduleId ||
            candidate.packageId != request.moduleId ||
            candidate.entryAbi != GenerationEntryAbiRefUnitV1 ||
            candidate.refMode != LUNA_NATIVE_REF_SHARED_BORROW_V1 ||
            candidate.resultMode != LUNA_NATIVE_RESULT_UNIT_V1 ||
            candidate.statusDomain != LUNA_NATIVE_STATUS_DOMAIN_REF_UNIT_V1 ||
            slot == bindings.end() ||
            slot->symbolId != candidate.slotSymbolId ||
            slot->contractId != candidate.slotContractId ||
            slot->declarationKind != LUNA_RUNTIME_DECLARATION_SLOT_V1 ||
            slot->flags != 0 ||
            (alias != bindings.end() &&
             alias->symbolId == candidate.symbolId) ||
            (index != 0 && refUnitCandidates[index - 1].symbolId ==
                               candidate.symbolId)) {
            error = "generation resolution produced an invalid Ref/unit candidate";
            return false;
        }
    }
    try {
        if (initializer && !initializer(request, bindings, error)) {
            if (error.empty()) error = "generation initializer failed";
            return false;
        }
    } catch (const std::exception& exception) {
        error = "generation initializer threw: " +
            std::string(exception.what());
        return false;
    } catch (...) {
        error = "generation initializer threw";
        return false;
    }

    auto generation = std::make_shared<GenerationState>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (nextGenerationId_ == 0) {
            error = "generation identity space is exhausted";
            return false;
        }
        generation->generationId = nextGenerationId_++;
    }
    generation->moduleId = request.moduleId;
    generation->contentDigest = request.contentDigest;
    generation->materializationKey = request.materializationKey;
    generation->moduleLease = request.moduleLease;
    generation->bindings = std::move(bindings);
    generation->refUnitCandidates = std::move(refUnitCandidates);
    generation->refUnitCaller = request.refUnitCaller;
    generation->refUnitCallerLease = request.refUnitCallerLease;
    generation->requiredAbsentModuleIds =
        std::move(requiredAbsentModuleIds);
    generation->requiredActiveModules = std::move(requiredActiveModules);
    staged.owner_ = this;
    staged.generation_ = std::move(generation);
    return true;
}

bool MoonRuntime::consumeSafePoint(
    SafePoint& safePoint, std::string& error) const {
    if (safePoint.owner_ != this || safePoint.consumed_) {
        error = "activation requires a fresh safe point from the same runtime";
        return false;
    }
    safePoint.consumed_ = true;
    return true;
}

bool MoonRuntime::validatesRequirements(
    const ModuleState& module, const GenerationState& generation,
    std::string& error) {
    for (const auto& requirement : module.switchableRequirements) {
        const auto* binding = findBinding(
            generation, requirement.symbolId, requirement.contractId);
        if (!binding ||
            binding->declarationKind != requirement.declarationKind ||
            (binding->flags & requirement.requiredFlags) !=
                requirement.requiredFlags ||
            binding->entryAbi != requirement.entryAbi) {
            error = "generation does not satisfy an existing switchable binding";
            return false;
        }
    }
    return true;
}

bool MoonRuntime::validatesModuleRelations(
    const GenerationState& generation, std::string& error) const {
    for (const auto& requirement : generation.requiredActiveModules) {
        const auto found = modules_.find(requirement.moduleId);
        const auto active = found == modules_.end()
            ? nullptr : found->second->active.load();
        if (!active || active->generationId != requirement.generationId) {
            error = "generation requires a different active module generation";
            return false;
        }
    }
    for (const auto& [moduleId, module] : modules_) {
        if (moduleId == generation.moduleId) continue;
        const auto active = module->active.load();
        if (!active) continue;
        if (std::binary_search(generation.requiredAbsentModuleIds.begin(),
                               generation.requiredAbsentModuleIds.end(),
                               moduleId) ||
            std::binary_search(active->requiredAbsentModuleIds.begin(),
                               active->requiredAbsentModuleIds.end(),
                               generation.moduleId)) {
            error = "generation conflicts with an active module";
            return false;
        }
        const auto requirement = std::lower_bound(
            active->requiredActiveModules.begin(),
            active->requiredActiveModules.end(), generation.moduleId,
            [](const auto& candidate, const std::string& moduleId) {
                return candidate.moduleId < moduleId;
            });
        if (requirement != active->requiredActiveModules.end() &&
            requirement->moduleId == generation.moduleId &&
            requirement->generationId != generation.generationId) {
            error = "generation replacement breaks an active module dependency";
            return false;
        }
    }
    return true;
}

bool MoonRuntime::validatesPairedModuleRelations(
    const std::shared_ptr<const GenerationState>& first,
    const std::shared_ptr<const GenerationState>& second,
    std::string& error) const {
    const auto finalActive = [&](const std::string& moduleId)
        -> std::shared_ptr<const GenerationState> {
        if (moduleId == first->moduleId) return first;
        if (moduleId == second->moduleId) return second;
        const auto found = modules_.find(moduleId);
        return found == modules_.end() ? nullptr : found->second->active.load();
    };
    const auto validates = [&](const GenerationState& generation) {
        for (const auto& requirement : generation.requiredActiveModules) {
            const auto active = finalActive(requirement.moduleId);
            if (!active || active->generationId != requirement.generationId) {
                error = "generation requires a different active module generation";
                return false;
            }
        }
        for (const auto& moduleId : generation.requiredAbsentModuleIds) {
            if (finalActive(moduleId)) {
                error = "generation conflicts with an active module";
                return false;
            }
        }
        return true;
    };
    if (!validates(*first) || !validates(*second)) return false;
    for (const auto& [moduleId, module] : modules_) {
        if (moduleId == first->moduleId || moduleId == second->moduleId)
            continue;
        const auto active = module->active.load();
        if (active && !validates(*active)) return false;
    }
    return true;
}

bool MoonRuntime::loadOnce(
    StagedGeneration& staged, PinnedGeneration& loaded,
    std::string& error) {
    error.clear();
    if (loaded.generation_) {
        error = "load-once output already owns a generation";
        return false;
    }
    if (staged.owner_ != this || !staged.generation_) {
        error = "load-once requires a staged generation from the same runtime";
        return false;
    }
    if (staged.generation_->refUnitCaller) {
        error = "callable Ref/unit candidate requires explicit activation";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto& module = modules_[staged.generation_->moduleId];
    if (!module) module = std::make_shared<ModuleState>();
    const auto active = module->active.load();
    if (active) {
        if (!active->refUnitCandidates.empty() ||
            !staged.generation_->refUnitCandidates.empty() ||
            active->refUnitCaller) {
            error = "Ref/unit candidate cannot use the load-once cache";
            return false;
        }
        if (active->contentDigest != staged.generation_->contentDigest) {
            error = "module is already loaded with different content";
            return false;
        }
        if (active->materializationKey != staged.generation_->materializationKey) {
            error = "module is already loaded with different materialization configuration";
            return false;
        }
        if (active->requiredAbsentModuleIds !=
            staged.generation_->requiredAbsentModuleIds) {
            error = "same-content generation changed module exclusions";
            return false;
        }
        if (active->requiredActiveModules !=
            staged.generation_->requiredActiveModules) {
            error = "same-content generation changed module dependencies";
            return false;
        }
        if (active->bindings.size() != staged.generation_->bindings.size()) {
            error = "same-content generation resolved a different binding set";
            return false;
        }
        for (size_t index = 0; index < active->bindings.size(); ++index) {
            const auto& current = active->bindings[index];
            const auto& candidate = staged.generation_->bindings[index];
            if (current.symbolId != candidate.symbolId ||
                current.contractId != candidate.contractId ||
                current.declarationKind != candidate.declarationKind ||
                current.flags != candidate.flags ||
                current.entryAbi != candidate.entryAbi) {
                error = "same-content generation resolved a different binding set";
                return false;
            }
        }
        loaded.generation_ = active;
        staged.generation_.reset();
        staged.owner_ = nullptr;
        return true;
    }
    if (!module->history.empty()) {
        error = "load-once module has retained generations but no active generation";
        return false;
    }
    if (!validatesModuleRelations(*staged.generation_, error)) return false;
    module->history.push_back(staged.generation_);
    module->active.store(staged.generation_);
    loaded.generation_ = staged.generation_;
    staged.generation_.reset();
    staged.owner_ = nullptr;
    return true;
}

bool MoonRuntime::activate(
    StagedGeneration& staged, SafePoint& safePoint, std::string& error) {
    error.clear();
    if (staged.owner_ != this || !staged.generation_) {
        error = "activation requires a staged generation from the same runtime";
        return false;
    }
    if (!consumeSafePoint(safePoint, error)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& module = modules_[staged.generation_->moduleId];
    if (!module) module = std::make_shared<ModuleState>();
    if (!validatesRequirements(*module, *staged.generation_, error))
        return false;
    if (!validatesModuleRelations(*staged.generation_, error))
        return false;
    module->history.push_back(staged.generation_);
    module->active.store(staged.generation_);
    staged.generation_.reset();
    staged.owner_ = nullptr;
    return true;
}

bool MoonRuntime::activateTogether(
    StagedGeneration& first, StagedGeneration& second,
    SafePoint& safePoint, std::string& error) {
    error.clear();
    if (&first == &second || first.owner_ != this || second.owner_ != this ||
        !first.generation_ || !second.generation_ ||
        first.generation_->moduleId == second.generation_->moduleId) {
        error = "paired activation requires distinct staged modules from the same runtime";
        return false;
    }
    if (!consumeSafePoint(safePoint, error)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto firstFound = modules_.find(first.generation_->moduleId);
    const auto secondFound = modules_.find(second.generation_->moduleId);
    const bool firstMissing = firstFound == modules_.end();
    const bool secondMissing = secondFound == modules_.end();
    auto firstModule = firstMissing
        ? std::make_shared<ModuleState>() : firstFound->second;
    auto secondModule = secondMissing
        ? std::make_shared<ModuleState>() : secondFound->second;
    if (!validatesRequirements(*firstModule, *first.generation_, error) ||
        !validatesRequirements(*secondModule, *second.generation_, error) ||
        !validatesPairedModuleRelations(
            first.generation_, second.generation_, error))
        return false;
    firstModule->history.reserve(firstModule->history.size() + 1);
    secondModule->history.reserve(secondModule->history.size() + 1);
    if (firstMissing)
        modules_.emplace(first.generation_->moduleId, firstModule);
    if (secondMissing)
        modules_.emplace(second.generation_->moduleId, secondModule);
    firstModule->history.push_back(first.generation_);
    secondModule->history.push_back(second.generation_);
    firstModule->active.store(first.generation_);
    secondModule->active.store(second.generation_);
    first.generation_.reset();
    second.generation_.reset();
    first.owner_ = nullptr;
    second.owner_ = nullptr;
    return true;
}

bool MoonRuntime::rollback(
    const std::string& moduleId, uint64_t generationId,
    SafePoint& safePoint, std::string& error) {
    error.clear();
    if (!consumeSafePoint(safePoint, error)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto moduleIt = modules_.find(moduleId);
    if (moduleIt == modules_.end()) {
        error = "rollback module has no retained generation";
        return false;
    }
    auto& module = *moduleIt->second;
    const auto generation = std::find_if(
        module.history.begin(), module.history.end(),
        [generationId](const auto& candidate) {
            return candidate->generationId == generationId;
        });
    if (generation == module.history.end()) {
        error = "rollback generation is not retained by this module";
        return false;
    }
    if (!validatesRequirements(module, **generation, error)) return false;
    if (!validatesModuleRelations(**generation, error)) return false;
    module.active.store(*generation);
    return true;
}

MoonRuntime::PinnedGeneration MoonRuntime::pin(
    const std::string& moduleId) const {
    PinnedGeneration result;
    std::shared_ptr<ModuleState> module;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = modules_.find(moduleId);
        if (found == modules_.end()) return result;
        module = found->second;
    }
    result.generation_ = module->active.load();
    return result;
}

std::pair<MoonRuntime::PinnedGeneration, MoonRuntime::PinnedGeneration>
MoonRuntime::pinTogether(
    const std::string& firstModuleId,
    const std::string& secondModuleId) const {
    std::pair<PinnedGeneration, PinnedGeneration> result;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto first = modules_.find(firstModuleId);
    const auto second = modules_.find(secondModuleId);
    if (first != modules_.end())
        result.first.generation_ = first->second->active.load();
    if (second != modules_.end())
        result.second.generation_ = second->second->active.load();
    return result;
}

bool MoonRuntime::matchesStagedGeneration(
    const StagedGeneration& staged,
    const std::shared_ptr<const void>& lease,
    const GenerationBinding& binding) const {
    if (staged.owner_ != this || !staged.generation_ || !lease)
        return false;
    const auto& retained = staged.generation_->moduleLease;
    if (retained.get() != lease.get() ||
        retained.owner_before(lease) || lease.owner_before(retained))
        return false;
    const auto* actual = findBinding(
        *staged.generation_, binding.symbolId, binding.contractId);
    return actual && actual->implementation == binding.implementation &&
        actual->declarationKind == binding.declarationKind &&
        actual->flags == binding.flags &&
        actual->entryAbi == binding.entryAbi;
}

bool MoonRuntime::makeSwitchable(
    const std::string& moduleId, const std::string& symbolId,
    const std::string& contractId, SwitchableBinding& binding,
    std::string& error) {
    GenerationBindingRequirement requirement;
    requirement.symbolId = symbolId;
    requirement.contractId = contractId;
    return makeSwitchable(moduleId, requirement, binding, error);
}

bool MoonRuntime::makeSwitchable(
    const std::string& moduleId,
    const GenerationBindingRequirement& requested,
    SwitchableBinding& binding, std::string& error) {
    error.clear();
    if (binding.module_) {
        error = "switchable binding output is already initialized";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto moduleIt = modules_.find(moduleId);
    if (moduleIt == modules_.end()) {
        error = "switchable binding module has no active generation";
        return false;
    }
    const auto active = moduleIt->second->active.load();
    const auto* activeBinding = active
        ? findBinding(*active, requested.symbolId, requested.contractId)
        : nullptr;
    if (!activeBinding ||
        (requested.declarationKind != 0 &&
         activeBinding->declarationKind != requested.declarationKind) ||
        (activeBinding->flags & requested.requiredFlags) !=
            requested.requiredFlags ||
        (requested.entryAbi != GenerationEntryAbiAny &&
         activeBinding->entryAbi != requested.entryAbi)) {
        error = "switchable binding identity is absent from the active generation";
        return false;
    }
    GenerationBindingRequirement requirement = requested;
    requirement.declarationKind = activeBinding->declarationKind;
    requirement.requiredFlags = activeBinding->flags;
    requirement.entryAbi = activeBinding->entryAbi;
    auto& requirements = moduleIt->second->switchableRequirements;
    const auto duplicate = std::find_if(
        requirements.begin(), requirements.end(),
        [&](const GenerationBindingRequirement& existing) {
            return existing.symbolId == requirement.symbolId &&
                existing.contractId == requirement.contractId &&
                existing.declarationKind == requirement.declarationKind &&
                existing.requiredFlags == requirement.requiredFlags &&
                existing.entryAbi == requirement.entryAbi;
        });
    if (duplicate == requirements.end())
        requirements.push_back(requirement);
    binding.module_ = moduleIt->second;
    binding.symbolId_ = requirement.symbolId;
    binding.contractId_ = requirement.contractId;
    binding.declarationKind_ = requirement.declarationKind;
    binding.requiredFlags_ = requirement.requiredFlags;
    binding.entryAbi_ = requirement.entryAbi;
    return true;
}

uint64_t MoonRuntime::activeGenerationId(
    const std::string& moduleId) const {
    return pin(moduleId).generationId();
}

size_t MoonRuntime::retainedGenerationCount(
    const std::string& moduleId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = modules_.find(moduleId);
    return found == modules_.end() ? 0 : found->second->history.size();
}

} // namespace luna::runtime
