#include "runtime/RuntimeFragment.h"
#include "runtime/RuntimeDescriptorABI.h"

#include <array>
#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace luna::runtime;
constexpr unsigned Readers = 4;
constexpr unsigned Epochs = 96;
constexpr unsigned PinnedCalls = 256;
constexpr const char* SlotId = "slot:concurrent";
constexpr const char* SlotContract = "contract:concurrent-slot";
constexpr const char* ArgumentsLayout = "layout:concurrent-arguments";

struct Evidence {
    std::array<std::atomic<unsigned>, 3> environments{};
    std::atomic<unsigned> modules{0};
    std::atomic<bool> cleanupOrdered{true};
    std::atomic<bool> failed{false};
    Evidence() {
        for (auto& count : environments) count.store(0, std::memory_order_relaxed);
    }
};

struct Environment {
    unsigned marker;
    Evidence* evidence;
};

struct Publication {
    std::atomic<unsigned> epoch{0};
    std::array<std::atomic<unsigned>, Readers> entered{};
    std::atomic<unsigned> ready{0};
    std::atomic<unsigned> finished{0};
    std::atomic<bool> pinnedPhase{false};
    std::atomic<bool> abort{false};
    Publication() {
        for (auto& value : entered) value.store(0, std::memory_order_relaxed);
    }
};

struct Call {
    std::array<unsigned, 5> trace{};
    size_t size = 0;
    int32_t control = LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1;
    Publication* publication = nullptr;
    unsigned reader = 0;
    unsigned epoch = 0;
    bool valid = true;
    void record(unsigned marker) {
        if (size == trace.size()) valid = false;
        else trace[size++] = marker;
    }
};

struct Arguments { Call* call; };

int32_t factory(const void* arguments, void** output) {
    *output = new Environment(*static_cast<const Environment*>(arguments));
    return 0;
}

void destroy(void* storage) {
    auto* environment = static_cast<Environment*>(storage);
    environment->evidence->environments[environment->marker - 1].fetch_add(
        1, std::memory_order_relaxed);
    delete environment;
}

void execute(void* storage, void* activation) {
    const auto& environment = *static_cast<const Environment*>(storage);
    const auto* arguments = static_cast<const Arguments*>(
        luna_runtime_fragment_activation_arguments_v1(
            activation, SlotId, SlotContract, ArgumentsLayout,
            sizeof(Arguments), alignof(Arguments)));
    if (!arguments || !arguments->call) return;
    auto& call = *arguments->call;
    call.record(environment.marker);
    if (luna_runtime_fragment_activation_resume_v1(activation) ==
        LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1)
        call.record(environment.marker + 10);
}

int32_t base(void* storage) {
    auto& call = *static_cast<Call*>(storage);
    call.record(0);
    if (call.publication) {
        auto& publication = *call.publication;
        // Publication can advance only when every old continuation is live.
        publication.entered[call.reader].store(call.epoch, std::memory_order_release);
        while (publication.epoch.load(std::memory_order_acquire) <= call.epoch &&
               !publication.abort.load(std::memory_order_acquire))
            std::this_thread::yield();
    }
    return call.control;
}

bool dispatchAndCheck(const RuntimeFragmentExecutionContext& context,
                      unsigned policy, Call& call, bool cAbi) {
    const RuntimeSlotRequirement slot{SlotId, SlotContract};
    const Arguments carrier{&call};
    const RuntimeFragmentArguments arguments{
        ArgumentsLayout, sizeof(carrier), alignof(Arguments), &carrier};
    const bool expectedSuccess = call.control != 99;
    const auto expectedOutcome = call.control == 1
        ? RuntimeFragmentDispatchOutcome::ContinuationEscaped
        : RuntimeFragmentDispatchOutcome::Completed;
    if (cAbi) {
        const auto status = luna_runtime_fragment_dispatch_v1(
            context.opaque(), SlotId, SlotContract, ArgumentsLayout,
            sizeof(carrier), alignof(Arguments), &carrier, base, &call);
        const auto expectedStatus = call.control == 99
            ? LUNA_RUNTIME_FRAGMENT_DISPATCH_EXECUTION_FAILED_V1
            : (call.control == 1 ? LUNA_RUNTIME_FRAGMENT_DISPATCH_CONTINUATION_ESCAPED_V1
                                 : LUNA_RUNTIME_FRAGMENT_DISPATCH_SUCCESS_V1);
        if (status != expectedStatus) return false;
    } else {
        std::string error;
        RuntimeFragmentDispatchOutcome outcome;
        if (context.dispatchWithOutcome(slot, arguments, base, &call, outcome, error) !=
                expectedSuccess || outcome != expectedOutcome ||
            (expectedSuccess ? !error.empty() : error.find("invalid control result") == std::string::npos))
            return false;
    }
    const std::array<unsigned, 5> expected = policy == 0
        ? std::array<unsigned, 5>{0, 0, 0, 0, 0}
        : (policy == 1 ? std::array<unsigned, 5>{1, 0, 11, 0, 0}
                       : std::array<unsigned, 5>{2, 3, 0, 13, 12});
    const size_t size = policy == 0 ? 1 :
        (call.control == 0 ? (policy == 1 ? 3 : 5) : (policy == 1 ? 2 : 3));
    if (!call.valid || call.size != size) return false;
    for (size_t index = 0; index < size; ++index)
        if (call.trace[index] != expected[index]) return false;
    return true;
}

