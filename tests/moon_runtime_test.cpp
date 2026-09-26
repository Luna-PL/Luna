#include "runtime/Evolution.h"

#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

int fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

struct LeaseProbe {
    explicit LeaseProbe(std::atomic<unsigned>& destructions)
        : destructions(destructions) {}
    ~LeaseProbe() { ++destructions; }
    std::atomic<unsigned>& destructions;
};

struct Implementation {
    int value = 0;
};

using Runtime = luna::runtime::MoonRuntime;

static_assert(luna::runtime::EvolutionApiVersion == 1);
using Request = luna::runtime::GenerationStagingRequest;
using Binding = luna::runtime::GenerationBinding;

bool stageOne(Runtime& runtime, const Request& request,
              const std::string& symbolId, const std::string& contractId,
              const void* implementation, Runtime::StagedGeneration& staged,
              std::string& phases, std::string& error,
              bool initializerSucceeds = true,
              uint32_t declarationKind = 1,
              uint32_t flags = luna::runtime::GenerationBindingCallable) {
    return runtime.stage(
        request,
        [&](const Request&, std::string&) {
            phases += 'V';
            return true;
        },
        [&](const Request&, std::vector<Binding>& bindings, std::string&) {
            phases += 'R';
            bindings.push_back({symbolId, contractId, implementation,
                                declarationKind, flags});
            return true;
        },
        [&](const Request&, const std::vector<Binding>& bindings,
            std::string& initializerError) {
            phases += 'I';
            if (bindings.size() != 1) {
                initializerError = "initializer saw incomplete bindings";
                return false;
            }
            if (!initializerSucceeds)
                initializerError = "fixture initializer failed";
            return initializerSucceeds;
        },
        staged, error);
}

const Implementation* implementationOf(const Runtime::PinnedBinding& binding) {
    return static_cast<const Implementation*>(binding.implementation());
}

} // namespace

