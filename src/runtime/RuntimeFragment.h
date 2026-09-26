#pragma once

#include "MoonRuntime.h"
#include "RuntimeFragmentABI.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace luna::runtime {

struct RuntimeSlotRequirement {
    // Nominal ABI keys cannot contain embedded NUL or CR/LF/tab characters.
    std::string slotId;
    std::string contractId;
};

struct RuntimeFragmentFactoryArguments {
    std::string contractId;
    const void* data = nullptr;
};

struct BorrowedFragmentEnvironment {
    // The address must satisfy the declared alignment, and the lease must
    // keep the full environment storage alive. Runtime checks the alignment,
    // not the truth of the host's claimed bounds or lifetime.
    std::string layoutId;
    uint64_t size = 0;
    uint64_t alignment = 0;
    void* data = nullptr;
    std::shared_ptr<const void> lease;
};

struct RuntimeFragmentArguments {
    // Nonempty storage requires an actually aligned address; an empty record
    // uses size 0, alignment 1, and null data. The host owns bounds/lifetime.
    std::string layoutId;
    uint64_t size = 0;
    uint64_t alignment = 0;
    const void* data = nullptr;
};

using RuntimeFragmentResumeCallback = int32_t (*)(void* context);

enum class RuntimeFragmentDispatchOutcome : uint8_t {
    Completed = 0,
    ContinuationEscaped = 1,
};

struct RuntimeFragmentActivationState;

// Runtime-owned, non-copyable activation. Its opaque address is the only value
// passed to generated Fragment code; the continuation and consumed bit never
// become source-language values.
class RuntimeFragmentActivation {
public:
    RuntimeFragmentActivation();
    RuntimeFragmentActivation(const RuntimeFragmentActivation&) = delete;
    RuntimeFragmentActivation& operator=(const RuntimeFragmentActivation&) = delete;
    RuntimeFragmentActivation(RuntimeFragmentActivation&&) noexcept;
    RuntimeFragmentActivation& operator=(RuntimeFragmentActivation&&) noexcept;
    ~RuntimeFragmentActivation();

    explicit operator bool() const { return state_ != nullptr; }
    void* opaque() const { return state_.get(); }
    bool resumed() const;

private:
    friend bool makeRuntimeFragmentActivation(
        const RuntimeSlotRequirement&, RuntimeFragmentArguments,
        RuntimeFragmentResumeCallback, void*, RuntimeFragmentActivation&,
        std::string&);
    std::unique_ptr<RuntimeFragmentActivationState> state_;
};

bool makeRuntimeFragmentActivation(
    const RuntimeSlotRequirement& slot,
    RuntimeFragmentArguments arguments,
    RuntimeFragmentResumeCallback continuation,
    void* continuationContext,
    RuntimeFragmentActivation& output,
    std::string& error);

// Immutable, generation-pinned result of querying one exact nominal Slot.
// Discovery may scan or use an internal index; callers observe only a stable
// SymbolId-ordered snapshot and remain responsible for selection policy.
class RuntimeFragmentCandidateSnapshot {
public:
    explicit operator bool() const { return valid_; }
    uint64_t generationId() const { return generation_.generationId(); }
    const RuntimeSlotRequirement& slot() const { return slot_; }
    size_t size() const { return candidates_.size(); }
    const MoonRuntime::PinnedBinding* at(size_t index) const {
        return index < candidates_.size() ? &candidates_[index] : nullptr;
    }

private:
    friend bool snapshotRuntimeFragmentCandidates(
        const MoonRuntime::PinnedGeneration&, const RuntimeSlotRequirement&,
        RuntimeFragmentCandidateSnapshot&, std::string&);
    MoonRuntime::PinnedGeneration generation_;
    RuntimeSlotRequirement slot_;
    std::vector<MoonRuntime::PinnedBinding> candidates_;
    bool valid_ = false;
};

bool validateRuntimeFragmentDescriptor(
    const LunaRuntimeFragmentDescriptorV1& descriptor,
    std::string& error);