bool buildPolicies(MoonRuntime& runtime, Evidence& evidence,
                   std::array<LunaRuntimeFragmentDescriptorV1, 3>& descriptors,
                   std::array<RuntimeFragmentBindingSet, 3>& policies, std::string& error) {
    const std::array<const char*, 3> names{
        "fragment:concurrent-one", "fragment:concurrent-first", "fragment:concurrent-second"};
    for (unsigned index = 0; index < descriptors.size(); ++index) {
        descriptors[index] = {
            LUNA_RUNTIME_FRAGMENT_MAGIC_V1, LUNA_RUNTIME_FRAGMENT_ABI_V1,
            sizeof(LunaRuntimeFragmentDescriptorV1), 0, 0, 0, 0, 0,
            names[index], names[index], SlotId, SlotContract, ArgumentsLayout,
            sizeof(Arguments), alignof(Arguments), "contract:concurrent-factory",
            "layout:concurrent-environment", sizeof(Environment), alignof(Environment),
            factory, destroy, execute};
    }
    auto lease = std::shared_ptr<const void>(new int(1), [&evidence](const void* storage) {
        for (const auto& count : evidence.environments)
            if (count.load(std::memory_order_relaxed) != 1)
                evidence.cleanupOrdered.store(false, std::memory_order_relaxed);
        evidence.modules.fetch_add(1, std::memory_order_relaxed);
        delete static_cast<const int*>(storage);
    });
    const uint32_t flags = GenerationBindingPublicControl | GenerationBindingFragmentExecutable;
    MoonRuntime::StagedGeneration staged;
    if (!runtime.stage({"org.luna.concurrent-fragments", std::string(64, 'f'), lease},
            [](const auto&, std::string&) { return true; },
            [&](const auto&, auto& bindings, std::string&) {
                for (const auto& descriptor : descriptors)
                    bindings.push_back({descriptor.fragment_id, descriptor.fragment_contract_id,
                        &descriptor, LUNA_RUNTIME_DECLARATION_FRAGMENT_V1, flags});
                return true;
            }, {}, staged, error)) return false;
    MoonRuntime::PinnedGeneration generation;
    if (!runtime.loadOnce(staged, generation, error) ||
        !makeRuntimeFragmentBindingSet({}, policies[0], error)) return false;
    std::vector<RuntimeFragmentRef> one;
    std::vector<RuntimeFragmentRef> chain;
    for (unsigned index = 0; index < descriptors.size(); ++index) {
        const auto& descriptor = descriptors[index];
        const auto binding = generation.find({descriptor.fragment_id, descriptor.fragment_contract_id,
            LUNA_RUNTIME_DECLARATION_FRAGMENT_V1, flags});
        const Environment arguments{index + 1, &evidence};
        RuntimeFragmentRef reference;
        if (!makeOwnedRuntimeFragmentRef(binding, {SlotId, SlotContract},
                {descriptor.factory_contract_id, &arguments}, reference, error)) return false;
        (index == 0 ? one : chain).push_back(std::move(reference));
    }
    return makeRuntimeFragmentBindingSet(std::move(one), policies[1], error) &&
           makeRuntimeFragmentChainBindingSet(std::move(chain), policies[2], error);
}
} // namespace

