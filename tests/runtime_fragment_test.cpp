#include "runtime/RuntimeFragment.h"
#include "runtime/RuntimeDescriptorABI.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

// Test-only ordinary C++ allocation counter, enabled only across synchronous
// dispatch. It is not a timer or a claim about plugin/OS/aligned allocations.
thread_local bool countDispatchAllocations = false;
thread_local size_t dispatchAllocations = 0;

} // namespace

void* operator new(std::size_t size) {
    if (void* storage = std::malloc(size == 0 ? 1 : size)) {
        if (countDispatchAllocations) ++dispatchAllocations;
        return storage;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
// libstdc++ stable_sort uses nothrow allocation for its temporary buffer.
// Replace the entire ordinary allocation family: leaving the sanitizer's
// nothrow new paired with our free-based delete is an alloc/dealloc mismatch.
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try { return ::operator new(size); }
    catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try { return ::operator new[](size); }
    catch (...) { return nullptr; }
}
void operator delete(void* storage) noexcept { std::free(storage); }
void operator delete[](void* storage) noexcept { std::free(storage); }
void operator delete(void* storage, std::size_t) noexcept { std::free(storage); }
void operator delete[](void* storage, std::size_t) noexcept { std::free(storage); }
void operator delete(void* storage, const std::nothrow_t&) noexcept { std::free(storage); }
void operator delete[](void* storage, const std::nothrow_t&) noexcept { std::free(storage); }

namespace {

int fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

struct FactoryArguments {
    int value = 0;
};

struct Environment {
    int value = 0;
};

struct Activation {
    int observed = 0;
};

struct LeaseProbe {
    explicit LeaseProbe(std::atomic<unsigned>& destructions)
        : destructions(destructions) {}
    ~LeaseProbe() { ++destructions; }
    std::atomic<unsigned>& destructions;
};

struct BorrowedEnvironmentProbe {
    BorrowedEnvironmentProbe(
        int value, std::atomic<unsigned>& destructions)
        : environment{value}, destructions(destructions) {}
    ~BorrowedEnvironmentProbe() { ++destructions; }
    Environment environment;
    std::atomic<unsigned>& destructions;
};

std::atomic<unsigned> factoryCalls{0};
std::atomic<unsigned> destroyCalls{0};
unsigned payloadProbeExecutions = 0;
unsigned payloadProbeDestructions = 0;
bool repeatResumeProbe = true;
std::array<int32_t, 3> repeatedResumeResults{};
std::vector<int>* activeChainTrace = nullptr;

int32_t resumeActivation(void* context) {
    ++*static_cast<unsigned*>(context);
    return LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1;
}

// Adversarial payload tests must never dereference intentionally unaligned
// storage, even when running against the old, permissive implementation.
void executePayloadProbe(void*, void*) {
    ++payloadProbeExecutions;
}

int32_t createPayloadProbe(const void* arguments, void** outputEnvironment) {
    *outputEnvironment = const_cast<void*>(arguments);
    return 0;
}

void destroyPayloadProbe(void*) {
    ++payloadProbeDestructions;
}

int32_t createEnvironment(
    const void* arguments, void** outputEnvironment) {
    ++factoryCalls;
    if (!arguments || !outputEnvironment) return 1;
    const auto* input = static_cast<const FactoryArguments*>(arguments);
    *outputEnvironment = new Environment{input->value};
    return 0;
}

void destroyEnvironment(void* environment) {
    ++destroyCalls;
    delete static_cast<Environment*>(environment);
}

void executeFragment(void* environment, void* activation) {
    auto* state = static_cast<Environment*>(environment);
    auto* call = static_cast<Activation*>(activation);
    call->observed = state ? state->value : 7;
}

void executeResumingFragment(void*, void* activation) {
    const auto* argument = static_cast<const int*>(
        luna_runtime_fragment_activation_arguments_v1(
            activation,
            "slot:pipeline", "contract:slot-pipeline",
            "layout:pipeline-arguments", sizeof(int), alignof(int)));
    if (argument && *argument == 42)
        luna_runtime_fragment_activation_resume_v1(activation);
}

// Intentionally ignores resume errors to test the outer dispatch boundary.
void executeRepeatedResumeFragment(void*, void* activation) {
    repeatedResumeResults[0] = luna_runtime_fragment_activation_resume_v1(activation);
    if (repeatResumeProbe) {
        repeatedResumeResults[1] = luna_runtime_fragment_activation_resume_v1(activation);
        repeatedResumeResults[2] = luna_runtime_fragment_activation_resume_v1(activation);
    }
}

struct CountedControlProbe {
    unsigned calls = 0;
    int32_t result = LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1;
};

int32_t countedControl(void* context) {
    auto& probe = *static_cast<CountedControlProbe*>(context);
    ++probe.calls;
    return probe.result;
}

struct ReentrantResumeProbe {
    void* activation = nullptr;
    unsigned calls = 0;
    int32_t nestedResult = 0;
};

int32_t reentrantResume(void* context) {
    auto& probe = *static_cast<ReentrantResumeProbe*>(context);
    ++probe.calls;
    probe.nestedResult = luna_runtime_fragment_activation_resume_v1(probe.activation);
    return LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1;
}

void executeFirstChainFragment(void*, void* activation) {
    if (activeChainTrace) activeChainTrace->push_back(1);
    if (luna_runtime_fragment_activation_resume_v1(activation) ==
        LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1) {
        if (activeChainTrace) activeChainTrace->push_back(4);
    }
}

void executeSecondChainFragment(void*, void* activation) {
    if (activeChainTrace) activeChainTrace->push_back(2);
    if (luna_runtime_fragment_activation_resume_v1(activation) ==
        LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1) {
        if (activeChainTrace) activeChainTrace->push_back(3);
    }
}

int32_t recordChainBase(void* context) {
    static_cast<std::vector<int>*>(context)->push_back(0);
    return LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1;
}

int32_t recordEscapingChainBase(void* context) {
    static_cast<std::vector<int>*>(context)->push_back(0);
    return LUNA_RUNTIME_FRAGMENT_CONTINUATION_ESCAPED_V1;
}

struct NestedDispatchProbe {
    const void* executionContext;
    const luna::runtime::RuntimeSlotRequirement* slot;
    const luna::runtime::RuntimeFragmentArguments* arguments;
    std::vector<int>* trace;
    bool entered = false;
    bool escapeInner = false;
    int32_t nestedStatus = LUNA_RUNTIME_FRAGMENT_DISPATCH_INVALID_CONTEXT_V1;
};

int32_t nestedChainBase(void* context) {
    auto& probe = *static_cast<NestedDispatchProbe*>(context);
    probe.trace->push_back(0);
    if (probe.entered)
        return probe.escapeInner
            ? LUNA_RUNTIME_FRAGMENT_CONTINUATION_ESCAPED_V1
            : LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1;
    probe.entered = true;
    probe.nestedStatus = luna_runtime_fragment_dispatch_v1(
        probe.executionContext, probe.slot->slotId.c_str(),
        probe.slot->contractId.c_str(), probe.arguments->layoutId.c_str(),
        probe.arguments->size, probe.arguments->alignment,
        probe.arguments->data, nestedChainBase, &probe);
    return probe.nestedStatus;
}

using Runtime = luna::runtime::MoonRuntime;

bool stageFragment(
    Runtime& runtime, const std::shared_ptr<const void>& lease,
    const LunaRuntimeFragmentDescriptorV1* descriptor,
    Runtime::PinnedBinding& binding, std::string& error,
    uint32_t declarationKind = LUNA_RUNTIME_DECLARATION_FRAGMENT_V1,
    uint32_t flags =
        luna::runtime::GenerationBindingFragmentExecutable |
        luna::runtime::GenerationBindingPublicControl) {
    luna::runtime::GenerationStagingRequest request{
        "org.luna.runtime.fragment", std::string(64, 'a'), lease};
    Runtime::StagedGeneration staged;
    if (!runtime.stage(
            request,
            [](const auto&, std::string&) { return true; },
            [=](const auto&, auto& bindings, std::string&) {
                bindings.push_back({
                    descriptor->fragment_id,
                    descriptor->fragment_contract_id,
                    descriptor, declarationKind, flags});
                return true;
            },
            {}, staged, error))
        return false;
    Runtime::PinnedGeneration loaded;
    if (!runtime.loadOnce(staged, loaded, error)) return false;
    const luna::runtime::GenerationBindingRequirement requirement{
        descriptor->fragment_id, descriptor->fragment_contract_id,
        declarationKind, flags};
    binding = loaded.find(requirement);
    return static_cast<bool>(binding);
}

struct ScopedActivationProbe {
    const luna::runtime::RuntimeFragmentBindingSet* bindings = nullptr;
    const luna::runtime::RuntimeSlotRequirement* slot = nullptr;
    const luna::runtime::RuntimeFragmentArguments* arguments = nullptr;
    std::array<void*, 256> live{};
    size_t depth = 0;
    unsigned entered = 0;
    unsigned returned = 0;
    unsigned baseCalls = 0;
    bool valid = true;
    bool nest = false;
    bool nested = false;
    bool throwHandler = false;
    size_t throwDepth = 0;
    int32_t control = LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1;

    bool matches(void* activation) const {
        return luna_runtime_fragment_activation_arguments_v1(
            activation, slot->slotId.c_str(), slot->contractId.c_str(),
            arguments->layoutId.c_str(), arguments->size,
            arguments->alignment) == arguments->data;
    }
};

ScopedActivationProbe* activeScopedActivationProbe = nullptr;

void executeScopedActivationProbe(void*, void* activation) {
    auto& probe = *activeScopedActivationProbe;
    ++probe.entered;
    if (probe.depth >= probe.live.size()) {
        probe.valid = false;
        return;
    }
    for (size_t index = 0; index < probe.depth; ++index)
        probe.valid &= probe.live[index] != activation;
    probe.live[probe.depth++] = activation;
    probe.valid &= probe.matches(activation);
    probe.valid &= luna_runtime_fragment_activation_arguments_v1(
        activation, "slot:wrong", probe.slot->contractId.c_str(),
        probe.arguments->layoutId.c_str(), probe.arguments->size,
        probe.arguments->alignment) == nullptr;
    const int32_t result = luna_runtime_fragment_activation_resume_v1(activation);
    probe.valid &= probe.matches(activation);
    --probe.depth;
    ++probe.returned;
    if (probe.throwHandler && probe.depth == probe.throwDepth)
        throw std::runtime_error("scoped activation handler probe");
    // A failed downstream handler is deliberately ignored here, as native
    // execute has no result channel. Outer dispatch must retain that failure.
    if (!probe.throwHandler) probe.valid &= result == probe.control;
}

int32_t scopedActivationBase(void* context) {
    auto& probe = *static_cast<ScopedActivationProbe*>(context);
    ++probe.baseCalls;
    for (size_t index = 0; index < probe.depth; ++index)
        probe.valid &= probe.matches(probe.live[index]);
    if (probe.nest && !probe.nested) {
        probe.nested = true;
        std::string error;
        auto outcome = luna::runtime::RuntimeFragmentDispatchOutcome::Completed;
        probe.valid &= probe.bindings->dispatchWithOutcome(
            *probe.slot, *probe.arguments, scopedActivationBase, &probe,
            outcome, error) && error.empty();
        probe.valid &= (outcome ==
            luna::runtime::RuntimeFragmentDispatchOutcome::ContinuationEscaped) ==
            (probe.control == LUNA_RUNTIME_FRAGMENT_CONTINUATION_ESCAPED_V1);
        for (size_t index = 0; index < probe.depth; ++index)
            probe.valid &= probe.matches(probe.live[index]);
    }
    return probe.control;
}

int testScopedActivation(const LunaRuntimeFragmentDescriptorV1& prototype) {
    // Exercise nothrow allocation with both ordinary release (the STL path)
    // and nothrow cleanup release, even on libraries whose sort uses no buffer.
    dispatchAllocations = 0;
    countDispatchAllocations = true;
    void* scalar = ::operator new(64, std::nothrow);
    void* array = ::operator new[](64, std::nothrow);
    void* scalarCleanup = ::operator new(64, std::nothrow);
    void* arrayCleanup = ::operator new[](64, std::nothrow);
    countDispatchAllocations = false;
    const bool counted = scalar && array && scalarCleanup && arrayCleanup &&
        dispatchAllocations == 4;
    ::operator delete(scalar, std::size_t{64});
    ::operator delete[](array);
    ::operator delete(scalarCleanup, std::nothrow);
    ::operator delete[](arrayCleanup, std::nothrow);
    if (!counted) return fail("ordinary nothrow allocation counter is not paired or counted");

    // Deliberately exceed typical SSO capacities; short names would hide the
    // former per-handler identity/carrier copies from allocation counting.
    const std::string slotId = "slot:" + std::string(128, 's');
    const std::string contractId = "contract:" + std::string(128, 'c');
    const std::string layoutId = "layout:" + std::string(128, 'l');
    auto descriptor = prototype;
    descriptor.flags = LUNA_RUNTIME_FRAGMENT_CAPTURE_FREE_V1;
    descriptor.slot_id = slotId.c_str();
    descriptor.slot_contract_id = contractId.c_str();
    descriptor.slot_arguments_layout_id = layoutId.c_str();
    descriptor.slot_arguments_size = sizeof(int);
    descriptor.slot_arguments_alignment = alignof(int);
    descriptor.factory_contract_id = "";
    descriptor.environment_layout_id = "layout:empty";
    descriptor.environment_size = 0;
    descriptor.environment_alignment = 1;
    descriptor.factory = nullptr;
    descriptor.destroy = nullptr;
    descriptor.execute = executeScopedActivationProbe;
    std::string error;
    Runtime runtime;
    Runtime::PinnedBinding binding;
    if (!stageFragment(runtime, std::make_shared<int>(1), &descriptor, binding, error))
        return fail("scoped activation fixture did not stage");
    const luna::runtime::RuntimeSlotRequirement slot{slotId, contractId};
    int payload = 42;
    const luna::runtime::RuntimeFragmentArguments arguments{
        layoutId, sizeof(payload), alignof(int), &payload};
    size_t oneAllocations = 0;
    for (unsigned chainLength : {1u, 4u, 64u}) {
        std::vector<luna::runtime::RuntimeFragmentRef> references;
        for (unsigned index = 0; index < chainLength; ++index) {
            luna::runtime::RuntimeFragmentRef reference;
            if (!luna::runtime::makeOwnedRuntimeFragmentRef(
                    binding, slot, {"", nullptr}, reference, error))
                return fail("scoped activation reference did not bind");
            references.push_back(std::move(reference));
        }
        luna::runtime::RuntimeFragmentBindingSet bindings;
        if (!luna::runtime::makeRuntimeFragmentChainBindingSet(
                std::move(references), bindings, error))
            return fail("scoped activation chain did not initialize");
        ScopedActivationProbe probe;
        probe.bindings = &bindings;
        probe.slot = &slot;
        probe.arguments = &arguments;
        activeScopedActivationProbe = &probe;
        dispatchAllocations = 0;
        countDispatchAllocations = true;
        const bool dispatched = bindings.dispatch(
            slot, arguments, scopedActivationBase, &probe, error);
        countDispatchAllocations = false;
        activeScopedActivationProbe = nullptr;
        if (!dispatched || !error.empty() || !probe.valid || probe.depth != 0 ||
            probe.entered != chainLength || probe.returned != chainLength ||
            probe.baseCalls != 1)
            return fail("scoped activation lost arguments or distinct live state");
        if (chainLength == 1) oneAllocations = dispatchAllocations;
        else if (dispatchAllocations != oneAllocations)
            return fail("synchronous activation allocations grew with chain length");

        // Keep a 64-handler outer chain suspended while the same Slot runs
        // another 64 handlers. All 128 live activation addresses must differ.
        for (int32_t control : {LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1,
                                LUNA_RUNTIME_FRAGMENT_CONTINUATION_ESCAPED_V1}) {
            probe.depth = 0;
            probe.entered = probe.returned = probe.baseCalls = 0;
            probe.nest = true;
            probe.nested = false;
            probe.control = control;
            activeScopedActivationProbe = &probe;
            auto outcome = luna::runtime::RuntimeFragmentDispatchOutcome::Completed;
            const bool nested = bindings.dispatchWithOutcome(
                slot, arguments, scopedActivationBase, &probe, outcome, error);
            activeScopedActivationProbe = nullptr;
            if (!nested || !error.empty() || !probe.valid || probe.depth != 0 ||
                probe.entered != 2 * chainLength ||
                probe.returned != 2 * chainLength || probe.baseCalls != 2 ||
                (outcome == luna::runtime::RuntimeFragmentDispatchOutcome::ContinuationEscaped) !=
                    (control == LUNA_RUNTIME_FRAGMENT_CONTINUATION_ESCAPED_V1))
                return fail("nested scoped activation state or escape was not isolated");
        }
        probe.nest = false;
        probe.control = LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1;
        probe.throwHandler = true;
        probe.throwDepth = chainLength - 1;
        activeScopedActivationProbe = &probe;
        const bool threw = bindings.dispatch(
            slot, arguments, scopedActivationBase, &probe, error);
        const bool diagnosed = error.find("scoped activation handler probe") != std::string::npos;
        probe.throwHandler = false;
        const bool recovered = bindings.dispatch(
            slot, arguments, scopedActivationBase, &probe, error);
        activeScopedActivationProbe = nullptr;
        if (threw || !diagnosed || !recovered || !error.empty() || !probe.valid || probe.depth != 0)
            return fail("scoped activation handler failure poisoned a fresh dispatch");
    }

    // Public activation still owns identity/carrier values across caller
    // mutation and moves; only payload storage keeps the host lifetime duty.
    luna::runtime::RuntimeFragmentActivation activation;
    unsigned resumes = 0;
    {
        auto mutableSlot = slot;
        auto mutableArguments = arguments;
        if (!luna::runtime::makeRuntimeFragmentActivation(
                mutableSlot, mutableArguments, resumeActivation, &resumes,
                activation, error))
            return fail("owning activation fixture did not initialize");
        mutableSlot.slotId = mutableSlot.contractId = "changed";
        mutableArguments.layoutId = "changed";
        mutableArguments.data = nullptr;
    }
    luna::runtime::RuntimeFragmentActivation moved(std::move(activation));
    luna::runtime::RuntimeFragmentActivation assigned;
    assigned = std::move(moved);
    if (activation || moved || !assigned || assigned.resumed() ||
        luna_runtime_fragment_activation_arguments_v1(
            assigned.opaque(), slotId.c_str(), contractId.c_str(),
            layoutId.c_str(), sizeof(payload), alignof(int)) != &payload ||
        luna_runtime_fragment_activation_resume_v1(assigned.opaque()) !=
            LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1 ||
        !assigned.resumed() || resumes != 1)
        return fail("public activation stopped owning identity/carrier values");
    return 0;
}

struct DispatchLifetimeProbe {
    // Counters outlive handle cleanup, including an early fixture failure.
    std::atomic<unsigned> environmentDestructions{0};
    std::atomic<unsigned> moduleDestructions{0};
    luna::runtime::RuntimeFragmentBindingSet bindings;
    luna::runtime::RuntimeFragmentExecutionContext context;
    luna::runtime::RuntimeFragmentBindingSet replacement;
    luna::runtime::RuntimeFragmentExecutionContext replacementContext;
    unsigned handlers = 0;
    unsigned returnedHandlers = 0;
    unsigned baseCalls = 0;
    bool releaseInHandler = false;
    bool retainedThroughout = true;
    bool environmentBeforeModule = false;
    int32_t control = LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1;

    void releasePublishedHandles() {
        bindings = replacement;
        context = replacementContext;
        checkRetained();
    }
    void checkRetained() {
        retainedThroughout &= environmentDestructions.load() == 0 &&
                              moduleDestructions.load() == 0;
    }
};

DispatchLifetimeProbe* activeLifetimeProbe = nullptr;

struct LifetimeEnvironment {
    explicit LifetimeEnvironment(std::atomic<unsigned>& destructions)
        : destructions(destructions) {}
    ~LifetimeEnvironment() { ++destructions; }
    std::atomic<unsigned>& destructions;
};

int32_t createLifetimeEnvironment(const void* arguments, void** output) {
    auto& probe = *const_cast<DispatchLifetimeProbe*>(
        static_cast<const DispatchLifetimeProbe*>(arguments));
    *output = new LifetimeEnvironment(probe.environmentDestructions);
    return 0;
}

void destroyLifetimeEnvironment(void* environment) {
    delete static_cast<LifetimeEnvironment*>(environment);
}

void executeLifetimeFragment(void*, void* activation) {
    auto& probe = *activeLifetimeProbe;
    ++probe.handlers;
    if (probe.releaseInHandler && probe.handlers == 1)
        probe.releasePublishedHandles();
    // Fail the old implementation without traversing a freed chain or reading
    // a freed environment. Lifetime evidence comes from external counters.
    if (!probe.retainedThroughout) return;
    luna_runtime_fragment_activation_resume_v1(activation);
    probe.checkRetained();
    ++probe.returnedHandlers;
}

int32_t releaseLifetimeBase(void* context) {
    auto& probe = *static_cast<DispatchLifetimeProbe*>(context);
    ++probe.baseCalls;
    if (!probe.releaseInHandler) probe.releasePublishedHandles();
    if (probe.control == -99) throw std::runtime_error("lifetime continuation probe");
    return probe.control;
}

int testDispatchLifetime(const LunaRuntimeFragmentDescriptorV1& original) {
    using namespace luna::runtime;
    auto descriptor = original;
    descriptor.factory_contract_id = "contract:lifetime-factory";
    descriptor.environment_layout_id = "layout:lifetime-environment";
    descriptor.environment_size = sizeof(LifetimeEnvironment);
    descriptor.environment_alignment = alignof(LifetimeEnvironment);
    descriptor.factory = createLifetimeEnvironment;
    descriptor.destroy = destroyLifetimeEnvironment;
    descriptor.execute = executeLifetimeFragment;
    const RuntimeSlotRequirement slot{descriptor.slot_id, descriptor.slot_contract_id};
    int argument = 42;
    const RuntimeFragmentArguments arguments{
        descriptor.slot_arguments_layout_id, sizeof(argument), alignof(int), &argument};
    // One and chain; owned and borrowed environments; BindingSet, C++ context,
    // and C ABI; release before resume or in base; clear or replace with None;
    // completed, escaped, invalid, and throwing continuation results.
    for (unsigned chainLength : {1u, 2u})
    for (bool borrowed : {false, true})
    for (unsigned entry : {0u, 1u, 2u})
    for (bool releaseInHandler : {false, true})
    for (bool replace : {false, true})
    for (int32_t control : {0, 1, 99, -99}) {
        DispatchLifetimeProbe probe;
        probe.releaseInHandler = releaseInHandler;
        probe.control = control;
        std::string error;
        if (replace &&
            (!makeRuntimeFragmentBindingSet({}, probe.replacement, error) ||
             !makeRuntimeFragmentExecutionContext(
                 probe.replacement, probe.replacementContext, error)))
            return fail("dispatch lifetime replacement did not initialize");
        {
            Runtime runtime;
            auto lease = std::shared_ptr<const void>(new int(1),
                [&probe, chainLength](const void* storage) {
                    probe.environmentBeforeModule =
                        probe.environmentDestructions.load() == chainLength;
                    ++probe.moduleDestructions;
                    delete static_cast<const int*>(storage);
                });
            Runtime::PinnedBinding binding;
            if (!stageFragment(runtime, lease, &descriptor, binding, error))
                return fail("dispatch lifetime generation did not stage");
            std::vector<RuntimeFragmentRef> references;
            for (unsigned index = 0; index < chainLength; ++index) {
                RuntimeFragmentRef reference;
                if (borrowed) {
                    auto environment = std::make_shared<LifetimeEnvironment>(
                        probe.environmentDestructions);
                    if (!makeBorrowedRuntimeFragmentRef(binding, slot,
                            {descriptor.environment_layout_id,
                             descriptor.environment_size, descriptor.environment_alignment,
                             environment.get(), environment}, reference, error))
                        return fail("dispatch lifetime borrowed environment did not bind");
                } else if (!makeOwnedRuntimeFragmentRef(binding, slot,
                               {descriptor.factory_contract_id, &probe}, reference, error)) {
                    return fail("dispatch lifetime owned environment did not bind");
                }
                references.push_back(std::move(reference));
            }
            if (!makeRuntimeFragmentChainBindingSet(
                    std::move(references), probe.bindings, error))
                return fail("dispatch lifetime BindingSet did not initialize");
            if (entry != 0) {
                if (!makeRuntimeFragmentExecutionContext(
                        probe.bindings, probe.context, error))
                    return fail("dispatch lifetime context did not initialize");
                probe.bindings = {};
            }
        } // The published handle is now the only owner of environments/generation.
        activeLifetimeProbe = &probe;
        RuntimeFragmentDispatchOutcome outcome = RuntimeFragmentDispatchOutcome::Completed;
        bool succeeded;
        int32_t status = LUNA_RUNTIME_FRAGMENT_DISPATCH_SUCCESS_V1;
        if (entry == 2) {
            status = luna_runtime_fragment_dispatch_v1(
                probe.context.opaque(), slot.slotId.c_str(), slot.contractId.c_str(),
                arguments.layoutId.c_str(), arguments.size, arguments.alignment,
                arguments.data, releaseLifetimeBase, &probe);
            succeeded = status >= 0;
            outcome = status == LUNA_RUNTIME_FRAGMENT_DISPATCH_CONTINUATION_ESCAPED_V1
                ? RuntimeFragmentDispatchOutcome::ContinuationEscaped
                : RuntimeFragmentDispatchOutcome::Completed;
        } else if (entry == 1) {
            succeeded = probe.context.dispatchWithOutcome(
                slot, arguments, releaseLifetimeBase, &probe, outcome, error);
        } else {
            succeeded = probe.bindings.dispatchWithOutcome(
                slot, arguments, releaseLifetimeBase, &probe, outcome, error);
        }
        activeLifetimeProbe = nullptr;
        if (!probe.retainedThroughout || probe.handlers != chainLength ||
            probe.returnedHandlers != chainLength || probe.baseCalls != 1)
            return fail("dispatch did not pin its snapshot until every handler returned");
        if (succeeded != (control == 0 || control == 1) ||
            outcome != (control == 1 ? RuntimeFragmentDispatchOutcome::ContinuationEscaped
                                    : RuntimeFragmentDispatchOutcome::Completed))
            return fail("dispatch lifetime pin changed continuation control results");
        if (control == 99 || control == -99) {
            if ((entry == 2 && status != LUNA_RUNTIME_FRAGMENT_DISPATCH_EXECUTION_FAILED_V1) ||
                (entry != 2 && error.find(control == 99 ? "invalid control result"
                                                       : "lifetime continuation probe") == std::string::npos))
                return fail("dispatch lifetime pin lost the continuation failure diagnostic");
        }
        if (probe.environmentDestructions.load() != chainLength ||
            probe.moduleDestructions.load() != 1 || !probe.environmentBeforeModule)
            return fail("dispatch snapshot leaked or released generation before environments");
        if (replace) {
            unsigned resumes = 0;
            const bool ranNone = entry == 0
                ? probe.bindings.dispatch(slot, arguments, resumeActivation, &resumes, error)
                : probe.context.dispatch(slot, arguments, resumeActivation, &resumes, error);
            if (!ranNone || resumes != 1 || probe.handlers != chainLength)
                return fail("published replacement did not apply to the next invocation");
        }
    }
    return 0;
}

enum class FactoryLifetimeResult {
    Success, FailedWithEnvironment, FailedWithoutEnvironment,
    NullSuccess, UnalignedEnvironment, StandardException, UnknownException,
};

struct FactoryLifetimeProbe {
    unsigned moduleDestructions = 0;
    unsigned environmentDestructions = 0;
    unsigned factoryCalls = 0;
    bool factoryRetainedGeneration = true;
    bool cleanupRetainedGeneration = true;
    bool environmentBeforeModule = false;
    bool createsEnvironment = true;
    FactoryLifetimeResult result = FactoryLifetimeResult::Success;
    // Static fixture storage/callback code stays valid even on the unfixed
    // implementation. External counters detect premature module release.
    alignas(16) std::array<unsigned char, 16> storage{};
    Runtime::PinnedBinding published;
    Runtime::PinnedBinding replacement;
};

FactoryLifetimeProbe* activeFactoryLifetimeProbe = nullptr;

int32_t createFactoryLifetimeEnvironment(const void* arguments, void** output) {
    auto& probe = *const_cast<FactoryLifetimeProbe*>(
        static_cast<const FactoryLifetimeProbe*>(arguments));
    ++probe.factoryCalls;
    probe.published = probe.replacement;
    probe.factoryRetainedGeneration &= probe.moduleDestructions == 0;
    if (probe.createsEnvironment) {
        *output = probe.storage.data() +
            (probe.result == FactoryLifetimeResult::UnalignedEnvironment ? 1 : 0);
    }
    // Defensive exception paths are exercised here; ABI callbacks are still
    // required not to unwind across the C boundary.
    if (probe.result == FactoryLifetimeResult::StandardException)
        throw std::runtime_error("factory lifetime probe");
    if (probe.result == FactoryLifetimeResult::UnknownException) throw 42;
    return probe.result == FactoryLifetimeResult::FailedWithEnvironment ||
           probe.result == FactoryLifetimeResult::FailedWithoutEnvironment ? 1 : 0;
}

void destroyFactoryLifetimeEnvironment(void*) {
    auto& probe = *activeFactoryLifetimeProbe;
    probe.cleanupRetainedGeneration &= probe.moduleDestructions == 0;
    ++probe.environmentDestructions;
}

int testFactoryLifetime(const LunaRuntimeFragmentDescriptorV1& original) {
    using namespace luna::runtime;
    auto descriptor = original;
    descriptor.factory_contract_id = "contract:factory-lifetime";
    descriptor.environment_layout_id = "layout:factory-lifetime";
    descriptor.environment_size = 8;
    descriptor.environment_alignment = 8;
    descriptor.factory = createFactoryLifetimeEnvironment;
    descriptor.destroy = destroyFactoryLifetimeEnvironment;
    descriptor.execute = executePayloadProbe;
    auto replacementDescriptor = descriptor;
    replacementDescriptor.fragment_id = "fragment:factory-replacement";
    replacementDescriptor.fragment_contract_id = "contract:factory-replacement";
    const RuntimeSlotRequirement slot{descriptor.slot_id, descriptor.slot_contract_id};
    for (bool replace : {false, true})
    for (FactoryLifetimeResult result : {
             FactoryLifetimeResult::Success,
             FactoryLifetimeResult::FailedWithEnvironment,
             FactoryLifetimeResult::FailedWithoutEnvironment,
             FactoryLifetimeResult::NullSuccess,
             FactoryLifetimeResult::UnalignedEnvironment,
             FactoryLifetimeResult::StandardException,
             FactoryLifetimeResult::UnknownException}) {
        FactoryLifetimeProbe probe;
        probe.result = result;
        probe.createsEnvironment = result != FactoryLifetimeResult::FailedWithoutEnvironment &&
                                   result != FactoryLifetimeResult::NullSuccess;
        std::string error;
        if (replace) {
            Runtime runtime;
            if (!stageFragment(runtime, std::make_shared<int>(1),
                    &replacementDescriptor, probe.replacement, error))
                return fail("factory lifetime replacement did not stage");
        }
        {
            Runtime runtime;
            auto lease = std::shared_ptr<const void>(new int(1),
                [&probe](const void* storage) {
                    probe.environmentBeforeModule = probe.environmentDestructions ==
                        (probe.createsEnvironment ? 1u : 0u);
                    ++probe.moduleDestructions;
                    delete static_cast<const int*>(storage);
                });
            if (!stageFragment(runtime, lease, &descriptor, probe.published, error))
                return fail("factory lifetime original generation did not stage");
        }
        const auto originalGeneration = probe.published.generationId();
        RuntimeFragmentRef reference;
        activeFactoryLifetimeProbe = &probe;
        const bool constructed = makeOwnedRuntimeFragmentRef(probe.published, slot,
            {descriptor.factory_contract_id, &probe}, reference, error);
        const bool success = result == FactoryLifetimeResult::Success;
        const bool correctReference = success
            ? reference && reference.generationId() == originalGeneration &&
              reference.fragmentId() == descriptor.fragment_id &&
              reference.fragmentContractId() == descriptor.fragment_contract_id &&
              reference.descriptor() == &descriptor && reference.environment() == probe.storage.data() &&
              probe.moduleDestructions == 0 && probe.environmentDestructions == 0 && error.empty()
            : !reference && reference.generationId() == 0 && !error.empty();
        reference.reset();
        activeFactoryLifetimeProbe = nullptr;
        if (constructed != success || !correctReference || probe.factoryCalls != 1 ||
            !probe.factoryRetainedGeneration || !probe.cleanupRetainedGeneration)
            return fail("Fragment factory did not retain its original validated generation");
        if (probe.environmentDestructions != (probe.createsEnvironment ? 1u : 0u) ||
            probe.moduleDestructions != 1 || !probe.environmentBeforeModule)
            return fail("Fragment factory failure leaked or released module before cleanup");
        if ((replace && probe.published.symbolId() != replacementDescriptor.fragment_id) ||
            (!replace && probe.published))
            return fail("Fragment construction overwrote the host's changed binding handle");
        if (result == FactoryLifetimeResult::StandardException &&
            error.find("factory lifetime probe") == std::string::npos)
            return fail("Fragment factory lost its exception diagnostic");
        if (result == FactoryLifetimeResult::UnknownException &&
            error.find("factory threw") == std::string::npos)
            return fail("Fragment factory lost its unknown exception diagnostic");
    }
    return 0;
}

enum class CleanupOperation { Reset, MoveAssign, Destruction };
enum class CleanupAction { Reset, Replace };

struct CleanupReentryProbe {
    std::array<unsigned, 3> environmentCalls{};
    std::array<unsigned, 3> environmentCompletions{};
    std::array<unsigned, 3> moduleDestructions{};
    bool retainedDuringCallback = true;
    bool environmentBeforeModule = true;
    bool observedExpectedState = true;
    bool entered = false;
    CleanupOperation operation = CleanupOperation::Reset;
    CleanupAction action = CleanupAction::Reset;
    luna::runtime::RuntimeFragmentRef* target = nullptr;
    luna::runtime::RuntimeFragmentRef* replacement = nullptr;
    const char* incomingId = nullptr;
};

struct CleanupEnvironment {
    CleanupReentryProbe* probe;
    unsigned index;
};

void cleanupReentryEnvironment(void* storage) {
    const auto environment = *static_cast<CleanupEnvironment*>(storage);
    auto& probe = *environment.probe;
    ++probe.environmentCalls[environment.index];
    if (environment.index == 0 && !probe.entered && probe.target) {
        probe.entered = true;
        probe.retainedDuringCallback &= probe.moduleDestructions[0] == 0;
        probe.observedExpectedState &= probe.operation == CleanupOperation::MoveAssign
            ? *probe.target && probe.target->fragmentId() == probe.incomingId
            : !*probe.target && probe.target->generationId() == 0 &&
              probe.target->descriptor() == nullptr && probe.target->environment() == nullptr;
        if (probe.action == CleanupAction::Reset)
            probe.target->reset();
        else
            *probe.target = std::move(*probe.replacement);
        probe.retainedDuringCallback &= probe.moduleDestructions[0] == 0;
    }
    ++probe.environmentCompletions[environment.index];
}

int testCleanupReentry(const LunaRuntimeFragmentDescriptorV1& original) {
    using namespace luna::runtime;
    std::array<LunaRuntimeFragmentDescriptorV1, 3> descriptors{original, original, original};
    const std::array<const char*, 3> ids{
        "fragment:cleanup-old", "fragment:cleanup-incoming", "fragment:cleanup-replacement"};
    for (unsigned index = 0; index < descriptors.size(); ++index) {
        auto& descriptor = descriptors[index];
        descriptor.fragment_id = ids[index];
        descriptor.fragment_contract_id = ids[index];
        descriptor.factory_contract_id = "contract:cleanup-factory";
        descriptor.environment_layout_id = "layout:cleanup-environment";
        descriptor.environment_size = sizeof(CleanupEnvironment);
        descriptor.environment_alignment = alignof(CleanupEnvironment);
        descriptor.factory = createPayloadProbe;
        descriptor.destroy = cleanupReentryEnvironment;
        descriptor.execute = executePayloadProbe;
    }
    const RuntimeSlotRequirement slot{original.slot_id, original.slot_contract_id};
    for (bool borrowed : {false, true})
    for (CleanupOperation operation : {
             CleanupOperation::Reset, CleanupOperation::MoveAssign, CleanupOperation::Destruction})
    for (CleanupAction action : {CleanupAction::Reset, CleanupAction::Replace}) {
        // Rebinding a destroying object is not supported. Nested reset is
        // harmless, but callbacks must not resurrect an object in its destructor.
        if (operation == CleanupOperation::Destruction && action == CleanupAction::Replace)
            continue;
        CleanupReentryProbe probe;
        probe.operation = operation;
        probe.action = action;
        probe.incomingId = ids[1];
        std::array<CleanupEnvironment, 3> environments{{{&probe, 0}, {&probe, 1}, {&probe, 2}}};
        auto target = std::make_unique<RuntimeFragmentRef>();
        RuntimeFragmentRef incoming;
        RuntimeFragmentRef replacement;
        const std::array<RuntimeFragmentRef*, 3> references{target.get(), &incoming, &replacement};
        std::string error;
        for (unsigned index = 0; index < references.size(); ++index) {
            Runtime runtime;
            auto lease = std::shared_ptr<const void>(new int(1),
                [&probe, index](const void* storage) {
                    probe.environmentBeforeModule &= probe.environmentCompletions[index] == 1;
                    ++probe.moduleDestructions[index];
                    delete static_cast<const int*>(storage);
                });
            Runtime::PinnedBinding binding;
            if (!stageFragment(runtime, lease, &descriptors[index], binding, error))
                return fail("cleanup reentry generation did not stage");
            if (borrowed) {
                auto environmentLease = std::shared_ptr<const void>(&environments[index],
                    [](const void* storage) {
                        cleanupReentryEnvironment(const_cast<void*>(storage));
                    });
                if (!makeBorrowedRuntimeFragmentRef(binding, slot,
                        {descriptors[index].environment_layout_id,
                         sizeof(CleanupEnvironment), alignof(CleanupEnvironment),
                         &environments[index], environmentLease}, *references[index], error))
                    return fail("cleanup reentry borrowed environment did not bind");
            } else if (!makeOwnedRuntimeFragmentRef(binding, slot,
                           {descriptors[index].factory_contract_id, &environments[index]},
                           *references[index], error)) {
                return fail("cleanup reentry owned environment did not bind");
            }
        }
        probe.target = target.get();
        probe.replacement = &replacement;
        const auto initialGeneration = target->generationId();
        auto& self = *target;
        auto& alias = *references[0];
        self = std::move(alias);
        if (!self || self.fragmentId() != ids[0] || self.generationId() != initialGeneration ||
            self.environment() != &environments[0] || probe.environmentCalls[0] != 0 ||
            probe.moduleDestructions[0] != 0) {
            probe.target = nullptr;
            return fail("self move assignment changed a live Fragment reference");
        }
        if (operation == CleanupOperation::Destruction) target.reset();
        else if (operation == CleanupOperation::MoveAssign) *target = std::move(incoming);
        else target->reset();
        const bool correctFinalState = operation == CleanupOperation::Destruction ||
            (action == CleanupAction::Reset ? !*target
                : *target && target->fragmentId() == ids[2] && !replacement &&
                  probe.moduleDestructions[2] == 0 && probe.environmentCalls[2] == 0);
        const bool sourceMoved = operation != CleanupOperation::MoveAssign ||
            (!incoming && incoming.generationId() == 0 && incoming.environment() == nullptr &&
             incoming.descriptor() == nullptr);
        // Fixture environments stay allocated on the stack even when testing
        // the old implementation, so duplicate destroys are counted, not freed.
        if (target) target->reset();
        incoming.reset();
        replacement.reset();
        probe.target = nullptr;
        if (!correctFinalState || !sourceMoved || !probe.entered || !probe.observedExpectedState ||
            !probe.retainedDuringCallback || !probe.environmentBeforeModule)
            return fail("Fragment cleanup reentry exposed stale state or lost a new binding");
        for (unsigned index = 0; index < environments.size(); ++index) {
            if (probe.environmentCalls[index] != 1 || probe.environmentCompletions[index] != 1 ||
                probe.moduleDestructions[index] != 1)
                return fail("Fragment cleanup reentry repeated destruction or leaked an environment");
        }
    }
    return 0;
}

} // namespace

int main() {
    LunaRuntimeFragmentDescriptorV1 descriptor = {
        LUNA_RUNTIME_FRAGMENT_MAGIC_V1,
        LUNA_RUNTIME_FRAGMENT_ABI_V1,
        sizeof(LunaRuntimeFragmentDescriptorV1),
        0,
        0, 0, 0, 0,
        "fragment:trace",
        "contract:fragment-trace",
        "slot:pipeline",
        "contract:slot-pipeline",
        "layout:pipeline-arguments",
        sizeof(int),
        alignof(int),
        "contract:trace-factory",
        "layout:trace-environment",
        sizeof(Environment),
        alignof(Environment),
        createEnvironment,
        destroyEnvironment,
        executeFragment,
    };
    std::string error;
    if (!luna::runtime::validateRuntimeFragmentDescriptor(descriptor, error))
        return fail("valid runtime Fragment descriptor was rejected");

    auto malformed = descriptor;
    malformed.flags = LUNA_RUNTIME_FRAGMENT_CAPTURE_FREE_V1;
    if (luna::runtime::validateRuntimeFragmentDescriptor(malformed, error) ||
        error.find("capture-free") == std::string::npos)
        return fail("capture-free descriptor accepted a stateful environment");

    std::atomic<unsigned> moduleLeaseDestructions{0};
    luna::runtime::RuntimeFragmentRef owned;
    {
        Runtime runtime;
        auto lease = std::make_shared<LeaseProbe>(moduleLeaseDestructions);
        Runtime::PinnedBinding binding;
        if (!stageFragment(runtime, lease, &descriptor, binding, error))
            return fail("runtime Fragment generation did not stage and pin");
        lease.reset();

        const luna::runtime::RuntimeSlotRequirement slot{
            descriptor.slot_id, descriptor.slot_contract_id};
        FactoryArguments arguments{42};
        const luna::runtime::RuntimeFragmentFactoryArguments factoryArguments{
            descriptor.factory_contract_id, &arguments};

        auto wrongSlot = slot;
        wrongSlot.slotId = "slot:same-shape-but-distinct";
        if (luna::runtime::makeOwnedRuntimeFragmentRef(
                binding, wrongSlot, factoryArguments, owned, error) ||
            error.find("SlotId/ContractId") == std::string::npos ||
            factoryCalls.load() != 0)
            return fail("runtime Fragment accepted the wrong nominal SlotId");

        auto wrongArguments = factoryArguments;
        wrongArguments.contractId = "contract:wrong-factory";
        if (luna::runtime::makeOwnedRuntimeFragmentRef(
                binding, slot, wrongArguments, owned, error) ||
            error.find("factory arguments") == std::string::npos ||
            factoryCalls.load() != 0)
            return fail("runtime Fragment accepted the wrong factory contract");

        if (!luna::runtime::makeOwnedRuntimeFragmentRef(
                binding, slot, factoryArguments, owned, error) ||
            !owned || owned.generationId() == 0 ||
            owned.fragmentId() != descriptor.fragment_id ||
            owned.fragmentContractId() != descriptor.fragment_contract_id ||
            std::string(owned.slotId()) != descriptor.slot_id ||
            std::string(owned.slotContractId()) != descriptor.slot_contract_id ||
            factoryCalls.load() != 1)
            return fail("owned RuntimeFragmentRef lost its verified identity");

        Activation activation;
        owned.descriptor()->execute(owned.environment(), &activation);
        if (activation.observed != arguments.value)
            return fail("RuntimeFragmentRef execution lost its explicit environment");

        luna::runtime::RuntimeFragmentRef duplicate;
        if (luna::runtime::makeOwnedRuntimeFragmentRef(
                binding, slot, factoryArguments, owned, error) ||
            error.find("already initialized") == std::string::npos)
            return fail("RuntimeFragmentRef construction overwrote a live reference");

        {
            Runtime functionRuntime;
            Runtime::PinnedBinding wrongKind;
            if (!stageFragment(
                    functionRuntime, std::make_shared<int>(1), &descriptor,
                    wrongKind, error,
                    LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
                    luna::runtime::GenerationBindingCallable))
                return fail("wrong-kind fixture could not create a binding");
            if (luna::runtime::makeOwnedRuntimeFragmentRef(
                    wrongKind, slot, factoryArguments, duplicate, error) ||
                error.find("executable Fragment binding") == std::string::npos)
                return fail("ordinary callable binding became a RuntimeFragmentRef");
        }

        if (moduleLeaseDestructions.load() != 0)
            return fail("live RuntimeFragmentRef released its ModuleLease");
    }

    if (moduleLeaseDestructions.load() != 0 || destroyCalls.load() != 0)
        return fail("RuntimeFragmentRef did not pin its generation and environment");
    luna::runtime::RuntimeFragmentRef moved = std::move(owned);
    if (owned || !moved)
        return fail("RuntimeFragmentRef move did not transfer affine ownership");
    moved.reset();
    if (destroyCalls.load() != 1 || moduleLeaseDestructions.load() != 1)
        return fail("RuntimeFragmentRef cleanup order did not release environment then lease");

    LunaRuntimeFragmentDescriptorV1 borrowedDescriptor = descriptor;
    borrowedDescriptor.fragment_id = "fragment:borrowed";
    borrowedDescriptor.fragment_contract_id = "contract:fragment-borrowed";
    std::atomic<unsigned> borrowedLeaseDestructions{0};
    luna::runtime::RuntimeFragmentRef borrowed;
    {
        Runtime runtime;
        auto moduleLease = std::make_shared<int>(1);
        Runtime::PinnedBinding binding;
        if (!stageFragment(
                runtime, moduleLease, &borrowedDescriptor, binding, error))
            return fail("borrowed Fragment generation did not stage");
        auto environmentLease = std::make_shared<BorrowedEnvironmentProbe>(
            91, borrowedLeaseDestructions);
        luna::runtime::BorrowedFragmentEnvironment borrow{
            borrowedDescriptor.environment_layout_id,
            borrowedDescriptor.environment_size,
            borrowedDescriptor.environment_alignment,
            &environmentLease->environment,
            environmentLease,
        };
        const luna::runtime::RuntimeSlotRequirement slot{
            borrowedDescriptor.slot_id,
            borrowedDescriptor.slot_contract_id};
        if (!luna::runtime::makeBorrowedRuntimeFragmentRef(
                binding, slot, std::move(borrow), borrowed, error))
            return fail("matching borrowed Fragment environment was rejected");
        const int expected = environmentLease->environment.value;
        environmentLease.reset();
        Activation activation;
        borrowed.descriptor()->execute(borrowed.environment(), &activation);
        if (activation.observed != expected)
            return fail("borrowed RuntimeFragmentRef lost its environment");
    }
    if (borrowedLeaseDestructions.load() != 0)
        return fail("borrowed RuntimeFragmentRef did not retain its environment lease");
    borrowed.reset();
    if (borrowedLeaseDestructions.load() != 1 || destroyCalls.load() != 1)
        return fail("borrowed environment used owned Fragment destruction");

    LunaRuntimeFragmentDescriptorV1 captureFree = descriptor;
    captureFree.flags = LUNA_RUNTIME_FRAGMENT_CAPTURE_FREE_V1;
    captureFree.fragment_id = "fragment:capture-free";
    captureFree.fragment_contract_id = "contract:fragment-capture-free";
    captureFree.factory_contract_id = "";
    captureFree.environment_layout_id = "layout:unit";
    captureFree.environment_size = 0;
    captureFree.environment_alignment = 1;
    captureFree.factory = nullptr;
    captureFree.destroy = nullptr;

    {
        auto probeDescriptor = captureFree;
        probeDescriptor.slot_arguments_size = 8;
        probeDescriptor.slot_arguments_alignment = 8;
        probeDescriptor.execute = executePayloadProbe;
        Runtime runtime;
        auto lease = std::make_shared<int>(1);
        Runtime::PinnedBinding binding;
        if (!stageFragment(runtime, lease, &probeDescriptor, binding, error))
            return fail("payload validation fixture did not stage");
        const luna::runtime::RuntimeSlotRequirement slot{
            probeDescriptor.slot_id, probeDescriptor.slot_contract_id};
        luna::runtime::RuntimeFragmentRef reference;
        if (!luna::runtime::makeOwnedRuntimeFragmentRef(
                binding, slot, {"", nullptr}, reference, error))
            return fail("payload validation fixture did not bind");
        std::vector<luna::runtime::RuntimeFragmentRef> references;
        references.push_back(std::move(reference));
        luna::runtime::RuntimeFragmentBindingSet selected;
        luna::runtime::RuntimeFragmentBindingSet none;
        if (!luna::runtime::makeRuntimeFragmentBindingSet(
                std::move(references), selected, error) ||
            !luna::runtime::makeRuntimeFragmentBindingSet({}, none, error))
            return fail("payload validation BindingSets did not initialize");
        alignas(64) std::array<unsigned char, 64> storage{};
        const std::string layout = probeDescriptor.slot_arguments_layout_id;
        const std::array<luna::runtime::RuntimeFragmentArguments, 9> invalid = {{
            {layout, 8, 8, storage.data() + 1},
            {layout, 8, 8, nullptr},
            {layout, 0, 1, storage.data()},
            {layout, 8, 0, storage.data()},
            {layout, 8, 3, storage.data()},
            {layout, 8, 1u << 21, storage.data()},
            {layout, 0, 8, nullptr},
            {"", 8, 8, storage.data()},
            {"layout:\ninvalid", 8, 8, storage.data()},
        }};
        unsigned resumes = 0;
        for (const auto& arguments : invalid) {
            luna::runtime::RuntimeFragmentActivation activation;
            if (luna::runtime::makeRuntimeFragmentActivation(
                    slot, arguments, resumeActivation, &resumes, activation, error) ||
                activation || error.empty())
                return fail("activation accepted an invalid or unaligned payload");
            for (const auto* bindings : {&none, &selected}) {
                auto outcome = luna::runtime::RuntimeFragmentDispatchOutcome::ContinuationEscaped;
                if (bindings->dispatchWithOutcome(
                        slot, arguments, resumeActivation, &resumes, outcome, error) ||
                    error.empty() ||
                    outcome != luna::runtime::RuntimeFragmentDispatchOutcome::Completed)
                    return fail("None/One dispatch accepted an invalid payload");
                luna::runtime::RuntimeFragmentExecutionContext context;
                if (!luna::runtime::makeRuntimeFragmentExecutionContext(
                        *bindings, context, error))
                    return fail("payload validation context did not initialize");
                if (context.dispatch(slot, arguments, resumeActivation, &resumes, error) ||
                    error.empty())
                    return fail("execution context accepted an invalid payload");
                if (luna_runtime_fragment_dispatch_v1(
                        context.opaque(), slot.slotId.c_str(), slot.contractId.c_str(),
                        arguments.layoutId.c_str(), arguments.size, arguments.alignment,
                        arguments.data, resumeActivation, &resumes) !=
                        LUNA_RUNTIME_FRAGMENT_DISPATCH_INVALID_INVOCATION_V1 ||
                    resumes != 0 || payloadProbeExecutions != 0)
                    return fail("invalid payload reached a Fragment or continuation callback");
            }
        }
        const luna::runtime::RuntimeFragmentArguments aligned{
            layout, 8, 8, storage.data()};
        for (const bool inContract : {false, true}) {
            auto ambiguousSlot = slot;
            auto& identity = inContract ? ambiguousSlot.contractId : ambiguousSlot.slotId;
            identity += '\0';
            identity += "hidden";
            luna::runtime::RuntimeFragmentActivation activation;
            if (luna::runtime::makeRuntimeFragmentActivation(
                    ambiguousSlot, aligned, resumeActivation, &resumes, activation, error) ||
                activation || error.empty())
                return fail("activation accepted an embedded NUL Slot identity");
            luna::runtime::RuntimeFragmentRef ambiguousRef;
            if (luna::runtime::makeOwnedRuntimeFragmentRef(
                    binding, ambiguousSlot, {"", nullptr}, ambiguousRef, error) ||
                ambiguousRef || error.find("Slot requirement is invalid") == std::string::npos)
                return fail("Fragment construction did not reject an ambiguous Slot identity");
            for (const auto* bindings : {&none, &selected}) {
                if (bindings->dispatch(
                        ambiguousSlot, aligned, resumeActivation, &resumes, error) ||
                    error.empty() || resumes != 0 || payloadProbeExecutions != 0)
                    return fail("ambiguous Slot identity reached a dispatch callback");
                luna::runtime::RuntimeFragmentBindingSet localOverride;
                if (luna::runtime::makeRuntimeFragmentBindingOverride(
                        *bindings, ambiguousSlot, {}, localOverride, error) ||
                    localOverride || error.empty())
                    return fail("local override accepted an ambiguous Slot identity");
            }
        }
        auto ambiguousArguments = aligned;
        ambiguousArguments.layoutId += '\0';
        ambiguousArguments.layoutId += "hidden";
        luna::runtime::RuntimeFragmentActivation ambiguousActivation;
        if (luna::runtime::makeRuntimeFragmentActivation(
                slot, ambiguousArguments, resumeActivation, &resumes, ambiguousActivation, error) ||
            ambiguousActivation || error.empty() ||
            none.dispatch(slot, ambiguousArguments, resumeActivation, &resumes, error) ||
            error.empty() || resumes != 0)
            return fail("embedded NUL argument layout reached an activation or None callback");
        if (!selected.dispatch(slot, aligned, resumeActivation, &resumes, error) ||
            payloadProbeExecutions != 1 || resumes != 0 ||
            !none.dispatch(slot, aligned, resumeActivation, &resumes, error) ||
            resumes != 1)
            return fail("valid aligned None/One dispatch was rejected");
        const luna::runtime::RuntimeFragmentArguments empty{
            "layout:empty", 0, 1, nullptr};
        if (!none.dispatch(slot, empty, resumeActivation, &resumes, error) || resumes != 2)
            return fail("canonical empty payload was rejected");
        luna::runtime::RuntimeFragmentActivation emptyActivation;
        if (!luna::runtime::makeRuntimeFragmentActivation(
                slot, empty, resumeActivation, &resumes, emptyActivation, error) ||
            luna_runtime_fragment_activation_resume_v1(emptyActivation.opaque()) !=
                LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1 || resumes != 3)
            return fail("canonical empty activation was rejected");
        luna::runtime::RuntimeFragmentExecutionContext noneContext;
        if (!luna::runtime::makeRuntimeFragmentExecutionContext(none, noneContext, error) ||
            luna_runtime_fragment_dispatch_v1(
                noneContext.opaque(), slot.slotId.c_str(), slot.contractId.c_str(),
                empty.layoutId.c_str(), 0, 1, nullptr, resumeActivation, &resumes) !=
                LUNA_RUNTIME_FRAGMENT_DISPATCH_SUCCESS_V1 || resumes != 4)
            return fail("canonical empty C ABI invocation was rejected");
    }

    {
        auto probeDescriptor = descriptor;
        probeDescriptor.environment_size = 8;
        probeDescriptor.environment_alignment = 8;
        probeDescriptor.factory = createPayloadProbe;
        probeDescriptor.destroy = destroyPayloadProbe;
        probeDescriptor.execute = executePayloadProbe;
        Runtime runtime;
        auto lease = std::make_shared<int>(1);
        Runtime::PinnedBinding binding;
        if (!stageFragment(runtime, lease, &probeDescriptor, binding, error))
            return fail("environment validation fixture did not stage");
        const luna::runtime::RuntimeSlotRequirement slot{
            probeDescriptor.slot_id, probeDescriptor.slot_contract_id};
        alignas(64) std::array<unsigned char, 64> storage{};
        luna::runtime::RuntimeFragmentRef reference;
        luna::runtime::BorrowedFragmentEnvironment environment{
            probeDescriptor.environment_layout_id, 8, 8, storage.data() + 1, lease};
        if (luna::runtime::makeBorrowedRuntimeFragmentRef(
                binding, slot, environment, reference, error) || reference || error.empty())
            return fail("borrowed Fragment accepted an unaligned environment");
        if (luna::runtime::makeOwnedRuntimeFragmentRef(
                binding, slot,
                {probeDescriptor.factory_contract_id, storage.data() + 1}, reference, error) ||
            reference || error.empty() || payloadProbeDestructions != 1)
            return fail("invalid factory environment was published or not reclaimed once");
        environment.data = storage.data();
        if (!luna::runtime::makeBorrowedRuntimeFragmentRef(
                binding, slot, environment, reference, error))
            return fail("aligned borrowed environment was rejected");
        reference.reset();
        if (payloadProbeDestructions != 1 ||
            !luna::runtime::makeOwnedRuntimeFragmentRef(
                binding, slot, {probeDescriptor.factory_contract_id, storage.data()},
                reference, error))
            return fail("borrowed cleanup destroyed storage or aligned factory was rejected");
        reference.reset();
        if (payloadProbeDestructions != 2)
            return fail("aligned factory environment was not destroyed exactly once");
    }
    captureFree.execute = executeResumingFragment;
    if (!luna::runtime::validateRuntimeFragmentDescriptor(captureFree, error))
        return fail("canonical capture-free Fragment descriptor was rejected");

    {
        auto faulty = captureFree;
        faulty.execute = executeRepeatedResumeFragment;
        Runtime runtime;
        Runtime::PinnedBinding binding;
        if (!stageFragment(runtime, std::make_shared<int>(1), &faulty, binding, error))
            return fail("single-shot failure fixture did not stage");
        const luna::runtime::RuntimeSlotRequirement slot{
            faulty.slot_id, faulty.slot_contract_id};
        int argument = 42;
        const luna::runtime::RuntimeFragmentArguments arguments{
            faulty.slot_arguments_layout_id, sizeof(argument), alignof(int), &argument};
        luna::runtime::RuntimeFragmentRef reference;
        if (!luna::runtime::makeOwnedRuntimeFragmentRef(
                binding, slot, {"", nullptr}, reference, error))
            return fail("single-shot failure fixture did not bind");
        std::vector<luna::runtime::RuntimeFragmentRef> references;
        references.push_back(std::move(reference));
        luna::runtime::RuntimeFragmentBindingSet bindings;
        luna::runtime::RuntimeFragmentExecutionContext context;
        if (!luna::runtime::makeRuntimeFragmentBindingSet(
                std::move(references), bindings, error) ||
            !luna::runtime::makeRuntimeFragmentExecutionContext(bindings, context, error))
            return fail("single-shot failure context did not initialize");
        for (const int32_t result : {
                 LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1,
                 LUNA_RUNTIME_FRAGMENT_CONTINUATION_ESCAPED_V1}) {
            CountedControlProbe probe{0, result};
            auto outcome = luna::runtime::RuntimeFragmentDispatchOutcome::ContinuationEscaped;
            if (bindings.dispatchWithOutcome(
                    slot, arguments, countedControl, &probe, outcome, error) ||
                error.find("single-shot") == std::string::npos || probe.calls != 1 ||
                outcome != luna::runtime::RuntimeFragmentDispatchOutcome::Completed ||
                repeatedResumeResults[0] != result ||
                repeatedResumeResults[1] != LUNA_RUNTIME_FRAGMENT_DISPATCH_EXECUTION_FAILED_V1 ||
                repeatedResumeResults[2] != LUNA_RUNTIME_FRAGMENT_DISPATCH_EXECUTION_FAILED_V1)
                return fail("ignored repeated resume was reported as successful dispatch");
            probe.calls = 0;
            if (luna_runtime_fragment_dispatch_v1(
                    context.opaque(), slot.slotId.c_str(), slot.contractId.c_str(),
                    arguments.layoutId.c_str(), arguments.size, arguments.alignment,
                    arguments.data, countedControl, &probe) !=
                    LUNA_RUNTIME_FRAGMENT_DISPATCH_EXECUTION_FAILED_V1 || probe.calls != 1)
                return fail("C ABI lost a repeated-resume activation failure");
        }
        CountedControlProbe invalidControl{0, 99};
        if (context.dispatch(slot, arguments, countedControl, &invalidControl, error) ||
            invalidControl.calls != 1 || error.find("invalid control result") == std::string::npos)
            return fail("activation failure overwrote a downstream continuation diagnostic");
        repeatResumeProbe = false;
        CountedControlProbe recovered;
        if (!context.dispatch(slot, arguments, countedControl, &recovered, error) ||
            recovered.calls != 1 || !error.empty())
            return fail("failed activation poisoned a fresh invocation of the same context");
        repeatResumeProbe = true;

        auto outer = captureFree;
        outer.fragment_id = "fragment:failure-outer";
        outer.fragment_contract_id = "contract:failure-outer";
        outer.execute = executeFirstChainFragment;
        Runtime outerRuntime;
        Runtime::PinnedBinding outerBinding;
        if (!stageFragment(
                outerRuntime, std::make_shared<int>(1), &outer, outerBinding, error))
            return fail("single-shot outer chain fixture did not stage");
        luna::runtime::RuntimeFragmentRef outerRef;
        luna::runtime::RuntimeFragmentRef innerRef;
        if (!luna::runtime::makeOwnedRuntimeFragmentRef(
                outerBinding, slot, {"", nullptr}, outerRef, error) ||
            !luna::runtime::makeOwnedRuntimeFragmentRef(
                binding, slot, {"", nullptr}, innerRef, error))
            return fail("single-shot failure chain did not bind");
        std::vector<luna::runtime::RuntimeFragmentRef> chainRefs;
        chainRefs.push_back(std::move(outerRef));
        chainRefs.push_back(std::move(innerRef));
        luna::runtime::RuntimeFragmentBindingSet chain;
        if (!luna::runtime::makeRuntimeFragmentChainBindingSet(
                std::move(chainRefs), chain, error))
            return fail("single-shot failure chain did not initialize");
        std::vector<int> trace;
        activeChainTrace = &trace;
        const bool failedChain = chain.dispatch(slot, arguments, recordChainBase, &trace, error);
        activeChainTrace = nullptr;
        if (failedChain || error.find("single-shot") == std::string::npos ||
            trace != std::vector<int>({1, 0}))
            return fail("inner repeated resume failed to reach the outer chain handler");
        trace.clear();
        repeatResumeProbe = false;
        activeChainTrace = &trace;
        const bool recoveredChain = chain.dispatch(slot, arguments, recordChainBase, &trace, error);
        activeChainTrace = nullptr;
        repeatResumeProbe = true;
        if (!recoveredChain || !error.empty() || trace != std::vector<int>({1, 0, 4}))
            return fail("single-shot failure poisoned a new invocation of the chain");

        ReentrantResumeProbe reentrant;
        luna::runtime::RuntimeFragmentActivation activation;
        if (!luna::runtime::makeRuntimeFragmentActivation(
                slot, arguments, reentrantResume, &reentrant, activation, error))
            return fail("reentrant resume fixture did not initialize");
        reentrant.activation = activation.opaque();
        if (luna_runtime_fragment_activation_resume_v1(activation.opaque()) !=
                LUNA_RUNTIME_FRAGMENT_DISPATCH_EXECUTION_FAILED_V1 ||
            reentrant.nestedResult != LUNA_RUNTIME_FRAGMENT_DISPATCH_EXECUTION_FAILED_V1 ||
            reentrant.calls != 1 || !activation.resumed())
            return fail("pending resume reported success after recursive use of its activation");
    }

    {
        int argument = 42;
        unsigned resumes = 0;
        luna::runtime::RuntimeFragmentActivation activation;
        const luna::runtime::RuntimeSlotRequirement slot{
            descriptor.slot_id, descriptor.slot_contract_id};
        luna::runtime::RuntimeFragmentArguments arguments{
            descriptor.slot_arguments_layout_id,
            descriptor.slot_arguments_size,
            descriptor.slot_arguments_alignment,
            &argument};
        if (!luna::runtime::makeRuntimeFragmentActivation(
                slot, arguments, resumeActivation, &resumes,
                activation, error) || !activation || activation.resumed())
            return fail("valid opaque Fragment activation was rejected");
        if (luna_runtime_fragment_activation_arguments_v1(
                activation.opaque(), descriptor.slot_id,
                descriptor.slot_contract_id,
                descriptor.slot_arguments_layout_id,
                descriptor.slot_arguments_size,
                descriptor.slot_arguments_alignment) != &argument)
            return fail("opaque Fragment activation lost its typed arguments");
        if (luna_runtime_fragment_activation_arguments_v1(
                activation.opaque(), "slot:wrong",
                descriptor.slot_contract_id,
                descriptor.slot_arguments_layout_id,
                descriptor.slot_arguments_size,
                descriptor.slot_arguments_alignment))
            return fail("opaque Fragment activation accepted the wrong SlotId");
        if (luna_runtime_fragment_activation_resume_v1(
                activation.opaque()) != 0 ||
            !activation.resumed() || resumes != 1 ||
            luna_runtime_fragment_activation_resume_v1(
                activation.opaque()) == 0 || resumes != 1)
            return fail("opaque Fragment activation did not enforce single-shot resume");
    }

    {
        LunaRuntimeFragmentDescriptorV1 otherSlot = captureFree;
        otherSlot.fragment_id = "fragment:other-slot";
        otherSlot.fragment_contract_id = "contract:fragment-other-slot";
        otherSlot.slot_id = "slot:other";
        otherSlot.slot_contract_id = "contract:slot-other";
        LunaRuntimeFragmentDescriptorV1 privateMatch = captureFree;
        privateMatch.fragment_id = "fragment:private-match";
        privateMatch.fragment_contract_id = "contract:fragment-private-match";

        Runtime runtime;
        luna::runtime::GenerationStagingRequest request{
            "org.luna.runtime.fragment.catalog", std::string(64, 'c'),
            std::make_shared<int>(1)};
        Runtime::StagedGeneration staged;
        if (!runtime.stage(
                request,
                [](const auto&, std::string&) { return true; },
                [&](const auto&, auto& bindings, std::string&) {
                    const uint32_t publicExecutable =
                        luna::runtime::GenerationBindingPublicControl |
                        luna::runtime::GenerationBindingFragmentExecutable;
                    bindings.push_back({
                        captureFree.fragment_id,
                        captureFree.fragment_contract_id,
                        &captureFree,
                        LUNA_RUNTIME_DECLARATION_FRAGMENT_V1,
                        publicExecutable});
                    bindings.push_back({
                        otherSlot.fragment_id,
                        otherSlot.fragment_contract_id,
                        &otherSlot,
                        LUNA_RUNTIME_DECLARATION_FRAGMENT_V1,
                        publicExecutable});
                    bindings.push_back({
                        privateMatch.fragment_id,
                        privateMatch.fragment_contract_id,
                        &privateMatch,
                        LUNA_RUNTIME_DECLARATION_FRAGMENT_V1,
                        luna::runtime::GenerationBindingFragmentExecutable});
                    return true;
                },
                {}, staged, error))
            return fail("runtime Fragment catalog generation did not stage");
        Runtime::PinnedGeneration generation;
        if (!runtime.loadOnce(staged, generation, error))
            return fail("runtime Fragment catalog generation did not load");
        const luna::runtime::RuntimeSlotRequirement slot{
            captureFree.slot_id, captureFree.slot_contract_id};
        luna::runtime::RuntimeFragmentCandidateSnapshot snapshot;
        if (!luna::runtime::snapshotRuntimeFragmentCandidates(
                generation, slot, snapshot, error) ||
            !snapshot || snapshot.generationId() != generation.generationId() ||
            snapshot.size() != 1 || !snapshot.at(0) ||
            snapshot.at(0)->symbolId() != captureFree.fragment_id)
            return fail("typed candidate snapshot did not filter by public exact Slot contract");
        if (luna::runtime::snapshotRuntimeFragmentCandidates(
                generation, slot, snapshot, error) ||
            error.find("already initialized") == std::string::npos)
            return fail("candidate discovery overwrote an immutable snapshot");
        for (const bool inContract : {false, true}) {
            auto ambiguousSlot = slot;
            auto& identity = inContract ? ambiguousSlot.contractId : ambiguousSlot.slotId;
            identity += '\0';
            identity += "hidden";
            luna::runtime::RuntimeFragmentCandidateSnapshot ambiguousSnapshot;
            if (luna::runtime::snapshotRuntimeFragmentCandidates(
                    generation, ambiguousSlot, ambiguousSnapshot, error) ||
                ambiguousSnapshot || error.empty())
                return fail("candidate discovery accepted an embedded NUL Slot identity");
        }

        const luna::runtime::RuntimeFragmentFactoryArguments noFactory{
            "", nullptr};
        luna::runtime::RuntimeFragmentRef selected;
        if (!luna::runtime::makeOwnedRuntimeFragmentRef(
                *snapshot.at(0), slot, noFactory, selected, error))
            return fail("candidate could not become a selected Fragment ref");
        std::vector<luna::runtime::RuntimeFragmentRef> selectedBindings;
        selectedBindings.push_back(std::move(selected));
        luna::runtime::RuntimeFragmentBindingSet bindingSet;
        if (!luna::runtime::makeRuntimeFragmentBindingSet(
                std::move(selectedBindings), bindingSet, error) ||
            !bindingSet || bindingSet.size() != 1)
            return fail("host selection could not become an immutable BindingSet");

        luna::runtime::RuntimeFragmentRef duplicateLeft;
        luna::runtime::RuntimeFragmentRef duplicateRight;
        if (!luna::runtime::makeOwnedRuntimeFragmentRef(
                *snapshot.at(0), slot, noFactory, duplicateLeft, error) ||
            !luna::runtime::makeOwnedRuntimeFragmentRef(
                *snapshot.at(0), slot, noFactory, duplicateRight, error))
            return fail("duplicate BindingSet fixture could not bind candidates");
        std::vector<luna::runtime::RuntimeFragmentRef> duplicates;
        duplicates.push_back(std::move(duplicateLeft));
        duplicates.push_back(std::move(duplicateRight));
        luna::runtime::RuntimeFragmentBindingSet duplicateSet;
        if (luna::runtime::makeRuntimeFragmentBindingSet(
                std::move(duplicates), duplicateSet, error) ||
            error.find("more than one") == std::string::npos)
            return fail("BindingSet accepted two winners for one exact Slot");

        Runtime otherRuntime;
        auto wrongRuntimeSafePoint = otherRuntime.safePoint();
        if (runtime.activateFragmentBindings(
                bindingSet, wrongRuntimeSafePoint, error) ||
            error.find("same runtime") == std::string::npos)
            return fail("BindingSet accepted another runtime's safe point");
        auto activationPoint = runtime.safePoint();
        if (!runtime.activateFragmentBindings(
                bindingSet, activationPoint, error))
            return fail("BindingSet activation failed at a safe point");
        if (runtime.activateFragmentBindings(
                bindingSet, activationPoint, error) ||
            error.find("fresh safe point") == std::string::npos)
            return fail("BindingSet activation reused a consumed safe point");

        const auto active = runtime.pinFragmentBindings();
        if (!active || active.size() != 1)
            return fail("active BindingSet could not be pinned immutably");
        int argument = 42;
        unsigned resumes = 0;
        const luna::runtime::RuntimeFragmentArguments dispatchArguments{
            captureFree.slot_arguments_layout_id,
            captureFree.slot_arguments_size,
            captureFree.slot_arguments_alignment,
            &argument};
        if (!active.dispatch(
                slot, dispatchArguments, resumeActivation,
                &resumes, error) || resumes != 1)
            return fail("active BindingSet did not dispatch its selected Fragment");

        luna::runtime::RuntimeFragmentExecutionContext executionContext;
        if (!luna::runtime::makeRuntimeFragmentExecutionContext(
                active, executionContext, error) || !executionContext ||
            !executionContext.opaque())
            return fail("active BindingSet did not become an explicit execution context");
        unsigned abiResumes = 0;
        if (luna_runtime_fragment_dispatch_v1(
                executionContext.opaque(), slot.slotId.c_str(),
                slot.contractId.c_str(), dispatchArguments.layoutId.c_str(),
                dispatchArguments.size, dispatchArguments.alignment,
                dispatchArguments.data, resumeActivation, &abiResumes) !=
                LUNA_RUNTIME_FRAGMENT_DISPATCH_SUCCESS_V1 ||
            abiResumes != 1)
            return fail("explicit execution context did not dispatch through the C ABI");
        if (luna_runtime_fragment_dispatch_v1(
                nullptr, slot.slotId.c_str(), slot.contractId.c_str(),
                dispatchArguments.layoutId.c_str(), dispatchArguments.size,
                dispatchArguments.alignment, dispatchArguments.data,
                resumeActivation, &abiResumes) !=
                LUNA_RUNTIME_FRAGMENT_DISPATCH_INVALID_CONTEXT_V1 ||
            abiResumes != 1)
            return fail("Fragment dispatch accepted an implicit/null execution context");
        auto wrongLayout = dispatchArguments;
        wrongLayout.layoutId = "layout:wrong";
        if (active.dispatch(
                slot, wrongLayout, resumeActivation, &resumes, error) ||
            error.find("do not match") == std::string::npos ||
            resumes != 1)
            return fail("BindingSet dispatch accepted the wrong argument layout");

        luna::runtime::RuntimeFragmentBindingSet emptySet;
        if (!luna::runtime::makeRuntimeFragmentBindingSet(
                {}, emptySet, error) || !emptySet || emptySet.size() != 0)
            return fail("host policy None did not produce an empty BindingSet");
        auto clearPoint = runtime.safePoint();
        if (!runtime.activateFragmentBindings(
                emptySet, clearPoint, error))
            return fail("empty BindingSet did not activate at a safe point");
        const auto cleared = runtime.pinFragmentBindings();
        if (!cleared || cleared.size() != 0 ||
            !cleared.dispatch(
                slot, dispatchArguments, resumeActivation,
                &resumes, error) || resumes != 2)
            return fail("empty BindingSet did not run the base continuation");
        if (!active.dispatch(
                slot, dispatchArguments, resumeActivation,
                &resumes, error) || resumes != 3)
            return fail("replaced BindingSet snapshot did not remain immutable");
        if (luna_runtime_fragment_dispatch_v1(
                executionContext.opaque(), slot.slotId.c_str(),
                slot.contractId.c_str(), dispatchArguments.layoutId.c_str(),
                dispatchArguments.size, dispatchArguments.alignment,
                dispatchArguments.data, resumeActivation, &abiResumes) !=
                LUNA_RUNTIME_FRAGMENT_DISPATCH_SUCCESS_V1 ||
            abiResumes != 2)
            return fail("execution context did not pin its pre-activation snapshot");
    }

    {
        LunaRuntimeFragmentDescriptorV1 first = captureFree;
        first.fragment_id = "fragment:z-host-first";
        first.fragment_contract_id = "contract:z-host-first";
        first.execute = executeFirstChainFragment;
        LunaRuntimeFragmentDescriptorV1 second = captureFree;
        second.fragment_id = "fragment:a-host-second";
        second.fragment_contract_id = "contract:a-host-second";
        second.execute = executeSecondChainFragment;

        Runtime runtime;
        luna::runtime::GenerationStagingRequest request{
            "org.luna.runtime.fragment.chain", std::string(64, 'd'),
            std::make_shared<int>(1)};
        Runtime::StagedGeneration staged;
        if (!runtime.stage(
                request,
                [](const auto&, std::string&) { return true; },
                [&](const auto&, auto& bindings, std::string&) {
                    const uint32_t flags =
                        luna::runtime::GenerationBindingPublicControl |
                        luna::runtime::GenerationBindingFragmentExecutable;
                    bindings.push_back({
                        first.fragment_id, first.fragment_contract_id,
                        &first, LUNA_RUNTIME_DECLARATION_FRAGMENT_V1, flags});
                    bindings.push_back({
                        second.fragment_id, second.fragment_contract_id,
                        &second, LUNA_RUNTIME_DECLARATION_FRAGMENT_V1, flags});
                    return true;
                },
                {}, staged, error))
            return fail("ordered Fragment chain generation did not stage");
        Runtime::PinnedGeneration generation;
        if (!runtime.loadOnce(staged, generation, error))
            return fail("ordered Fragment chain generation did not load");
        const luna::runtime::RuntimeSlotRequirement slot{
            captureFree.slot_id, captureFree.slot_contract_id};
        luna::runtime::RuntimeFragmentCandidateSnapshot snapshot;
        if (!luna::runtime::snapshotRuntimeFragmentCandidates(
                generation, slot, snapshot, error) || snapshot.size() != 2)
            return fail("ordered Fragment chain candidates were not discoverable");

        const Runtime::PinnedBinding* firstBinding = nullptr;
        const Runtime::PinnedBinding* secondBinding = nullptr;
        for (size_t index = 0; index < snapshot.size(); ++index) {
            const auto* candidate = snapshot.at(index);
            if (candidate->symbolId() == first.fragment_id)
                firstBinding = candidate;
            else if (candidate->symbolId() == second.fragment_id)
                secondBinding = candidate;
        }
        if (!firstBinding || !secondBinding)
            return fail("ordered Fragment chain lost a selected candidate");
        const luna::runtime::RuntimeFragmentFactoryArguments noFactory{
            "", nullptr};
        luna::runtime::RuntimeFragmentRef firstRef;
        luna::runtime::RuntimeFragmentRef secondRef;
        if (!luna::runtime::makeOwnedRuntimeFragmentRef(
                *firstBinding, slot, noFactory, firstRef, error) ||
            !luna::runtime::makeOwnedRuntimeFragmentRef(
                *secondBinding, slot, noFactory, secondRef, error))
            return fail("ordered Fragment chain references could not be built");
        std::vector<luna::runtime::RuntimeFragmentRef> hostOrder;
        hostOrder.push_back(std::move(firstRef));
        hostOrder.push_back(std::move(secondRef));
        luna::runtime::RuntimeFragmentBindingSet chain;
        if (!luna::runtime::makeRuntimeFragmentChainBindingSet(
                std::move(hostOrder), chain, error) ||
            chain.size() != 1 || chain.bindingCount() != 2 ||
            chain.chainSize(slot) != 2)
            return fail("host order did not produce one deterministic chain");

        int argument = 42;
        const luna::runtime::RuntimeFragmentArguments dispatchArguments{
            captureFree.slot_arguments_layout_id,
            captureFree.slot_arguments_size,
            captureFree.slot_arguments_alignment,
            &argument};
        std::vector<int> trace;
        activeChainTrace = &trace;
        const bool dispatched = chain.dispatch(
            slot, dispatchArguments, recordChainBase, &trace, error);
        activeChainTrace = nullptr;
        if (!dispatched || trace != std::vector<int>({1, 2, 0, 3, 4}))
            return fail("ordered Fragment chain did not follow host order and resume nesting");

        trace.clear();
        activeChainTrace = &trace;
        luna::runtime::RuntimeFragmentDispatchOutcome escapeOutcome;
        const bool escaped = chain.dispatchWithOutcome(
            slot, dispatchArguments, recordEscapingChainBase, &trace,
            escapeOutcome, error);
        activeChainTrace = nullptr;
        if (!escaped || escapeOutcome !=
                luna::runtime::RuntimeFragmentDispatchOutcome::ContinuationEscaped ||
            trace != std::vector<int>({1, 2, 0}))
            return fail("continuation escape did not bypass post-resume chain code");

        luna::runtime::RuntimeFragmentExecutionContext chainContext;
        if (!luna::runtime::makeRuntimeFragmentExecutionContext(
                chain, chainContext, error))
            return fail("ordered chain did not become an execution context");
        trace.clear();
        activeChainTrace = &trace;
        const int32_t escapeStatus = luna_runtime_fragment_dispatch_v1(
            chainContext.opaque(), slot.slotId.c_str(),
            slot.contractId.c_str(), dispatchArguments.layoutId.c_str(),
            dispatchArguments.size, dispatchArguments.alignment,
            dispatchArguments.data, recordEscapingChainBase, &trace);
        activeChainTrace = nullptr;
        if (escapeStatus !=
                LUNA_RUNTIME_FRAGMENT_DISPATCH_CONTINUATION_ESCAPED_V1 ||
            trace != std::vector<int>({1, 2, 0}))
            return fail("C dispatch ABI lost the continuation escape result");

        // The same selected chain can be invoked from its base continuation.
        // Each dispatch owns fresh consumed bits; the outer activation remains
        // suspended and resumes normally after the nested invocation completes.
        trace.clear();
        NestedDispatchProbe nested{
            chainContext.opaque(), &slot, &dispatchArguments, &trace};
        activeChainTrace = &trace;
        const int32_t nestedStatus = luna_runtime_fragment_dispatch_v1(
            chainContext.opaque(), slot.slotId.c_str(),
            slot.contractId.c_str(), dispatchArguments.layoutId.c_str(),
            dispatchArguments.size, dispatchArguments.alignment,
            dispatchArguments.data, nestedChainBase, &nested);
        activeChainTrace = nullptr;
        if (nestedStatus != LUNA_RUNTIME_FRAGMENT_DISPATCH_SUCCESS_V1 ||
            nested.nestedStatus != LUNA_RUNTIME_FRAGMENT_DISPATCH_SUCCESS_V1 ||
            trace != std::vector<int>({1, 2, 0, 1, 2, 0, 3, 4, 3, 4}))
            return fail("nested same-Slot dispatch shared single-shot activation state");

        trace.clear();
        nested.entered = false;
        nested.escapeInner = true;
        activeChainTrace = &trace;
        const int32_t nestedEscapeStatus = luna_runtime_fragment_dispatch_v1(
            chainContext.opaque(), slot.slotId.c_str(),
            slot.contractId.c_str(), dispatchArguments.layoutId.c_str(),
            dispatchArguments.size, dispatchArguments.alignment,
            dispatchArguments.data, nestedChainBase, &nested);
        activeChainTrace = nullptr;
        if (nestedEscapeStatus !=
                LUNA_RUNTIME_FRAGMENT_DISPATCH_CONTINUATION_ESCAPED_V1 ||
            nested.nestedStatus !=
                LUNA_RUNTIME_FRAGMENT_DISPATCH_CONTINUATION_ESCAPED_V1 ||
            trace != std::vector<int>({1, 2, 0, 1, 2, 0}))
            return fail("nested same-Slot escape resumed outer post-resume code");

        luna::runtime::RuntimeFragmentRef localRef;
        if (!luna::runtime::makeOwnedRuntimeFragmentRef(
                *secondBinding, slot, noFactory, localRef, error))
            return fail("local Fragment override reference could not be built");
        std::vector<luna::runtime::RuntimeFragmentRef> localSelection;
        localSelection.push_back(std::move(localRef));
        luna::runtime::RuntimeFragmentBindingSet localOverride;
        if (!luna::runtime::makeRuntimeFragmentBindingOverride(
                chain, slot, std::move(localSelection),
                localOverride, error) ||
            localOverride.chainSize(slot) != 1)
            return fail("local Fragment override did not replace one Slot chain");
        trace.clear();
        activeChainTrace = &trace;
        const bool locallyDispatched = localOverride.dispatch(
            slot, dispatchArguments, recordChainBase, &trace, error);
        activeChainTrace = nullptr;
        if (!locallyDispatched || trace != std::vector<int>({2, 0, 3}))
            return fail("local Fragment override did not use its replacement chain");

        luna::runtime::RuntimeFragmentBindingSet localNone;
        if (!luna::runtime::makeRuntimeFragmentBindingOverride(
                chain, slot, {}, localNone, error) ||
            localNone.chainSize(slot) != 0)
            return fail("local Fragment policy None did not remove its Slot chain");
        trace.clear();
        if (!localNone.dispatch(
                slot, dispatchArguments, recordChainBase, &trace, error) ||
            trace != std::vector<int>({0}))
            return fail("local Fragment policy None did not call the base continuation");

        luna::runtime::RuntimeFragmentExecutionContext overrideContext;
        luna::runtime::RuntimeFragmentExecutionContext noneContext;
        if (!luna::runtime::makeRuntimeFragmentExecutionContext(
                localOverride, overrideContext, error) ||
            !luna::runtime::makeRuntimeFragmentExecutionContext(
                localNone, noneContext, error))
            return fail("local override did not produce independent execution contexts");
        const auto dispatchNestedOverride = [&](const void* innerContext,
                                                 bool escapeInner) {
            trace.clear();
            NestedDispatchProbe probe{
                innerContext, &slot, &dispatchArguments, &trace};
            probe.escapeInner = escapeInner;
            activeChainTrace = &trace;
            const int32_t status = luna_runtime_fragment_dispatch_v1(
                chainContext.opaque(), slot.slotId.c_str(),
                slot.contractId.c_str(), dispatchArguments.layoutId.c_str(),
                dispatchArguments.size, dispatchArguments.alignment,
                dispatchArguments.data, nestedChainBase, &probe);
            activeChainTrace = nullptr;
            return std::pair<int32_t, int32_t>{status, probe.nestedStatus};
        };
        const auto overrideStatus = dispatchNestedOverride(
            overrideContext.opaque(), false);
        if (overrideStatus.first != LUNA_RUNTIME_FRAGMENT_DISPATCH_SUCCESS_V1 ||
            overrideStatus.second != LUNA_RUNTIME_FRAGMENT_DISPATCH_SUCCESS_V1 ||
            trace != std::vector<int>({1, 2, 0, 2, 0, 3, 3, 4}))
            return fail("nested local override changed its suspended outer chain");
        const auto noneStatus = dispatchNestedOverride(noneContext.opaque(), false);
        if (noneStatus.first != LUNA_RUNTIME_FRAGMENT_DISPATCH_SUCCESS_V1 ||
            noneStatus.second != LUNA_RUNTIME_FRAGMENT_DISPATCH_SUCCESS_V1 ||
            trace != std::vector<int>({1, 2, 0, 0, 3, 4}))
            return fail("nested policy None suppressed its suspended outer chain");
        const auto noneEscapeStatus = dispatchNestedOverride(noneContext.opaque(), true);
        if (noneEscapeStatus.first !=
                LUNA_RUNTIME_FRAGMENT_DISPATCH_CONTINUATION_ESCAPED_V1 ||
            noneEscapeStatus.second !=
                LUNA_RUNTIME_FRAGMENT_DISPATCH_CONTINUATION_ESCAPED_V1 ||
            trace != std::vector<int>({1, 2, 0, 0}))
            return fail("nested policy None lost outer continuation escape propagation");
        trace.clear();
        activeChainTrace = &trace;
        const bool baseUnchanged = chain.dispatch(
            slot, dispatchArguments, recordChainBase, &trace, error);
        activeChainTrace = nullptr;
        if (!baseUnchanged || trace != std::vector<int>({1, 2, 0, 3, 4}))
            return fail("local Fragment override mutated its base BindingSet");
    }

    if (testScopedActivation(descriptor) != 0) return 1;
    if (testDispatchLifetime(descriptor) != 0) return 1;
    if (testFactoryLifetime(descriptor) != 0) return 1;
    return testCleanupReentry(descriptor);
}