bool snapshotRuntimeFragmentCandidates(
    const MoonRuntime::PinnedGeneration& generation,
    const RuntimeSlotRequirement& slot,
    RuntimeFragmentCandidateSnapshot& output,
    std::string& error);

// Move-only, generation-pinned runtime value corresponding to the language
// type RuntimeFragmentRef<S>. Construction performs all identity, ABI and
// environment validation once. Dispatch may then use descriptor()/environment()
// without repeating those checks.
class RuntimeFragmentRef {
public:
    RuntimeFragmentRef() = default;
    RuntimeFragmentRef(const RuntimeFragmentRef&) = delete;
    RuntimeFragmentRef& operator=(const RuntimeFragmentRef&) = delete;
    RuntimeFragmentRef(RuntimeFragmentRef&& other) noexcept;
    RuntimeFragmentRef& operator=(RuntimeFragmentRef&& other) noexcept;
    ~RuntimeFragmentRef();

    explicit operator bool() const { return descriptor_ != nullptr; }
    uint64_t generationId() const;
    const std::string& fragmentId() const;
    const std::string& fragmentContractId() const;
    const char* slotId() const;
    const char* slotContractId() const;
    const LunaRuntimeFragmentDescriptorV1* descriptor() const {
        return descriptor_;
    }
    void* environment() const { return environment_; }

    void reset() noexcept;

private:
    friend bool makeOwnedRuntimeFragmentRef(
        const MoonRuntime::PinnedBinding&,
        const RuntimeSlotRequirement&,
        const RuntimeFragmentFactoryArguments&,
        RuntimeFragmentRef&, std::string&);
    friend bool makeBorrowedRuntimeFragmentRef(
        const MoonRuntime::PinnedBinding&,
        const RuntimeSlotRequirement&,
        BorrowedFragmentEnvironment,
        RuntimeFragmentRef&, std::string&);

    MoonRuntime::PinnedBinding binding_;
    const LunaRuntimeFragmentDescriptorV1* descriptor_ = nullptr;
    void* environment_ = nullptr;
    std::shared_ptr<const void> environmentLease_;
    bool ownsEnvironment_ = false;
};

// Retains the validated generation before invoking the factory, including
// failure cleanup. Synchronous replacement/release of the source binding does
// not change the new reference's identity or overwrite the host's handle.
bool makeOwnedRuntimeFragmentRef(
    const MoonRuntime::PinnedBinding& binding,
    const RuntimeSlotRequirement& slot,
    const RuntimeFragmentFactoryArguments& arguments,
    RuntimeFragmentRef& output, std::string& error);

bool makeBorrowedRuntimeFragmentRef(
    const MoonRuntime::PinnedBinding& binding,
    const RuntimeSlotRequirement& slot,
    BorrowedFragmentEnvironment environment,
    RuntimeFragmentRef& output, std::string& error);

struct RuntimeFragmentBindingSetState;
struct RuntimeFragmentExecutionContextState;
class RuntimeFragmentExecutionContext;

// Immutable host policy result. The strict constructor models None/One while
// the explicit chain constructor stores host-ordered bindings for each exact
// SlotId/ContractId. Copies share the frozen state and therefore pin every
// referenced module generation and environment until the final snapshot is
// released.
class RuntimeFragmentBindingSet {
public:
    RuntimeFragmentBindingSet() = default;

    explicit operator bool() const { return state_ != nullptr; }
    // Number of exact Slot keys represented in the set.
    size_t size() const;
    // Total selected Fragment count across all Slot chains.
    size_t bindingCount() const;
    size_t chainSize(const RuntimeSlotRequirement& slot) const;