int main() {
    Evidence evidence;
    std::array<LunaRuntimeFragmentDescriptorV1, 3> descriptors{};
    std::array<RuntimeFragmentBindingSet, 3> policies;
    auto runtime = std::make_unique<MoonRuntime>();
    std::string error;
    RuntimeFragmentExecutionContext heldContext;
    if (!buildPolicies(*runtime, evidence, descriptors, policies, error) ||
        !makeRuntimeFragmentExecutionContext(policies[2], heldContext, error)) {
        std::cerr << "Fragment concurrency fixture failed: " << error << '\n';
        return 1;
    }
    Publication publication;
    std::vector<std::thread> readers;
    for (unsigned reader = 0; reader < Readers; ++reader) {
        readers.emplace_back([&, reader, context = heldContext] {
            publication.ready.fetch_add(1, std::memory_order_release);
            for (unsigned epoch = 1; epoch <= Epochs; ++epoch) {
                while (publication.epoch.load(std::memory_order_acquire) < epoch &&
                       !publication.abort.load(std::memory_order_acquire))
                    std::this_thread::yield();
                if (publication.abort.load(std::memory_order_acquire)) break;
                const auto bindings = runtime->pinFragmentBindings();
                RuntimeFragmentExecutionContext selected;
                std::string localError;
                Call call;
                call.control = (epoch + reader) % 3 == 2 ? 99 : (epoch + reader) % 3;
                call.publication = &publication;
                call.reader = reader;
                call.epoch = epoch;
                if (bindings.chainSize({SlotId, SlotContract}) != epoch % 3 ||
                    !(reader % 2 == 0
                        ? makeRuntimeFragmentExecutionContextOverride(
                            context, {SlotId, SlotContract}, bindings, selected, localError)
                        : makeRuntimeFragmentExecutionContext(bindings, selected, localError)) ||
                    !dispatchAndCheck(selected, epoch % 3, call, reader % 2 == 0)) {
                    evidence.failed.store(true, std::memory_order_relaxed);
                    publication.abort.store(true, std::memory_order_release);
                    break;
                }
            }
            publication.finished.fetch_add(1, std::memory_order_release);
            while (!publication.pinnedPhase.load(std::memory_order_acquire))
                std::this_thread::yield();
            if (publication.abort.load(std::memory_order_acquire)) return;
            for (unsigned invocation = 0; invocation < PinnedCalls; ++invocation) {
                Call call;
                call.control = invocation % 3 == 2 ? 99 : invocation % 3;
                if (evidence.modules.load(std::memory_order_relaxed) != 0 ||
                    evidence.environments[1].load(std::memory_order_relaxed) != 0 ||
                    evidence.environments[2].load(std::memory_order_relaxed) != 0 ||
                    !dispatchAndCheck(context, 2, call, invocation % 2 == 0))
                    evidence.failed.store(true, std::memory_order_relaxed);
            }
        });
    }
    while (publication.ready.load(std::memory_order_acquire) != Readers)
        std::this_thread::yield();
    heldContext = {}; // Each reader owns its handle; no shared handle is mutated.
    for (unsigned epoch = 1; epoch <= Epochs; ++epoch) {
        auto safePoint = runtime->safePoint();
        if (!runtime->activateFragmentBindings(policies[epoch % 3], safePoint, error)) {
            evidence.failed.store(true, std::memory_order_relaxed);
            publication.abort.store(true, std::memory_order_release);
            break;
        }
        publication.epoch.store(epoch, std::memory_order_release);
        bool entered = false;
        while (!entered && !publication.abort.load(std::memory_order_acquire)) {
            entered = true;
            for (const auto& value : publication.entered)
                entered &= value.load(std::memory_order_acquire) == epoch;
            if (!entered) std::this_thread::yield();
        }
        if (publication.abort.load(std::memory_order_acquire)) break;
    }
    auto clearPoint = runtime->safePoint();
    if (!runtime->activateFragmentBindings(policies[0], clearPoint, error))
        evidence.failed.store(true, std::memory_order_relaxed);
    publication.epoch.store(Epochs + 1, std::memory_order_release);
    while (publication.finished.load(std::memory_order_acquire) != Readers)
        std::this_thread::yield();
    policies = {};
    runtime.reset(); // Readers can now use only their explicit pinned contexts.
    if (evidence.modules.load() != 0 || evidence.environments[0].load() != 1 ||
        evidence.environments[1].load() != 0 || evidence.environments[2].load() != 0)
        evidence.failed.store(true, std::memory_order_relaxed);
    publication.pinnedPhase.store(true, std::memory_order_release);
    for (auto& reader : readers) reader.join();
    if (evidence.failed.load() || !evidence.cleanupOrdered.load() || evidence.modules.load() != 1) {
        std::cerr << "concurrent Fragment publication/dispatch or pinned lifetime failed\n";
        return 1;
    }
    for (const auto& count : evidence.environments) {
        if (count.load() != 1) {
            std::cerr << "concurrent Fragment environment cleanup was not exactly once\n";
            return 1;
        }
    }
    return 0;
}
