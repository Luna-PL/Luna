#include "RuntimeFragment.h"

#include "RuntimeDescriptorABI.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <utility>

namespace luna::runtime {

struct RuntimeFragmentBindingSetState {
    struct Entry {
        std::string slotId;
        std::string slotContractId;
        std::vector<std::shared_ptr<RuntimeFragmentRef>> chain;
    };
    std::vector<Entry> entries;
};

struct RuntimeFragmentExecutionContextState {
    uint64_t magic = 0;
    RuntimeFragmentBindingSet bindings;
};

struct RuntimeFragmentActivationState {
    uint64_t magic = 0;
    RuntimeSlotRequirement slot;
    RuntimeFragmentArguments arguments;
    RuntimeFragmentResumeCallback continuation = nullptr;
    void* continuationContext = nullptr;
    bool consumed = false;
};

namespace {

static_assert(
    static_cast<uint32_t>(GenerationBindingFragmentExecutable) ==
        static_cast<uint32_t>(
            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_EXECUTABLE_V1),
    "generation and descriptor Fragment capability flags must agree");

constexpr size_t MaxIdentityBytes = 4096;
constexpr uint64_t MaxEnvironmentAlignment = 1u << 20;
constexpr uint64_t RuntimeFragmentActivationMagic =
    0x4c554e4141435431ULL; // "LUNAACT1"
constexpr uint64_t RuntimeFragmentExecutionContextMagic =
    0x4c554e4145584331ULL; // "LUNAEXC1"

const std::string& emptyString() {
    static const std::string empty;
    return empty;
}

bool validText(const char* text, bool allowEmpty = false) {
    if (!text) return false;
    size_t length = 0;
    while (length < MaxIdentityBytes && text[length] != '\0') {
        if (text[length] == '\r' || text[length] == '\n' ||
            text[length] == '\t')
            return false;
        ++length;
    }
    return length != MaxIdentityBytes && (allowEmpty || length != 0);
}

bool validIdentity(const std::string& value) {
    // Explicit count includes NUL: C++ and C ABI keys must denote one identity.
    return !value.empty() && value.size() < MaxIdentityBytes &&
        value.find_first_of("\r\n\t\0", 0, 4) == std::string::npos;
}

bool powerOfTwo(uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

// A declared alignment is not evidence that the actual payload address
// satisfies it. Check before generated code can perform a typed load. This
// does not establish allocation bounds or lifetime; those remain host duties.
bool validStorage(const void* data, uint64_t size, uint64_t alignment) {
    if (size == 0) return !data && alignment == 1;
    return data && powerOfTwo(alignment) &&
        alignment <= MaxEnvironmentAlignment &&
        (reinterpret_cast<uintptr_t>(data) & (alignment - 1)) == 0;
}

bool validateBinding(
    const MoonRuntime::PinnedBinding& binding,
    const RuntimeSlotRequirement& slot,
    const LunaRuntimeFragmentDescriptorV1*& descriptor,
    std::string& error) {
    if (!binding ||
        binding.declarationKind() != LUNA_RUNTIME_DECLARATION_FRAGMENT_V1 ||
        (binding.flags() & GenerationBindingFragmentExecutable) == 0 ||
        !binding.implementation()) {
        error = "runtime Fragment construction requires an executable Fragment binding";
        return false;
    }
    if (!validIdentity(slot.slotId) || !validIdentity(slot.contractId)) {
        error = "runtime Fragment Slot requirement is invalid";
        return false;
    }
    descriptor = static_cast<const LunaRuntimeFragmentDescriptorV1*>(
        binding.implementation());
    if (!validateRuntimeFragmentDescriptor(*descriptor, error)) return false;
    if (binding.symbolId() != descriptor->fragment_id ||
        binding.contractId() != descriptor->fragment_contract_id) {
        error = "runtime Fragment descriptor disagrees with its generation binding";
        return false;
    }
    if (slot.slotId != descriptor->slot_id ||
        slot.contractId != descriptor->slot_contract_id) {
        error = "runtime Fragment does not target the required SlotId/ContractId";
        return false;
    }
    return true;
}

} // namespace

bool validateRuntimeFragmentDescriptor(
    const LunaRuntimeFragmentDescriptorV1& descriptor,
    std::string& error) {
    error.clear();
    const uint32_t knownFlags = LUNA_RUNTIME_FRAGMENT_CAPTURE_FREE_V1;
    if (descriptor.magic != LUNA_RUNTIME_FRAGMENT_MAGIC_V1 ||
        descriptor.abi_version != LUNA_RUNTIME_FRAGMENT_ABI_V1 ||
        descriptor.struct_size < sizeof(LunaRuntimeFragmentDescriptorV1) ||
        (descriptor.flags & ~knownFlags) != 0 ||
        descriptor.reserved_zero_0 != 0 ||
        descriptor.reserved_zero_1 != 0 ||
        descriptor.reserved_zero_2 != 0 ||
        descriptor.reserved_zero_3 != 0 ||
        !validText(descriptor.fragment_id) ||
        !validText(descriptor.fragment_contract_id) ||
        !validText(descriptor.slot_id) ||
        !validText(descriptor.slot_contract_id) ||
        !validText(descriptor.slot_arguments_layout_id) ||
        (descriptor.slot_arguments_size == 0
             ? descriptor.slot_arguments_alignment != 1
             : (!powerOfTwo(descriptor.slot_arguments_alignment) ||
                descriptor.slot_arguments_alignment >
                    MaxEnvironmentAlignment)) ||
        !validText(descriptor.factory_contract_id, true) ||
        !validText(descriptor.environment_layout_id) ||
        !descriptor.execute) {
        error = "runtime Fragment descriptor header or identity is invalid";
        return false;
    }

    const bool captureFree =
        (descriptor.flags & LUNA_RUNTIME_FRAGMENT_CAPTURE_FREE_V1) != 0;
    if (captureFree) {
        if (descriptor.environment_size != 0 ||
            descriptor.environment_alignment != 1 ||
            descriptor.factory_contract_id[0] != '\0' ||
            descriptor.factory || descriptor.destroy) {
            error = "capture-free runtime Fragment declares a stateful environment";
            return false;
        }
    } else if (descriptor.environment_size == 0 ||
               !powerOfTwo(descriptor.environment_alignment) ||
               descriptor.environment_alignment > MaxEnvironmentAlignment ||
               descriptor.factory_contract_id[0] == '\0' ||
               !descriptor.factory || !descriptor.destroy) {
        error = "stateful runtime Fragment has an invalid environment contract";
        return false;
    }
    return true;
}

RuntimeFragmentActivation::RuntimeFragmentActivation(
    RuntimeFragmentActivation&&) noexcept = default;

RuntimeFragmentActivation::RuntimeFragmentActivation() = default;

RuntimeFragmentActivation& RuntimeFragmentActivation::operator=(
    RuntimeFragmentActivation&&) noexcept = default;

RuntimeFragmentActivation::~RuntimeFragmentActivation() = default;

bool RuntimeFragmentActivation::resumed() const {
    return state_ && state_->consumed;
}

bool makeRuntimeFragmentActivation(
    const RuntimeSlotRequirement& slot,
    RuntimeFragmentArguments arguments,
    RuntimeFragmentResumeCallback continuation,
    void* continuationContext,
    RuntimeFragmentActivation& output,
    std::string& error) {
    error.clear();
    if (output) {
        error = "runtime Fragment activation output is already initialized";
        return false;
    }
    if (!validIdentity(slot.slotId) || !validIdentity(slot.contractId) ||
        !validIdentity(arguments.layoutId) || !continuation ||
        !validStorage(arguments.data, arguments.size, arguments.alignment)) {
        error = "runtime Fragment activation contract is invalid";
        return false;
    }
    auto state = std::make_unique<RuntimeFragmentActivationState>();
    state->magic = RuntimeFragmentActivationMagic;
    state->slot = slot;
    state->arguments = std::move(arguments);
    state->continuation = continuation;
    state->continuationContext = continuationContext;
    output.state_ = std::move(state);
    return true;
}

bool snapshotRuntimeFragmentCandidates(
    const MoonRuntime::PinnedGeneration& generation,
    const RuntimeSlotRequirement& slot,
    RuntimeFragmentCandidateSnapshot& output,
    std::string& error) {
    error.clear();
    if (output) {
        error = "runtime Fragment candidate snapshot is already initialized";
        return false;
    }
    if (!generation || !validIdentity(slot.slotId) ||
        !validIdentity(slot.contractId)) {
        error = "runtime Fragment candidate query is invalid";
        return false;
    }
    const uint32_t requiredFlags =
        GenerationBindingPublicControl |
        GenerationBindingFragmentExecutable;
    auto bindings = generation.findAll(
        LUNA_RUNTIME_DECLARATION_FRAGMENT_V1, requiredFlags);
    std::vector<MoonRuntime::PinnedBinding> candidates;
    candidates.reserve(bindings.size());
    for (auto& binding : bindings) {
        const auto* descriptor =
            static_cast<const LunaRuntimeFragmentDescriptorV1*>(
                binding.implementation());
        if (!descriptor ||
            !validateRuntimeFragmentDescriptor(*descriptor, error) ||
            binding.symbolId() != descriptor->fragment_id ||
            binding.contractId() != descriptor->fragment_contract_id) {
            if (error.empty())
                error = "runtime Fragment candidate descriptor disagrees with its binding";
            return false;
        }
        if (slot.slotId == descriptor->slot_id &&
            slot.contractId == descriptor->slot_contract_id)
            candidates.push_back(std::move(binding));
    }
    output.generation_ = generation;
    output.slot_ = slot;
    output.candidates_ = std::move(candidates);
    output.valid_ = true;
    return true;
}

RuntimeFragmentRef::RuntimeFragmentRef(RuntimeFragmentRef&& other) noexcept
    : binding_(std::move(other.binding_)),
      descriptor_(std::exchange(other.descriptor_, nullptr)),
      environment_(std::exchange(other.environment_, nullptr)),
      environmentLease_(std::move(other.environmentLease_)),
      ownsEnvironment_(std::exchange(other.ownsEnvironment_, false)) {}

RuntimeFragmentRef& RuntimeFragmentRef::operator=(
    RuntimeFragmentRef&& other) noexcept {
    if (this == &other) return *this;
    reset();
    binding_ = std::move(other.binding_);
    descriptor_ = std::exchange(other.descriptor_, nullptr);
    environment_ = std::exchange(other.environment_, nullptr);
    environmentLease_ = std::move(other.environmentLease_);
    ownsEnvironment_ = std::exchange(other.ownsEnvironment_, false);
    return *this;
}

RuntimeFragmentRef::~RuntimeFragmentRef() {
    reset();
}

uint64_t RuntimeFragmentRef::generationId() const {
    return binding_.generationId();
}

const std::string& RuntimeFragmentRef::fragmentId() const {
    return descriptor_ ? binding_.symbolId() : emptyString();
}

const std::string& RuntimeFragmentRef::fragmentContractId() const {
    return descriptor_ ? binding_.contractId() : emptyString();
}

const char* RuntimeFragmentRef::slotId() const {
    return descriptor_ ? descriptor_->slot_id : "";
}

const char* RuntimeFragmentRef::slotContractId() const {
    return descriptor_ ? descriptor_->slot_contract_id : "";
}

void RuntimeFragmentRef::reset() noexcept {
    if (ownsEnvironment_ && descriptor_ && descriptor_->destroy && environment_)
        descriptor_->destroy(environment_);
    environmentLease_.reset();
    environment_ = nullptr;
    descriptor_ = nullptr;
    ownsEnvironment_ = false;
    binding_ = {};
}

bool makeOwnedRuntimeFragmentRef(
    const MoonRuntime::PinnedBinding& binding,
    const RuntimeSlotRequirement& slot,
    const RuntimeFragmentFactoryArguments& arguments,
    RuntimeFragmentRef& output, std::string& error) {
    error.clear();
    if (output) {
        error = "runtime Fragment output is already initialized";
        return false;
    }
    const LunaRuntimeFragmentDescriptorV1* descriptor = nullptr;
    if (!validateBinding(binding, slot, descriptor, error)) return false;

    void* environment = nullptr;
    const bool captureFree =
        (descriptor->flags & LUNA_RUNTIME_FRAGMENT_CAPTURE_FREE_V1) != 0;
    if (captureFree) {
        if (!arguments.contractId.empty() || arguments.data) {
            error = "capture-free runtime Fragment does not accept factory arguments";
            return false;
        }
    } else {
        if (arguments.contractId != descriptor->factory_contract_id ||
            !arguments.data) {
            error = "runtime Fragment factory arguments do not match its contract";
            return false;
        }
        try {
            if (descriptor->factory(arguments.data, &environment) != 0 ||
                !environment) {
                if (environment) descriptor->destroy(environment);
                error = "runtime Fragment factory failed to create its environment";
                return false;
            }
        } catch (const std::exception& exception) {
            if (environment) descriptor->destroy(environment);
            error = "runtime Fragment factory threw: " +
                std::string(exception.what());
            return false;
        } catch (...) {
            if (environment) descriptor->destroy(environment);
            error = "runtime Fragment factory threw";
            return false;
        }
        if (!validStorage(environment, descriptor->environment_size,
                          descriptor->environment_alignment)) {
            descriptor->destroy(environment);
            error = "runtime Fragment factory returned an unaligned environment";
            return false;
        }
    }

    output.binding_ = binding;
    output.descriptor_ = descriptor;
    output.environment_ = environment;
    output.ownsEnvironment_ = !captureFree;
    return true;
}

bool makeBorrowedRuntimeFragmentRef(
    const MoonRuntime::PinnedBinding& binding,
    const RuntimeSlotRequirement& slot,
    BorrowedFragmentEnvironment environment,
    RuntimeFragmentRef& output, std::string& error) {
    error.clear();
    if (output) {
        error = "runtime Fragment output is already initialized";
        return false;
    }
    const LunaRuntimeFragmentDescriptorV1* descriptor = nullptr;
    if (!validateBinding(binding, slot, descriptor, error)) return false;
    const bool captureFree =
        (descriptor->flags & LUNA_RUNTIME_FRAGMENT_CAPTURE_FREE_V1) != 0;
    if (captureFree) {
        if (environment.data || environment.lease || environment.size != 0 ||
            environment.alignment != 0 || !environment.layoutId.empty()) {
            error = "capture-free runtime Fragment does not accept a borrowed environment";
            return false;
        }
    } else if (!environment.data || !environment.lease ||
               environment.layoutId != descriptor->environment_layout_id ||
               environment.size != descriptor->environment_size ||
               environment.alignment != descriptor->environment_alignment ||
               !validStorage(environment.data, environment.size, environment.alignment)) {
        error = "borrowed runtime Fragment environment does not match its layout";
        return false;
    }

    output.binding_ = binding;
    output.descriptor_ = descriptor;
    output.environment_ = environment.data;
    output.environmentLease_ = std::move(environment.lease);
    return true;
}

size_t RuntimeFragmentBindingSet::size() const {
    return state_ ? state_->entries.size() : 0;
}

size_t RuntimeFragmentBindingSet::bindingCount() const {
    if (!state_) return 0;
    size_t result = 0;
    for (const auto& entry : state_->entries) result += entry.chain.size();
    return result;
}

namespace {

const RuntimeFragmentBindingSetState::Entry* findBindingEntry(
    const RuntimeFragmentBindingSetState& state,
    const RuntimeSlotRequirement& slot) {
    const auto found = std::lower_bound(
        state.entries.begin(), state.entries.end(), slot,
        [](const RuntimeFragmentBindingSetState::Entry& entry,
           const RuntimeSlotRequirement& requirement) {
            if (entry.slotId != requirement.slotId)
                return entry.slotId < requirement.slotId;
            return entry.slotContractId < requirement.contractId;
        });
    if (found == state.entries.end() ||
        found->slotId != slot.slotId ||
        found->slotContractId != slot.contractId)
        return nullptr;
    return &*found;
}

bool buildRuntimeFragmentBindingState(
    std::vector<RuntimeFragmentRef> bindings, bool allowChains,
    std::shared_ptr<const RuntimeFragmentBindingSetState>& output,
    std::string& error) {
    auto state = std::make_shared<RuntimeFragmentBindingSetState>();
    std::vector<RuntimeFragmentBindingSetState::Entry> selected;
    selected.reserve(bindings.size());
    for (auto& binding : bindings) {
        if (!binding || !binding.descriptor() ||
            !validText(binding.slotId()) ||
            !validText(binding.slotContractId())) {
            error = "runtime Fragment BindingSet contains an invalid reference";
            return false;
        }
        RuntimeFragmentBindingSetState::Entry entry;
        entry.slotId = binding.slotId();
        entry.slotContractId = binding.slotContractId();
        entry.chain.push_back(
            std::make_shared<RuntimeFragmentRef>(std::move(binding)));
        selected.push_back(std::move(entry));
    }
    std::stable_sort(
        selected.begin(), selected.end(),
        [](const auto& left, const auto& right) {
            if (left.slotId != right.slotId)
                return left.slotId < right.slotId;
            return left.slotContractId < right.slotContractId;
        });
    for (auto& candidate : selected) {
        if (state->entries.empty() ||
            state->entries.back().slotId != candidate.slotId ||
            state->entries.back().slotContractId !=
                candidate.slotContractId) {
            state->entries.push_back(std::move(candidate));
            continue;
        }
        if (!allowChains) {
            error = "runtime Fragment BindingSet selects more than one Fragment for one exact Slot";
            return false;
        }
        auto& chain = state->entries.back().chain;
        const auto* expected = chain.front()->descriptor();
        const auto* actual = candidate.chain.front()->descriptor();
        if (!expected || !actual ||
            std::strcmp(expected->slot_arguments_layout_id,
                        actual->slot_arguments_layout_id) != 0 ||
            expected->slot_arguments_size != actual->slot_arguments_size ||
            expected->slot_arguments_alignment !=
                actual->slot_arguments_alignment) {
            error = "runtime Fragment chain has inconsistent Slot argument layouts";
            return false;
        }
        chain.push_back(std::move(candidate.chain.front()));
    }
    output = std::move(state);
    return true;
}

struct RuntimeFragmentChainDispatch {
    const RuntimeFragmentBindingSetState::Entry* entry = nullptr;
    size_t next = 0;
    RuntimeSlotRequirement slot;
    RuntimeFragmentArguments arguments;
    RuntimeFragmentResumeCallback baseContinuation = nullptr;
    void* continuationContext = nullptr;
    std::string* error = nullptr;
    RuntimeFragmentDispatchOutcome outcome =
        RuntimeFragmentDispatchOutcome::Completed;
    bool failed = false;
};

bool dispatchRuntimeFragmentChain(RuntimeFragmentChainDispatch& dispatch);

int32_t resumeRuntimeFragmentChain(void* context) {
    auto* dispatch = static_cast<RuntimeFragmentChainDispatch*>(context);
    if (!dispatch || !dispatchRuntimeFragmentChain(*dispatch))
        return LUNA_RUNTIME_FRAGMENT_DISPATCH_EXECUTION_FAILED_V1;
    return dispatch->outcome ==
            RuntimeFragmentDispatchOutcome::ContinuationEscaped
        ? LUNA_RUNTIME_FRAGMENT_CONTINUATION_ESCAPED_V1
        : LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1;
}

bool dispatchRuntimeFragmentChain(RuntimeFragmentChainDispatch& dispatch) {
    if (dispatch.failed) return false;
    if (!dispatch.entry || dispatch.next >= dispatch.entry->chain.size()) {
        try {
            const int32_t result =
                dispatch.baseContinuation(dispatch.continuationContext);
            if (result ==
                LUNA_RUNTIME_FRAGMENT_CONTINUATION_ESCAPED_V1) {
                dispatch.outcome =
                    RuntimeFragmentDispatchOutcome::ContinuationEscaped;
            } else if (result !=
                       LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1) {
                *dispatch.error =
                    "runtime Fragment base continuation returned an invalid control result";
                dispatch.failed = true;
                return false;
            }
            return true;
        } catch (const std::exception& exception) {
            *dispatch.error = "runtime Fragment base continuation threw: " +
                std::string(exception.what());
        } catch (...) {
            *dispatch.error = "runtime Fragment base continuation threw";
        }
        dispatch.failed = true;
        return false;
    }

    const auto& fragment = dispatch.entry->chain[dispatch.next++];
    RuntimeFragmentActivation activation;
    if (!makeRuntimeFragmentActivation(
            dispatch.slot, dispatch.arguments,
            resumeRuntimeFragmentChain, &dispatch,
            activation, *dispatch.error)) {
        dispatch.failed = true;
        return false;
    }
    try {
        fragment->descriptor()->execute(
            fragment->environment(), activation.opaque());
    } catch (const std::exception& exception) {
        *dispatch.error = "runtime Fragment execution threw: " +
            std::string(exception.what());
        dispatch.failed = true;
        return false;
    } catch (...) {
        *dispatch.error = "runtime Fragment execution threw";
        dispatch.failed = true;
        return false;
    }
    return !dispatch.failed;
}

} // namespace

size_t RuntimeFragmentBindingSet::chainSize(
    const RuntimeSlotRequirement& slot) const {
    if (!state_) return 0;
    const auto* entry = findBindingEntry(*state_, slot);
    return entry ? entry->chain.size() : 0;
}

bool makeRuntimeFragmentBindingSet(
    std::vector<RuntimeFragmentRef> bindings,
    RuntimeFragmentBindingSet& output, std::string& error) {
    error.clear();
    if (output) {
        error = "runtime Fragment BindingSet output is already initialized";
        return false;
    }
    return buildRuntimeFragmentBindingState(
        std::move(bindings), false, output.state_, error);
}

bool makeRuntimeFragmentChainBindingSet(
    std::vector<RuntimeFragmentRef> bindings,
    RuntimeFragmentBindingSet& output, std::string& error) {
    error.clear();
    if (output) {
        error = "runtime Fragment BindingSet output is already initialized";
        return false;
    }
    return buildRuntimeFragmentBindingState(
        std::move(bindings), true, output.state_, error);
}

bool makeRuntimeFragmentBindingOverride(
    const RuntimeFragmentBindingSet& base,
    const RuntimeSlotRequirement& slot,
    std::vector<RuntimeFragmentRef> replacement,
    RuntimeFragmentBindingSet& output, std::string& error) {
    error.clear();
    if (!base.state_ || output ||
        !validIdentity(slot.slotId) || !validIdentity(slot.contractId)) {
        error = "runtime Fragment local override input is invalid";
        return false;
    }

    std::shared_ptr<const RuntimeFragmentBindingSetState> replacementState;
    if (!buildRuntimeFragmentBindingState(
            std::move(replacement), true, replacementState, error))
        return false;
    if (replacementState->entries.size() > 1 ||
        (!replacementState->entries.empty() &&
         (replacementState->entries.front().slotId != slot.slotId ||
          replacementState->entries.front().slotContractId !=
              slot.contractId))) {
        error = "runtime Fragment local override must target one exact Slot";
        return false;
    }

    auto state = std::make_shared<RuntimeFragmentBindingSetState>();
    state->entries = base.state_->entries;
    const auto position = std::lower_bound(
        state->entries.begin(), state->entries.end(), slot,
        [](const RuntimeFragmentBindingSetState::Entry& entry,
           const RuntimeSlotRequirement& requirement) {
            if (entry.slotId != requirement.slotId)
                return entry.slotId < requirement.slotId;
            return entry.slotContractId < requirement.contractId;
        });
    const bool existing = position != state->entries.end() &&
        position->slotId == slot.slotId &&
        position->slotContractId == slot.contractId;
    const size_t index = static_cast<size_t>(
        position - state->entries.begin());
    if (existing) state->entries.erase(state->entries.begin() + index);
    if (!replacementState->entries.empty())
        state->entries.insert(
            state->entries.begin() + index,
            replacementState->entries.front());
    output.state_ = std::move(state);
    return true;
}

bool RuntimeFragmentBindingSet::dispatch(
    const RuntimeSlotRequirement& slot,
    RuntimeFragmentArguments arguments,
    RuntimeFragmentResumeCallback baseContinuation,
    void* continuationContext,
    std::string& error) const {
    RuntimeFragmentDispatchOutcome outcome;
    return dispatchWithOutcome(
        slot, std::move(arguments), baseContinuation,
        continuationContext, outcome, error);
}

bool RuntimeFragmentBindingSet::dispatchWithOutcome(
    const RuntimeSlotRequirement& slot,
    RuntimeFragmentArguments arguments,
    RuntimeFragmentResumeCallback baseContinuation,
    void* continuationContext,
    RuntimeFragmentDispatchOutcome& outcome,
    std::string& error) const {
    error.clear();
    outcome = RuntimeFragmentDispatchOutcome::Completed;
    if (!state_) {
        error = "runtime Fragment dispatch requires an initialized BindingSet";
        return false;
    }
    if (!validIdentity(slot.slotId) || !validIdentity(slot.contractId) ||
        !baseContinuation) {
        error = "runtime Fragment dispatch requirement is invalid";
        return false;
    }
    if (!validIdentity(arguments.layoutId) ||
        !validStorage(arguments.data, arguments.size, arguments.alignment)) {
        error = "runtime Fragment dispatch argument carrier is invalid";
        return false;
    }
    const auto* found = findBindingEntry(*state_, slot);
    if (!found) {
        try {
            const int32_t result = baseContinuation(continuationContext);
            if (result == LUNA_RUNTIME_FRAGMENT_CONTINUATION_ESCAPED_V1)
                outcome = RuntimeFragmentDispatchOutcome::ContinuationEscaped;
            else if (result !=
                     LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1) {
                error = "runtime Fragment base continuation returned an invalid control result";
                return false;
            }
        } catch (const std::exception& exception) {
            error = "runtime Fragment base continuation threw: " +
                std::string(exception.what());
            return false;
        } catch (...) {
            error = "runtime Fragment base continuation threw";
            return false;
        }
        return true;
    }

    const auto* descriptor = found->chain.empty()
        ? nullptr : found->chain.front()->descriptor();
    if (!descriptor ||
        arguments.layoutId != descriptor->slot_arguments_layout_id ||
        arguments.size != descriptor->slot_arguments_size ||
        arguments.alignment != descriptor->slot_arguments_alignment ||
        (arguments.size == 0
             ? arguments.data != nullptr
             : arguments.data == nullptr)) {
        error = "runtime Fragment dispatch arguments do not match the selected Slot layout";
        return false;
    }

    RuntimeFragmentChainDispatch dispatch{
        found, 0, slot, std::move(arguments), baseContinuation,
        continuationContext, &error,
        RuntimeFragmentDispatchOutcome::Completed, false};
    if (!dispatchRuntimeFragmentChain(dispatch)) return false;
    outcome = dispatch.outcome;
    return true;
}

bool makeRuntimeFragmentExecutionContext(
    const RuntimeFragmentBindingSet& bindings,
    RuntimeFragmentExecutionContext& output,
    std::string& error) {
    error.clear();
    if (!bindings.state_) {
        error = "runtime Fragment execution context requires an initialized BindingSet";
        return false;
    }
    if (output) {
        error = "runtime Fragment execution context output is already initialized";
        return false;
    }
    auto state = std::make_shared<RuntimeFragmentExecutionContextState>();
    state->magic = RuntimeFragmentExecutionContextMagic;
    state->bindings = bindings;
    output.state_ = std::move(state);
    return true;
}

bool RuntimeFragmentExecutionContext::dispatch(
    const RuntimeSlotRequirement& slot,
    RuntimeFragmentArguments arguments,
    RuntimeFragmentResumeCallback baseContinuation,
    void* continuationContext,
    std::string& error) const {
    if (!state_ || state_->magic != RuntimeFragmentExecutionContextMagic) {
        error = "runtime Fragment dispatch requires a valid execution context";
        return false;
    }
    return state_->bindings.dispatch(
        slot, std::move(arguments), baseContinuation,
        continuationContext, error);
}

bool RuntimeFragmentExecutionContext::dispatchWithOutcome(
    const RuntimeSlotRequirement& slot,
    RuntimeFragmentArguments arguments,
    RuntimeFragmentResumeCallback baseContinuation,
    void* continuationContext,
    RuntimeFragmentDispatchOutcome& outcome,
    std::string& error) const {
    if (!state_ || state_->magic != RuntimeFragmentExecutionContextMagic) {
        error = "runtime Fragment dispatch requires a valid execution context";
        return false;
    }
    return state_->bindings.dispatchWithOutcome(
        slot, std::move(arguments), baseContinuation,
        continuationContext, outcome, error);
}

bool MoonRuntime::activateFragmentBindings(
    const RuntimeFragmentBindingSet& bindings,
    SafePoint& safePoint, std::string& error) {
    error.clear();
    if (!bindings.state_) {
        error = "Fragment BindingSet activation requires an initialized set";
        return false;
    }
    if (!consumeSafePoint(safePoint, error)) return false;
#if defined(__cpp_lib_atomic_shared_ptr) && \
    __cpp_lib_atomic_shared_ptr >= 201711L
    activeFragmentBindings_.store(bindings.state_, std::memory_order_release);
#else
    std::atomic_store_explicit(
        &activeFragmentBindings_, bindings.state_,
        std::memory_order_release);
#endif
    return true;
}

RuntimeFragmentBindingSet MoonRuntime::pinFragmentBindings() const {
    RuntimeFragmentBindingSet result;
#if defined(__cpp_lib_atomic_shared_ptr) && \
    __cpp_lib_atomic_shared_ptr >= 201711L
    result.state_ = activeFragmentBindings_.load(std::memory_order_acquire);
#else
    result.state_ = std::atomic_load_explicit(
        &activeFragmentBindings_, std::memory_order_acquire);
#endif
    return result;
}

} // namespace luna::runtime

extern "C" int32_t luna_runtime_fragment_dispatch_v1(
    const void* execution_context,
    const char* slot_id,
    const char* slot_contract_id,
    const char* arguments_layout_id,
    uint64_t arguments_size,
    uint64_t arguments_alignment,
    const void* arguments,
    LunaRuntimeFragmentContinuationFnV1 base_continuation,
    void* continuation_context) {
    try {
        const auto* state = static_cast<const
            luna::runtime::RuntimeFragmentExecutionContextState*>(
                execution_context);
        if (!state ||
            state->magic !=
                luna::runtime::RuntimeFragmentExecutionContextMagic)
            return LUNA_RUNTIME_FRAGMENT_DISPATCH_INVALID_CONTEXT_V1;
        if (!luna::runtime::validText(slot_id) ||
            !luna::runtime::validText(slot_contract_id) ||
            !luna::runtime::validText(arguments_layout_id) ||
            !base_continuation ||
            !luna::runtime::validStorage(
                arguments, arguments_size, arguments_alignment))
            return LUNA_RUNTIME_FRAGMENT_DISPATCH_INVALID_INVOCATION_V1;

        std::string error;
        const luna::runtime::RuntimeSlotRequirement slot{
            slot_id, slot_contract_id};
        const luna::runtime::RuntimeFragmentArguments invocation{
            arguments_layout_id, arguments_size,
            arguments_alignment, arguments};
        luna::runtime::RuntimeFragmentDispatchOutcome outcome;
        if (!state->bindings.dispatchWithOutcome(
                slot, invocation, base_continuation,
                continuation_context, outcome, error))
            return LUNA_RUNTIME_FRAGMENT_DISPATCH_EXECUTION_FAILED_V1;
        return outcome == luna::runtime::RuntimeFragmentDispatchOutcome::
                              ContinuationEscaped
            ? LUNA_RUNTIME_FRAGMENT_DISPATCH_CONTINUATION_ESCAPED_V1
            : LUNA_RUNTIME_FRAGMENT_DISPATCH_SUCCESS_V1;
    } catch (...) {
        return LUNA_RUNTIME_FRAGMENT_DISPATCH_EXECUTION_FAILED_V1;
    }
}

extern "C" const void* luna_runtime_fragment_activation_arguments_v1(
    void* activation,
    const char* slot_id,
    const char* slot_contract_id,
    const char* arguments_layout_id,
    uint64_t arguments_size,
    uint64_t arguments_alignment) {
    auto* state = static_cast<
        luna::runtime::RuntimeFragmentActivationState*>(activation);
    if (!state ||
        state->magic != luna::runtime::RuntimeFragmentActivationMagic ||
        !slot_id || !slot_contract_id || !arguments_layout_id ||
        state->slot.slotId != slot_id ||
        state->slot.contractId != slot_contract_id ||
        state->arguments.layoutId != arguments_layout_id ||
        state->arguments.size != arguments_size ||
        state->arguments.alignment != arguments_alignment)
        return nullptr;
    return state->arguments.data;
}

extern "C" int32_t luna_runtime_fragment_activation_resume_v1(
    void* activation) {
    auto* state = static_cast<
        luna::runtime::RuntimeFragmentActivationState*>(activation);
    if (!state ||
        state->magic != luna::runtime::RuntimeFragmentActivationMagic ||
        state->consumed || !state->continuation)
        return LUNA_RUNTIME_FRAGMENT_DISPATCH_EXECUTION_FAILED_V1;
    state->consumed = true;
    try {
        const int32_t result =
            state->continuation(state->continuationContext);
        if (result != LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1 &&
            result != LUNA_RUNTIME_FRAGMENT_CONTINUATION_ESCAPED_V1)
            return LUNA_RUNTIME_FRAGMENT_DISPATCH_EXECUTION_FAILED_V1;
        return result;
    } catch (...) {
        return LUNA_RUNTIME_FRAGMENT_DISPATCH_EXECUTION_FAILED_V1;
    }
}