    // Dispatches one exact Slot against this frozen snapshot. With no binding,
    // the base continuation runs directly. A selected Fragment may resume it
    // at most once or may end without resuming. The call retains the snapshot
    // until all handlers return, even if a synchronous callback replaces or
    // releases the caller's handle. This does not permit concurrent mutation
    // of the same C++ handle or reuse of a released opaque context pointer.
    bool dispatch(
        const RuntimeSlotRequirement& slot,
        RuntimeFragmentArguments arguments,
        RuntimeFragmentResumeCallback baseContinuation,
        void* continuationContext,
        std::string& error) const;
    bool dispatchWithOutcome(
        const RuntimeSlotRequirement& slot,
        RuntimeFragmentArguments arguments,
        RuntimeFragmentResumeCallback baseContinuation,
        void* continuationContext,
        RuntimeFragmentDispatchOutcome& outcome,
        std::string& error) const;

private:
    friend class MoonRuntime;
    friend bool makeRuntimeFragmentBindingSet(
        std::vector<RuntimeFragmentRef>, RuntimeFragmentBindingSet&,
        std::string&);
    friend bool makeRuntimeFragmentChainBindingSet(
        std::vector<RuntimeFragmentRef>, RuntimeFragmentBindingSet&,
        std::string&);
    friend bool makeRuntimeFragmentBindingOverride(
        const RuntimeFragmentBindingSet&, const RuntimeSlotRequirement&,
        std::vector<RuntimeFragmentRef>, RuntimeFragmentBindingSet&,
        std::string&);
    friend bool makeRuntimeFragmentExecutionContext(
        const RuntimeFragmentBindingSet&,
        RuntimeFragmentExecutionContext&, std::string&);
    std::shared_ptr<const RuntimeFragmentBindingSetState> state_;
};

// Consumes the selected references into one immutable set. The empty vector is
// a valid initialized set representing host policy `None` for every Slot.
bool makeRuntimeFragmentBindingSet(
    std::vector<RuntimeFragmentRef> bindings,
    RuntimeFragmentBindingSet& output, std::string& error);

// Consumes references into deterministic per-Slot chains. Input order is the
// host-selected execution order within each exact Slot; loading and SymbolId
// order never affect it. Different Slot keys may be interleaved in the input.
bool makeRuntimeFragmentChainBindingSet(
    std::vector<RuntimeFragmentRef> bindings,
    RuntimeFragmentBindingSet& output, std::string& error);

// Derives a local immutable view by replacing one exact Slot's active chain.
// An empty replacement means local policy None. The base and global active set
// are unchanged; shared references keep their environments/generations pinned.
bool makeRuntimeFragmentBindingOverride(
    const RuntimeFragmentBindingSet& base,
    const RuntimeSlotRequirement& slot,
    std::vector<RuntimeFragmentRef> replacement,
    RuntimeFragmentBindingSet& output, std::string& error);

// Explicit data-plane capability passed from a host entry boundary to
// generated code. It freezes one BindingSet snapshot for the duration of an
// execution and intentionally has no back-pointer to MoonRuntime. Copies pin
// the same immutable snapshot; local overrides become a different context.
class RuntimeFragmentExecutionContext {
public:
    RuntimeFragmentExecutionContext() = default;

    explicit operator bool() const { return state_ != nullptr; }
    const void* opaque() const { return state_.get(); }

    bool dispatch(
        const RuntimeSlotRequirement& slot,
        RuntimeFragmentArguments arguments,
        RuntimeFragmentResumeCallback baseContinuation,
        void* continuationContext,
        std::string& error) const;
    bool dispatchWithOutcome(
        const RuntimeSlotRequirement& slot,
        RuntimeFragmentArguments arguments,
        RuntimeFragmentResumeCallback baseContinuation,
        void* continuationContext,
        RuntimeFragmentDispatchOutcome& outcome,
        std::string& error) const;

private:
    friend bool makeRuntimeFragmentExecutionContext(
        const RuntimeFragmentBindingSet&,
        RuntimeFragmentExecutionContext&, std::string&);
    std::shared_ptr<const RuntimeFragmentExecutionContextState> state_;
};

bool makeRuntimeFragmentExecutionContext(
    const RuntimeFragmentBindingSet& bindings,
    RuntimeFragmentExecutionContext& output,
    std::string& error);

} // namespace luna::runtime