int main() {
    constexpr const char* ModuleId = "org.luna.test.evolution";
    constexpr const char* SymbolId = "symbol:answer";
    constexpr const char* ContractId = "contract:i32-v1";
    Implementation first{1};
    Implementation second{2};
    Implementation third{3};
    Implementation incompatible{99};
    std::atomic<unsigned> firstLeaseDestructions{0};
    std::atomic<unsigned> secondLeaseDestructions{0};
    std::atomic<unsigned> thirdLeaseDestructions{0};
    std::string error;

    // Artifact content and loader configuration are independent identities.
    // The optional key preserves existing three-field request construction.
    {
        Runtime runtime;
        Request request{ModuleId, std::string(64, 'a'), std::make_shared<int>(1)};
        request.materializationKey = "fixture:O0";
        Runtime::StagedGeneration staged;
        std::string phases;
        if (!stageOne(runtime, request, SymbolId, ContractId, &first, staged, phases, error) ||
            staged.materializationKey() != "fixture:O0")
            return fail("staging lost materialization identity");
        Runtime::PinnedGeneration loaded;
        if (!runtime.loadOnce(staged, loaded, error) ||
            loaded.materializationKey() != "fixture:O0")
            return fail("load-once lost materialization identity");
        request.materializationKey = "fixture:O2";
        phases.clear();
        if (!stageOne(runtime, request, SymbolId, ContractId, &second, staged, phases, error))
            return fail("alternate materialization did not stage");
        const auto alternateId = staged.generationId();
        Runtime::PinnedGeneration rejected;
        if (runtime.loadOnce(staged, rejected, error) || rejected ||
            staged.generationId() != alternateId ||
            error.find("materialization configuration") == std::string::npos ||
            runtime.activeGenerationId(ModuleId) != loaded.generationId() ||
            runtime.retainedGenerationCount(ModuleId) != 1)
            return fail("cross-configuration load-once changed publication or outputs");
        auto point = runtime.safePoint();
        if (!runtime.activate(staged, point, error) || staged ||
            runtime.pin(ModuleId).materializationKey() != "fixture:O2" ||
            loaded.materializationKey() != "fixture:O0")
            return fail("explicit activation did not preserve configuration snapshots");
        auto rollback = runtime.safePoint();
        if (!runtime.rollback(ModuleId, loaded.generationId(), rollback, error) ||
            runtime.pin(ModuleId).materializationKey() != "fixture:O0")
            return fail("rollback lost retained materialization configuration");
        request.materializationKey = "fixture:O0";
        phases.clear();
        if (!stageOne(runtime, request, SymbolId, ContractId, &second, staged, phases, error) ||
            !runtime.loadOnce(staged, rejected, error) ||
            rejected.generationId() != loaded.generationId())
            return fail("same-configuration load-once did not reuse code");
        for (const auto& invalid : {std::string("bad\0key", 7), std::string("bad\nkey"),
                                   std::string("bad\rkey"), std::string("bad\tkey")}) {
            request.materializationKey = invalid;
            phases.clear();
            if (stageOne(runtime, request, SymbolId, ContractId, &first, staged, phases, error) ||
                staged || !phases.empty() || error.empty())
                return fail("invalid materialization key reached verification");
        }
        if (!Runtime::StagedGeneration{}.materializationKey().empty() ||
            !Runtime::PinnedGeneration{}.materializationKey().empty())
            return fail("empty generation exposed a configuration key");
    }

    // Runtime IDs also cross null-terminated C ABI descriptors. A C++ string
    // with an embedded NUL must not become a second identity for one C name.
    for (unsigned field = 0; field < 3; ++field) {
        Runtime runtime;
        Request request{ModuleId, std::string(64, 'a'), std::make_shared<int>(1)};
        std::string symbol = SymbolId;
        std::string contract = ContractId;
        auto& identity = field == 0 ? request.moduleId : (field == 1 ? symbol : contract);
        identity += '\0';
        identity += "hidden";
        Runtime::StagedGeneration staged;
        std::string phases;
        if (stageOne(runtime, request, symbol, contract, &first, staged, phases, error) ||
            staged || error.empty() || phases != (field == 0 ? "" : "VR"))
            return fail("embedded NUL identity reached generation initialization");
    }

    {
        std::atomic<unsigned> leaseDestructions{0};
        Runtime runtime;
        Request request{
            ModuleId, std::string(64, 'a'),
            std::make_shared<LeaseProbe>(leaseDestructions)};
        Runtime::StagedGeneration staged;
        std::string phases;
        if (!stageOne(runtime, request, SymbolId, ContractId, &first,
                      staged, phases, error))
            return fail("load-once fixture did not stage");
        Runtime::PinnedGeneration loaded;
        if (!runtime.loadOnce(staged, loaded, error) || staged || !loaded ||
            runtime.retainedGenerationCount(ModuleId) != 1)
            return fail("first load-once publication did not pin one generation");
        const luna::runtime::GenerationBindingRequirement typedFunction{
            SymbolId, ContractId, 1,
            luna::runtime::GenerationBindingCallable};
        if (!loaded.find(typedFunction))
            return fail("typed binding requirement rejected its exact export");
        auto wrongKind = typedFunction;
        wrongKind.declarationKind = 2;
        if (loaded.find(wrongKind))
            return fail("typed binding requirement accepted the wrong declaration kind");
        auto wrongFlags = typedFunction;
        wrongFlags.requiredFlags = 1u << 8;
        if (loaded.find(wrongFlags))
            return fail("typed binding requirement accepted missing capabilities");
        auto fragmentContextFunction = typedFunction;
        fragmentContextFunction.requiredFlags =
            luna::runtime::GenerationBindingCallable |
            luna::runtime::GenerationBindingFragmentContext;
        if (loaded.find(fragmentContextFunction))
            return fail("ordinary callable binding satisfied fragment-context ABI");

        Runtime::StagedGeneration contextAware;
        phases.clear();
        if (!stageOne(
                runtime, request, "symbol:context-aware", ContractId, &first,
                contextAware, phases, error, true, 1,
                luna::runtime::GenerationBindingCallable |
                    luna::runtime::GenerationBindingFragmentContext) ||
            phases != "VRI")
            return fail("runtime rejected a context-aware function binding");

        Runtime::StagedGeneration invalidFragmentContext;
        phases.clear();
        if (stageOne(
                runtime, request, "symbol:missing-callable", ContractId,
                &first, invalidFragmentContext, phases, error, true, 1,
                luna::runtime::GenerationBindingFragmentContext) ||
            error.find("invalid or duplicate binding") == std::string::npos)
            return fail("runtime accepted fragment-context ABI without callable");

        Runtime::StagedGeneration invalidPublicControl;
        phases.clear();
        if (stageOne(
                runtime, request, "symbol:not-control", "contract:not-control",
                &first, invalidPublicControl, phases, error, true,
                1,
                luna::runtime::GenerationBindingPublicControl) ||
            error.find("invalid or duplicate binding") == std::string::npos)
            return fail("runtime accepted a public-control function binding");

        Runtime::StagedGeneration duplicateStaged;
        phases.clear();
        if (!stageOne(runtime, request, SymbolId, ContractId, &second,
                      duplicateStaged, phases, error))
            return fail("same-content load-once fixture did not stage");
        Runtime::PinnedGeneration duplicateLoaded;
        if (!runtime.loadOnce(
                duplicateStaged, duplicateLoaded, error) ||
            duplicateLoaded.generationId() != loaded.generationId() ||
            implementationOf(duplicateLoaded.find(typedFunction)) != &first ||
            runtime.retainedGenerationCount(ModuleId) != 1)
            return fail("same-content load-once did not reuse the first generation");

        Request changedRequest = request;
        changedRequest.contentDigest = std::string(64, 'b');
        Runtime::StagedGeneration changedStaged;
        phases.clear();
        if (!stageOne(runtime, changedRequest, SymbolId, ContractId, &second,
                      changedStaged, phases, error))
            return fail("changed-content load-once fixture did not stage");
        Runtime::PinnedGeneration changedLoaded;
        if (runtime.loadOnce(changedStaged, changedLoaded, error) ||
            error.find("different content") == std::string::npos ||
            runtime.retainedGenerationCount(ModuleId) != 1)
            return fail("load-once replaced an already loaded module");
    }

    {
        Runtime runtime;
        Request firstRequest{
            ModuleId, std::string(64, '1'),
            std::make_shared<LeaseProbe>(firstLeaseDestructions)};
        Runtime::StagedGeneration firstStaged;
        std::string phases;
        if (!stageOne(runtime, firstRequest, SymbolId, ContractId, &first,
                      firstStaged, phases, error) || phases != "VRI" ||
            firstStaged.generationId() == 0 ||
            firstStaged.moduleId() != ModuleId) {
            std::cerr << error << '\n';
            return fail("generation did not complete verify-resolve-initialize staging");
        }
        const uint64_t firstId = firstStaged.generationId();
        auto firstSafePoint = runtime.safePoint();
        if (!runtime.activate(firstStaged, firstSafePoint, error) ||
            firstStaged || runtime.activeGenerationId(ModuleId) != firstId)
            return fail("first generation did not activate atomically");
        if (runtime.rollback(ModuleId, firstId, firstSafePoint, error) ||
            error.find("fresh safe point") == std::string::npos)
            return fail("one safe point authorized more than one transition");

        auto pinnedFirst = runtime.pin(ModuleId);
        auto pinnedFirstBinding = pinnedFirst.find(SymbolId, ContractId);
        if (!pinnedFirstBinding || implementationOf(pinnedFirstBinding) != &first)
            return fail("ordinary reference did not pin the first generation");
        Runtime::SwitchableBinding switchable;
        if (!runtime.makeSwitchable(
                ModuleId, SymbolId, ContractId, switchable, error) ||
            implementationOf(switchable.pin()) != &first)
            return fail("switchable binding did not bind the active generation");
        Runtime::SwitchableBinding wrongContract;
        if (runtime.makeSwitchable(
                ModuleId, SymbolId, "contract:wrong", wrongContract, error))
            return fail("switchable binding accepted a mismatched ContractId");

        Request failedRequest{
            ModuleId, std::string(64, 'f'),
            std::make_shared<LeaseProbe>(firstLeaseDestructions)};
        Runtime::StagedGeneration failedStaged;
        phases.clear();
        if (stageOne(runtime, failedRequest, SymbolId, ContractId, &second,
                     failedStaged, phases, error, false) || failedStaged ||
            phases != "VRI" || runtime.activeGenerationId(ModuleId) != firstId)
            return fail("initializer failure changed the active generation");
        failedRequest.moduleLease.reset();

        Request secondRequest{
            ModuleId, std::string(64, '2'),
            std::make_shared<LeaseProbe>(secondLeaseDestructions)};
        Runtime::StagedGeneration secondStaged;
        phases.clear();
        if (!stageOne(runtime, secondRequest, SymbolId, ContractId, &second,
                      secondStaged, phases, error))
            return fail("second generation did not stage");
        const uint64_t secondId = secondStaged.generationId();
        auto secondSafePoint = runtime.safePoint();
        if (!runtime.activate(secondStaged, secondSafePoint, error) ||
            secondId <= firstId || implementationOf(switchable.pin()) != &second ||
            implementationOf(pinnedFirstBinding) != &first)
            return fail("activation did not distinguish switchable and pinned references");

        Request wrongKindRequest{
            ModuleId, std::string(64, 'e'),
            std::make_shared<LeaseProbe>(thirdLeaseDestructions)};
        Runtime::StagedGeneration wrongKindStaged;
        phases.clear();
        if (!stageOne(runtime, wrongKindRequest, SymbolId, ContractId,
                      &incompatible, wrongKindStaged, phases, error, true, 2,
                      0))
            return fail("wrong-kind generation staging precondition failed");
        auto wrongKindSafePoint = runtime.safePoint();
        if (runtime.activate(wrongKindStaged, wrongKindSafePoint, error) ||
            error.find("switchable binding") == std::string::npos ||
            runtime.activeGenerationId(ModuleId) != secondId)
            return fail("switchable typed reference accepted a changed declaration kind");

        Request incompatibleRequest{
            ModuleId, std::string(64, '3'),
            std::make_shared<LeaseProbe>(thirdLeaseDestructions)};
        Runtime::StagedGeneration incompatibleStaged;
        phases.clear();
        if (!stageOne(runtime, incompatibleRequest, SymbolId, "contract:v2",
                      &incompatible, incompatibleStaged, phases, error))
            return fail("incompatible generation staging precondition failed");
        auto incompatibleSafePoint = runtime.safePoint();
        if (runtime.activate(
                incompatibleStaged, incompatibleSafePoint, error) ||
            error.find("switchable binding") == std::string::npos ||
            runtime.activeGenerationId(ModuleId) != secondId ||
            implementationOf(switchable.pin()) != &second)
            return fail("failed binding validation changed the active generation");

        Runtime otherRuntime;
        auto wrongRuntimeSafePoint = otherRuntime.safePoint();
        if (runtime.activate(
                incompatibleStaged, wrongRuntimeSafePoint, error) ||
            error.find("same runtime") == std::string::npos)
            return fail("activation accepted another runtime's safe point");

        auto rollbackSafePoint = runtime.safePoint();
        if (!runtime.rollback(ModuleId, firstId, rollbackSafePoint, error) ||
            implementationOf(switchable.pin()) != &first ||
            implementationOf(pinnedFirstBinding) != &first)
            return fail("rollback did not restore the retained generation");

        Request thirdRequest{
            ModuleId, std::string(64, '4'),
            std::make_shared<LeaseProbe>(thirdLeaseDestructions)};
        Runtime::StagedGeneration thirdStaged;
        phases.clear();
        if (!stageOne(runtime, thirdRequest, SymbolId, ContractId, &third,
                      thirdStaged, phases, error))
            return fail("third generation did not stage");
        const uint64_t thirdId = thirdStaged.generationId();
        auto thirdSafePoint = runtime.safePoint();
        if (!runtime.activate(thirdStaged, thirdSafePoint, error))
            return fail("third generation did not activate");

        std::atomic<bool> stop{false};
        std::atomic<bool> startReaders{false};
        std::atomic<unsigned> readyReaders{0};
        std::atomic<bool> observedInvalid{false};
        std::atomic<unsigned> observedFirst{0};
        std::atomic<unsigned> observedSecond{0};
        std::atomic<unsigned> observedThird{0};
        std::vector<std::thread> readers;
        for (unsigned index = 0; index < 4; ++index) {
            readers.emplace_back([&] {
                readyReaders.fetch_add(1, std::memory_order_release);
                while (!startReaders.load(std::memory_order_acquire))
                    std::this_thread::yield();
                while (!stop.load(std::memory_order_acquire)) {
                    const auto binding = switchable.pin();
                    const auto* implementation = implementationOf(binding);
                    if (!binding ||
                        (binding.generationId() == firstId &&
                         implementation != &first) ||
                        (binding.generationId() == secondId &&
                         implementation != &second) ||
                        (binding.generationId() == thirdId &&
                         implementation != &third) ||
                        (binding.generationId() != firstId &&
                         binding.generationId() != secondId &&
                         binding.generationId() != thirdId)) {
                        observedInvalid.store(true, std::memory_order_release);
                        return;
                    }
                    if (binding.generationId() == firstId)
                        observedFirst.fetch_add(1, std::memory_order_relaxed);
                    else if (binding.generationId() == secondId)
                        observedSecond.fetch_add(1, std::memory_order_relaxed);
                    else
                        observedThird.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        while (readyReaders.load(std::memory_order_acquire) != 4)
            std::this_thread::yield();
        startReaders.store(true, std::memory_order_release);
        for (unsigned transition = 0; transition < 1000; ++transition) {
            auto safePoint = runtime.safePoint();
            const uint64_t target = transition % 3 == 0
                ? firstId : (transition % 3 == 1 ? secondId : thirdId);
            if (!runtime.rollback(ModuleId, target, safePoint, error)) {
                stop.store(true, std::memory_order_release);
                for (auto& reader : readers) reader.join();
                return fail("concurrent rollback transition failed");
            }
            std::this_thread::yield();
        }
        stop.store(true, std::memory_order_release);
        for (auto& reader : readers) reader.join();
        if (observedInvalid.load(std::memory_order_acquire))
            return fail("reader observed a partially activated generation");
        if (observedFirst.load() == 0 || observedSecond.load() == 0 ||
            observedThird.load() == 0)
            return fail("concurrency gate did not observe every activated generation");
        if (runtime.retainedGenerationCount(ModuleId) != 3)
            return fail("runtime reclaimed or duplicated retained code generations");

        pinnedFirst = {};
        pinnedFirstBinding = {};
        firstRequest.moduleLease.reset();
        secondRequest.moduleLease.reset();
        thirdRequest.moduleLease.reset();
        if (firstLeaseDestructions.load() != 1 ||
            secondLeaseDestructions.load() != 0 ||
            thirdLeaseDestructions.load() != 0)
            return fail("runtime reclaimed published generation leases");
    }

    if (firstLeaseDestructions.load() != 2 ||
        secondLeaseDestructions.load() != 1 ||
        thirdLeaseDestructions.load() != 3)
        return fail("runtime did not release all leases at process-scope teardown");
    return 0;
}
