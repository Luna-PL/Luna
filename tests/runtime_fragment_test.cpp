#include "runtime/RuntimeFragment.h"
#include "runtime/RuntimeDescriptorABI.h"

#include <array>
#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

    return 0;
}
