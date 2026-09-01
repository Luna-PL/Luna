#include "MoonRuntime.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <exception>
#include <utility>

namespace luna::runtime {

struct MoonRuntime::GenerationState {
    uint64_t generationId = 0;
    std::string moduleId;
    std::string contentDigest;
    std::shared_ptr<const void> moduleLease;
    std::vector<GenerationBinding> bindings;
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
    return !value.empty() &&
        value.find_first_of("\r\n\t") == std::string::npos;
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

uint64_t MoonRuntime::PinnedBinding::generationId() const {
    return generation_ ? generation_->generationId : 0;
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

uint64_t MoonRuntime::PinnedGeneration::generationId() const {
    return generation_ ? generation_->generationId : 0;
}

const std::string& MoonRuntime::PinnedGeneration::moduleId() const {
    return generation_ ? generation_->moduleId : emptyString();
}

const std::string& MoonRuntime::PinnedGeneration::contentDigest() const {
    return generation_ ? generation_->contentDigest : emptyString();
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
            requirement.requiredFlags)
        return {};
    return binding;
}

MoonRuntime::PinnedBinding MoonRuntime::SwitchableBinding::pin() const {
    if (!module_) return {};
    auto generation = module_->active.load();
    return MoonRuntime::makePinnedBinding(
        std::move(generation), symbolId_, contractId_);
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
        !validDigest(request.contentDigest) || !request.moduleLease) {
        error = "generation staging identity or module lease is invalid";
        return false;
    }
    if (!verifier || !resolver) {
        error = "generation staging requires verification and resolution";
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
        if (!validIdentity(binding.symbolId) ||
            !validIdentity(binding.contractId) || !binding.implementation ||
            (index != 0 && bindings[index - 1].symbolId == binding.symbolId)) {
            error = "generation resolution produced an invalid or duplicate binding";
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
    generation->moduleLease = request.moduleLease;
    generation->bindings = std::move(bindings);
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
                requirement.requiredFlags) {
            error = "generation does not satisfy an existing switchable binding";
            return false;
        }
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

    std::lock_guard<std::mutex> lock(mutex_);
    auto& module = modules_[staged.generation_->moduleId];
    if (!module) module = std::make_shared<ModuleState>();
    const auto active = module->active.load();
    if (active) {
        if (active->contentDigest != staged.generation_->contentDigest) {
            error = "module is already loaded with different content";
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
                current.flags != candidate.flags) {
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
    module->history.push_back(staged.generation_);
    module->active.store(staged.generation_);
    staged.generation_.reset();
    staged.owner_ = nullptr;
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
            requested.requiredFlags) {
        error = "switchable binding identity is absent from the active generation";
        return false;
    }
    GenerationBindingRequirement requirement = requested;
    requirement.declarationKind = activeBinding->declarationKind;
    requirement.requiredFlags = activeBinding->flags;
    auto& requirements = moduleIt->second->switchableRequirements;
    const auto duplicate = std::find_if(
        requirements.begin(), requirements.end(),
        [&](const GenerationBindingRequirement& existing) {
            return existing.symbolId == requirement.symbolId &&
                existing.contractId == requirement.contractId &&
                existing.declarationKind == requirement.declarationKind &&
                existing.requiredFlags == requirement.requiredFlags;
        });
    if (duplicate == requirements.end())
        requirements.push_back(requirement);
    binding.module_ = moduleIt->second;
    binding.symbolId_ = requirement.symbolId;
    binding.contractId_ = requirement.contractId;
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
