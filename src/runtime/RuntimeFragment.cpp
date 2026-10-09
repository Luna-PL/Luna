#include "RuntimeFragment.h"
#include "RuntimeFragmentCompilerBridge.h"
#include "RuntimeOwnedResult.h"

#include "RuntimeDescriptorABI.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace luna::runtime {

struct RuntimeFragmentBindingSetState {
    struct Entry {
        RuntimeSlotRequirement slot;
        std::vector<std::shared_ptr<RuntimeFragmentRef>> chain;
    };
    std::vector<Entry> entries;
};

struct RuntimeFragmentExecutionContextState {
    uint64_t magic = 0;
    RuntimeFragmentBindingSet bindings;
};

struct RuntimeFragmentRefHandleState {
    uint64_t magic = 0;
    RuntimeSlotRequirement slot;
    RuntimeFragmentBindingSet singleton;
};

struct RuntimeFragmentActivationState {
    uint64_t magic = 0;
    // Public activations own these records. Synchronous chain activations
    // leave them empty and borrow the pinned entry's Slot and dispatch's
    // owned argument carrier.
    // Keeping one state type avoids a second opaque ABI representation.
    RuntimeSlotRequirement ownedSlot;
    RuntimeFragmentArguments ownedArguments;
    const RuntimeSlotRequirement* slot = nullptr;
    const RuntimeFragmentArguments* arguments = nullptr;
    RuntimeFragmentResumeCallback continuation = nullptr;
    void* continuationContext = nullptr;
    bool consumed = false;
    bool failed = false;
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
constexpr uint64_t RuntimeFragmentRefHandleMagic =
    0x4c554e4152454631ULL; // "LUNAREF1"

struct OwnedResultState {
    void* payload = nullptr;
    RuntimeOwnedResultHandle::DropEntry drop = nullptr;
    std::shared_ptr<const void> codeLease;
    void** boundCell = nullptr;
};

std::mutex ownedResultMutex;
std::unordered_map<uintptr_t, std::unique_ptr<OwnedResultState>> ownedResults;
uintptr_t nextOwnedResultToken = 1;
std::unordered_map<uintptr_t, std::shared_ptr<const void>> ownedResultCodeLeases;
uintptr_t nextOwnedResultCodeLeaseToken = 1;

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

bool validateActivationContract(
    const RuntimeSlotRequirement& slot,
    const RuntimeFragmentArguments& arguments,
    RuntimeFragmentResumeCallback continuation,
    std::string& error) {
    error.clear();
    if (!validIdentity(slot.slotId) || !validIdentity(slot.contractId) ||
        !validIdentity(arguments.layoutId) || !continuation ||
        !validStorage(arguments.data, arguments.size, arguments.alignment)) {
        error = "runtime Fragment activation contract is invalid";
        return false;
    }
    return true;
}

// The records must outlive this activation. Public construction points at
// storage owned by the same heap state; the private synchronous path points
// at the immutable BindingSet entry and enclosing dispatch's argument carrier.
// The dispatch snapshot pin retains the entry through every nested resume.
void initializeActivationState(
    RuntimeFragmentActivationState& state,
    const RuntimeSlotRequirement& slot,
    const RuntimeFragmentArguments& arguments,
    RuntimeFragmentResumeCallback continuation,
    void* continuationContext) {
    state.slot = &slot;
    state.arguments = &arguments;
    state.continuation = continuation;
    state.continuationContext = continuationContext;
    state.consumed = false;
    state.failed = false;
    state.magic = RuntimeFragmentActivationMagic;
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
    if (!validateActivationContract(slot, arguments, continuation, error))
        return false;
    auto state = std::make_unique<RuntimeFragmentActivationState>();
    state->ownedSlot = slot;
    state->ownedArguments = std::move(arguments);
    initializeActivationState(
        *state, state->ownedSlot, state->ownedArguments,
        continuation, continuationContext);
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
    // Install the incoming value before invoking old cleanup. A synchronous
    // cleanup callback observes the new state and may reset/replace it; no
    // assignment after that callback may silently overwrite its changes.
    RuntimeFragmentRef retired(std::move(*this));
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
    // Detach first so nested reset is empty and live-reference rebind survives.
    // Declaration order keeps the original generation pinned until both owned
    // destroy and borrowed-environment lease cleanup have finished.
    auto binding = std::exchange(binding_, MoonRuntime::PinnedBinding{});
    auto environmentLease = std::move(environmentLease_);
    const auto* descriptor = std::exchange(descriptor_, nullptr);
    void* environment = std::exchange(environment_, nullptr);
    const bool ownsEnvironment = std::exchange(ownsEnvironment_, false);
    if (ownsEnvironment && descriptor && descriptor->destroy && environment)
        descriptor->destroy(environment);
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
    // The factory may synchronously clear or replace the caller's binding.
    // Retain the exact generation we validate through construction and any
    // rejected-output cleanup, then transfer that pin to the new reference.
    auto pinnedBinding = binding;
    const LunaRuntimeFragmentDescriptorV1* descriptor = nullptr;
    if (!validateBinding(pinnedBinding, slot, descriptor, error)) return false;

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

    output.binding_ = std::move(pinnedBinding);
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
            if (entry.slot.slotId != requirement.slotId)
                return entry.slot.slotId < requirement.slotId;
            return entry.slot.contractId < requirement.contractId;
        });
    if (found == state.entries.end() ||
        found->slot.slotId != slot.slotId ||
        found->slot.contractId != slot.contractId)
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
        entry.slot = {binding.slotId(), binding.slotContractId()};
        entry.chain.push_back(
            std::make_shared<RuntimeFragmentRef>(std::move(binding)));
        selected.push_back(std::move(entry));
    }
    std::stable_sort(
        selected.begin(), selected.end(),
        [](const auto& left, const auto& right) {
            if (left.slot.slotId != right.slot.slotId)
                return left.slot.slotId < right.slot.slotId;
            return left.slot.contractId < right.slot.contractId;
        });
    for (auto& candidate : selected) {
        if (state->entries.empty() ||
            state->entries.back().slot.slotId != candidate.slot.slotId ||
            state->entries.back().slot.contractId !=
                candidate.slot.contractId) {
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
    // Resume is synchronous, so this frame remains live until the handler and
    // all downstream handlers return. No per-handler heap state or identity
    // copies are needed. Slot identity comes from the pinned immutable entry,
    // never from caller storage; the dispatch owns its argument carrier and
    // retains the complete BindingSet generation/environment snapshot.
    RuntimeFragmentActivationState activation;
    if (!validateActivationContract(
            dispatch.entry->slot, dispatch.arguments,
            resumeRuntimeFragmentChain, *dispatch.error)) {
        dispatch.failed = true;
        return false;
    }
    initializeActivationState(
        activation, dispatch.entry->slot, dispatch.arguments,
        resumeRuntimeFragmentChain, &dispatch);
    try {
        fragment->descriptor()->execute(
            fragment->environment(), &activation);
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
    // Execute has no result channel. Preserve protocol failure even if a
    // native handler ignores resume's negative result, without replacing a
    // more specific error already reported by the downstream continuation.
    if (activation.failed && !dispatch.failed) {
        *dispatch.error = "runtime Fragment activation violated its single-shot continuation contract";
        dispatch.failed = true;
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

    RuntimeFragmentBindingSet replacementSnapshot;
    if (!buildRuntimeFragmentBindingState(
            std::move(replacement), true, replacementSnapshot.state_, error))
        return false;
    return makeRuntimeFragmentBindingOverrideFromSnapshot(
        base, slot, replacementSnapshot, output, error);
}

bool makeRuntimeFragmentBindingOverrideFromSnapshot(
    const RuntimeFragmentBindingSet& base,
    const RuntimeSlotRequirement& slot,
    const RuntimeFragmentBindingSet& replacement,
    RuntimeFragmentBindingSet& output, std::string& error) {
    error.clear();
    if (!base.state_ || !replacement.state_ || output ||
        !validIdentity(slot.slotId) || !validIdentity(slot.contractId)) {
        error = "runtime Fragment local override input is invalid";
        return false;
    }
    const auto& replacementState = replacement.state_;
    if (replacementState->entries.size() > 1 ||
        (!replacementState->entries.empty() &&
         (replacementState->entries.front().slot.slotId != slot.slotId ||
          replacementState->entries.front().slot.contractId !=
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
            if (entry.slot.slotId != requirement.slotId)
                return entry.slot.slotId < requirement.slotId;
            return entry.slot.contractId < requirement.contractId;
        });
    const bool existing = position != state->entries.end() &&
        position->slot.slotId == slot.slotId &&
        position->slot.contractId == slot.contractId;
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
    // Callbacks may synchronously replace/release the caller's published
    // handle. Own this exact snapshot until the complete chain has unwound;
    // no subsequent access may depend on the receiver still owning state_.
    const auto state = state_;
    if (!state) {
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
    const auto* found = findBindingEntry(*state, slot);
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
        found, 0, std::move(arguments), baseContinuation,
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

bool makeRuntimeFragmentExecutionContextOverride(
    const RuntimeFragmentExecutionContext& base,
    const RuntimeSlotRequirement& slot,
    const RuntimeFragmentBindingSet& replacement,
    RuntimeFragmentExecutionContext& output, std::string& error) {
    error.clear();
    if (!base.state_ ||
        base.state_->magic != RuntimeFragmentExecutionContextMagic || output) {
        error = "runtime Fragment local context override input is invalid";
        return false;
    }
    RuntimeFragmentBindingSet derived;
    if (!makeRuntimeFragmentBindingOverrideFromSnapshot(
            base.state_->bindings, slot, replacement, derived, error))
        return false;
    return makeRuntimeFragmentExecutionContext(derived, output, error);
}

RuntimeFragmentRefHandle::RuntimeFragmentRefHandle(
    RuntimeFragmentRefHandle&& other) noexcept : handle_(other.release()) {}

RuntimeFragmentRefHandle& RuntimeFragmentRefHandle::operator=(
    RuntimeFragmentRefHandle&& other) noexcept {
    if (this != &other) {
        void* retired = handle_;
        handle_ = other.release();
        luna_runtime_fragment_ref_drop_v1(&retired);
    }
    return *this;
}

RuntimeFragmentRefHandle::~RuntimeFragmentRefHandle() { reset(); }

void* RuntimeFragmentRefHandle::release() noexcept {
    return std::exchange(handle_, nullptr);
}

void RuntimeFragmentRefHandle::reset() noexcept {
    luna_runtime_fragment_ref_drop_v1(&handle_);
}

bool makeRuntimeFragmentRefHandle(
    RuntimeFragmentRef& reference, const RuntimeSlotRequirement& slot,
    RuntimeFragmentRefHandle& output, std::string& error) {
    error.clear();
    if (output || !reference ||
        !validIdentity(slot.slotId) || !validIdentity(slot.contractId) ||
        slot.slotId != reference.slotId() ||
        slot.contractId != reference.slotContractId()) {
        error = "runtime Fragment Ref handle requires an empty output and one exact Slot reference";
        return false;
    }
    auto handle = std::make_unique<RuntimeFragmentRefHandleState>();
    auto snapshot = std::make_shared<RuntimeFragmentBindingSetState>();
    auto pinned = std::make_shared<RuntimeFragmentRef>();
    snapshot->entries.push_back({slot, {pinned}});
    handle->slot = slot;
    handle->singleton.state_ = std::move(snapshot);
    handle->magic = RuntimeFragmentRefHandleMagic;
    // Nothing below allocates, validates or invokes a factory. Publish only
    // after every allocation has succeeded, leaving the source empty once.
    *pinned = std::move(reference);
    output.handle_ = handle.release();
    return true;
}

#ifdef LUNA_PRIVATE_REF_JIT_TEST
bool pinRuntimeFragmentRefHandleForTest(
    const RuntimeFragmentRefHandle& source,
    RuntimeFragmentRefHandle& output, std::string& error) {
    error.clear();
    const auto* original = static_cast<const RuntimeFragmentRefHandleState*>(
        source.opaque());
    if (output || !original ||
        luna_runtime_fragment_ref_check_v1(
            source.opaque(), original->slot.slotId.c_str(),
            original->slot.contractId.c_str()) !=
                LUNA_RUNTIME_FRAGMENT_REF_SUCCESS_V1) {
        error = "private Ref call pin requires one live handle and empty output";
        return false;
    }
    auto pinned = std::make_unique<RuntimeFragmentRefHandleState>(*original);
    output.handle_ = pinned.release();
    return true;
}
#endif

bool makeRuntimeFragmentExecutionContextOverrideFromRef(
    const RuntimeFragmentExecutionContext& base,
    const RuntimeSlotRequirement& slot,
    const RuntimeFragmentRefHandle& reference,
    RuntimeFragmentExecutionContext& output, std::string& error) {
    error.clear();
    if (!validIdentity(slot.slotId) || !validIdentity(slot.contractId) ||
        luna_runtime_fragment_ref_check_v1(
            reference.opaque(), slot.slotId.c_str(), slot.contractId.c_str()) !=
                LUNA_RUNTIME_FRAGMENT_REF_SUCCESS_V1) {
        error = "runtime Fragment Ref handle does not match the exact Slot";
        return false;
    }
    const auto* state = static_cast<const RuntimeFragmentRefHandleState*>(
        reference.opaque());
    return makeRuntimeFragmentExecutionContextOverride(
        base, slot, state->singleton, output, error);
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

RuntimeOwnedResultHandle& RuntimeOwnedResultHandle::operator=(
    RuntimeOwnedResultHandle&& other) noexcept {
    if (this != &other) {
        auto retired = std::move(cell_);
        cell_ = std::move(other.cell_);
        if (retired) luna_runtime_owned_result_drop_v1(retired.get());
    }
    return *this;
}

RuntimeOwnedResultCodeLease::RuntimeOwnedResultCodeLease(
    RuntimeOwnedResultCodeLease&& other) noexcept
    : token_(std::exchange(other.token_, 0)) {}

RuntimeOwnedResultCodeLease& RuntimeOwnedResultCodeLease::operator=(
    RuntimeOwnedResultCodeLease&& other) noexcept {
    if (this != &other) {
        reset();
        token_ = std::exchange(other.token_, 0);
    }
    return *this;
}

RuntimeOwnedResultCodeLease::~RuntimeOwnedResultCodeLease() { reset(); }

bool RuntimeOwnedResultCodeLease::prepare(
    std::shared_ptr<const void> codeLease, std::string& error) {
    error.clear();
    if (!codeLease || token_) {
        error = "runtime owned Result code lease requires a live, empty carrier";
        return false;
    }
    try {
        std::lock_guard<std::mutex> lock(ownedResultMutex);
        if (nextOwnedResultCodeLeaseToken == 0) {
            error = "runtime owned Result code lease tokens exhausted";
            return false;
        }
        const auto token = nextOwnedResultCodeLeaseToken;
        if (!ownedResultCodeLeases.emplace(token, std::move(codeLease)).second) {
            error = "runtime owned Result code lease token collision";
            return false;
        }
        ++nextOwnedResultCodeLeaseToken;
        token_ = token;
        return true;
    } catch (...) {
        error = "runtime owned Result code lease allocation failed";
        return false;
    }
}

void RuntimeOwnedResultCodeLease::reset() noexcept {
    const auto token = std::exchange(token_, 0);
    if (!token) return;
    std::shared_ptr<const void> retired;
    {
        std::lock_guard<std::mutex> lock(ownedResultMutex);
        const auto found = ownedResultCodeLeases.find(token);
        if (found != ownedResultCodeLeases.end()) {
            retired = std::move(found->second);
            ownedResultCodeLeases.erase(found);
        }
    }
}

RuntimeOwnedResultHandle::~RuntimeOwnedResultHandle() { reset(); }

bool RuntimeOwnedResultHandle::prepareEmptyCell(std::string& error) {
    error.clear();
    if (*this) {
        error = "runtime owned Result cell is already occupied";
        return false;
    }
    if (cell_) return true;
    try {
        cell_ = std::make_unique<void*>(nullptr);
        return true;
    } catch (...) {
        error = "runtime owned Result cell allocation failed";
        return false;
    }
}

int32_t RuntimeOwnedResultHandle::dropOnce() noexcept {
    return cell_ ? luna_runtime_owned_result_drop_v1(cell_.get())
                 : LUNA_RUNTIME_OWNED_RESULT_DROP_EMPTY_V1;
}

void RuntimeOwnedResultHandle::reset() noexcept {
    auto retired = std::move(cell_);
    if (retired) luna_runtime_owned_result_drop_v1(retired.get());
}

bool makeRuntimeOwnedResultHandle(
    void* payload, RuntimeOwnedResultHandle::DropEntry drop,
    std::shared_ptr<const void> codeLease,
    RuntimeOwnedResultHandle& output, std::string& error) {
    error.clear();
    if (!payload || !drop || !codeLease || output) {
        error = "runtime owned Result requires a payload, Drop entry, code lease and empty output";
        return false;
    }
    try {
        auto newCell = output.cell_ ? nullptr :
            std::make_unique<void*>(nullptr);
        void** boundCell = newCell ? newCell.get() : output.cell_.get();
        RuntimeOwnedResultCodeLease leaseToken;
        if (!leaseToken.prepare(std::move(codeLease), error)) return false;
        if (luna_runtime_owned_result_adopt_v1(
                payload, drop, leaseToken.opaque(), boundCell) !=
                LUNA_RUNTIME_OWNED_RESULT_ADOPT_SUCCESS_V1) {
            error = "runtime owned Result handle adoption failed";
            return false;
        }
        if (newCell) output.cell_ = std::move(newCell);
        return true;
    } catch (...) {
        error = "runtime owned Result handle allocation failed";
        return false;
    }
}

} // namespace luna::runtime

extern "C" int32_t luna_runtime_owned_result_adopt_v1(
    void* payload, LunaRuntimeOwnedResultDropEntryV1 drop,
    const void* code_lease, void** owner_cell) {
    if (!owner_cell ||
        (reinterpret_cast<uintptr_t>(owner_cell) & (alignof(void*) - 1)) != 0 ||
        *owner_cell)
        return LUNA_RUNTIME_OWNED_RESULT_ADOPT_INVALID_OUTPUT_V1;
    if (!payload || !drop || !code_lease)
        return LUNA_RUNTIME_OWNED_RESULT_ADOPT_INVALID_RESOURCE_V1;
    try {
        std::shared_ptr<const void> lease;
        {
            std::lock_guard<std::mutex> lock(luna::runtime::ownedResultMutex);
            const auto found = luna::runtime::ownedResultCodeLeases.find(
                reinterpret_cast<uintptr_t>(code_lease));
            if (found == luna::runtime::ownedResultCodeLeases.end())
                return LUNA_RUNTIME_OWNED_RESULT_ADOPT_INVALID_RESOURCE_V1;
            lease = found->second;
        }
        auto state = std::make_unique<luna::runtime::OwnedResultState>();
        state->payload = payload;
        state->drop = drop;
        state->codeLease = lease;
        state->boundCell = owner_cell;
        std::lock_guard<std::mutex> lock(luna::runtime::ownedResultMutex);
        if (luna::runtime::nextOwnedResultToken == 0)
            return LUNA_RUNTIME_OWNED_RESULT_ADOPT_FAILED_V1;
        const uintptr_t token = luna::runtime::nextOwnedResultToken;
        const auto inserted = luna::runtime::ownedResults.emplace(
            token, std::move(state));
        if (!inserted.second)
            return LUNA_RUNTIME_OWNED_RESULT_ADOPT_FAILED_V1;
        ++luna::runtime::nextOwnedResultToken;
        *owner_cell = reinterpret_cast<void*>(token);
        return LUNA_RUNTIME_OWNED_RESULT_ADOPT_SUCCESS_V1;
    } catch (...) {
        return LUNA_RUNTIME_OWNED_RESULT_ADOPT_FAILED_V1;
    }
}

extern "C" int32_t luna_runtime_owned_result_lease_check_v1(
    const void* code_lease) {
    if (!code_lease) return LUNA_RUNTIME_OWNED_RESULT_LEASE_INVALID_V1;
    try {
        std::lock_guard<std::mutex> lock(luna::runtime::ownedResultMutex);
        return luna::runtime::ownedResultCodeLeases.count(
            reinterpret_cast<uintptr_t>(code_lease))
            ? LUNA_RUNTIME_OWNED_RESULT_LEASE_LIVE_V1
            : LUNA_RUNTIME_OWNED_RESULT_LEASE_INVALID_V1;
    } catch (...) {
        return LUNA_RUNTIME_OWNED_RESULT_LEASE_INVALID_V1;
    }
}

extern "C" int32_t luna_runtime_owned_result_drop_v1(void** owner_cell) {
    if (!owner_cell) return LUNA_RUNTIME_OWNED_RESULT_DROP_INVALID_HANDLE_V1;
    std::unique_ptr<luna::runtime::OwnedResultState> retired;
    {
        std::lock_guard<std::mutex> lock(luna::runtime::ownedResultMutex);
        if (!*owner_cell) return LUNA_RUNTIME_OWNED_RESULT_DROP_EMPTY_V1;
        const auto token = reinterpret_cast<uintptr_t>(*owner_cell);
        const auto found = luna::runtime::ownedResults.find(token);
        if (found == luna::runtime::ownedResults.end() ||
            found->second->boundCell != owner_cell)
            return LUNA_RUNTIME_OWNED_RESULT_DROP_INVALID_HANDLE_V1;
        retired = std::move(found->second);
        luna::runtime::ownedResults.erase(found);
        *owner_cell = nullptr;
    }
    // The code lease stays in retired through the entire generated Drop call.
    // A verified Drop thunk must consume its payload without throwing. If it
    // violates that contract, unloading its code would be unsafe.
    try {
        if (retired->drop(&retired->payload) != 0 || retired->payload)
            std::terminate();
    } catch (...) {
        std::terminate();
    }
    return LUNA_RUNTIME_OWNED_RESULT_DROP_SUCCESS_V1;
}

extern "C" int32_t luna_runtime_fragment_ref_check_v1(
    const void* reference, const char* slot_id, const char* slot_contract_id) {
    const auto* state = static_cast<const
        luna::runtime::RuntimeFragmentRefHandleState*>(reference);
    if (!state || state->magic != luna::runtime::RuntimeFragmentRefHandleMagic)
        return LUNA_RUNTIME_FRAGMENT_REF_INVALID_HANDLE_V1;
    if (!luna::runtime::validText(slot_id) ||
        !luna::runtime::validText(slot_contract_id))
        return LUNA_RUNTIME_FRAGMENT_REF_INVALID_TARGET_V1;
    // Singleton shape is guaranteed only by the host constructor. No public
    // factory accepts an arbitrary frozen BindingSet or a multi-Fragment chain.
    return state->slot.slotId == slot_id &&
           state->slot.contractId == slot_contract_id
        ? LUNA_RUNTIME_FRAGMENT_REF_SUCCESS_V1
        : LUNA_RUNTIME_FRAGMENT_REF_INVALID_TARGET_V1;
}

extern "C" int32_t luna_runtime_fragment_ref_transfer_v1(
    void** source, const char* slot_id, const char* slot_contract_id,
    void** destination) {
    if (!source || !destination || source == destination || *destination)
        return LUNA_RUNTIME_FRAGMENT_REF_INVALID_CARRIER_V1;
    const int32_t checked = luna_runtime_fragment_ref_check_v1(
        *source, slot_id, slot_contract_id);
    if (checked != LUNA_RUNTIME_FRAGMENT_REF_SUCCESS_V1) return checked;
    void* moved = *source;
    *source = nullptr;
    *destination = moved;
    return LUNA_RUNTIME_FRAGMENT_REF_SUCCESS_V1;
}

extern "C" void luna_runtime_fragment_ref_drop_v1(void** reference) {
    if (!reference || !*reference) return;
    auto* retired = static_cast<
        luna::runtime::RuntimeFragmentRefHandleState*>(*reference);
    *reference = nullptr;
    retired->magic = 0;
    delete retired;
}

extern "C" int32_t luna_compiler_fragment_context_check(
    const void* parent_context) {
    const auto* parent = static_cast<const
        luna::runtime::RuntimeFragmentExecutionContextState*>(parent_context);
    return parent && parent->magic ==
        luna::runtime::RuntimeFragmentExecutionContextMagic
        ? LUNA_COMPILER_FRAGMENT_OVERRIDE_SUCCESS
        : LUNA_COMPILER_FRAGMENT_OVERRIDE_INVALID_CONTEXT;
}

extern "C" int32_t luna_compiler_fragment_context_override_from_ref(
    const void* parent_context, const void* reference,
    const char* slot_id, const char* slot_contract_id,
    void** output_context) {
    if (!output_context || *output_context)
        return LUNA_COMPILER_FRAGMENT_OVERRIDE_INVALID_OUTPUT;
    const auto* parent = static_cast<const
        luna::runtime::RuntimeFragmentExecutionContextState*>(parent_context);
    if (!parent || parent->magic !=
            luna::runtime::RuntimeFragmentExecutionContextMagic)
        return LUNA_COMPILER_FRAGMENT_OVERRIDE_INVALID_CONTEXT;
    if (!luna::runtime::validText(slot_id) ||
        !luna::runtime::validText(slot_contract_id) ||
        luna_runtime_fragment_ref_check_v1(
            reference, slot_id, slot_contract_id) !=
                LUNA_RUNTIME_FRAGMENT_REF_SUCCESS_V1)
        return LUNA_COMPILER_FRAGMENT_OVERRIDE_INVALID_REFERENCE;
    try {
        const auto* selected = static_cast<const
            luna::runtime::RuntimeFragmentRefHandleState*>(reference);
        const luna::runtime::RuntimeSlotRequirement slot{
            slot_id, slot_contract_id};
        luna::runtime::RuntimeFragmentBindingSet bindings;
        std::string error;
        if (!luna::runtime::makeRuntimeFragmentBindingOverrideFromSnapshot(
                parent->bindings, slot, selected->singleton,
                bindings, error))
            return LUNA_COMPILER_FRAGMENT_OVERRIDE_FAILED;
        auto derived = std::make_unique<
            luna::runtime::RuntimeFragmentExecutionContextState>();
        derived->bindings = std::move(bindings);
        derived->magic =
            luna::runtime::RuntimeFragmentExecutionContextMagic;
        *output_context = derived.release();
        return LUNA_COMPILER_FRAGMENT_OVERRIDE_SUCCESS;
    } catch (...) {
        return LUNA_COMPILER_FRAGMENT_OVERRIDE_FAILED;
    }
}

extern "C" void luna_compiler_fragment_context_drop(void** context) {
    if (!context || !*context) return;
    auto* retired = static_cast<
        luna::runtime::RuntimeFragmentExecutionContextState*>(
            std::exchange(*context, nullptr));
    retired->magic = 0;
    delete retired;
}

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
        !state->slot || !state->arguments ||
        !slot_id || !slot_contract_id || !arguments_layout_id ||
        state->slot->slotId != slot_id ||
        state->slot->contractId != slot_contract_id ||
        state->arguments->layoutId != arguments_layout_id ||
        state->arguments->size != arguments_size ||
        state->arguments->alignment != arguments_alignment)
        return nullptr;
    return state->arguments->data;
}

extern "C" int32_t luna_runtime_fragment_activation_resume_v1(
    void* activation) {
    auto* state = static_cast<
        luna::runtime::RuntimeFragmentActivationState*>(activation);
    if (!state ||
        state->magic != luna::runtime::RuntimeFragmentActivationMagic)
        return LUNA_RUNTIME_FRAGMENT_DISPATCH_EXECUTION_FAILED_V1;
    if (state->failed || state->consumed || !state->continuation) {
        state->failed = true;
        return LUNA_RUNTIME_FRAGMENT_DISPATCH_EXECUTION_FAILED_V1;
    }
    state->consumed = true;
    try {
        const int32_t result =
            state->continuation(state->continuationContext);
        if (state->failed ||
            (result != LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1 &&
             result != LUNA_RUNTIME_FRAGMENT_CONTINUATION_ESCAPED_V1)) {
            state->failed = true;
            return LUNA_RUNTIME_FRAGMENT_DISPATCH_EXECUTION_FAILED_V1;
        }
        return result;
    } catch (...) {
        state->failed = true;
        return LUNA_RUNTIME_FRAGMENT_DISPATCH_EXECUTION_FAILED_V1;
    }
}
