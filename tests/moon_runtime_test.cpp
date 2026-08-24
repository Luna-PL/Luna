#include "runtime/MoonRuntime.h"

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
using Request = luna::runtime::GenerationStagingRequest;
using Binding = luna::runtime::GenerationBinding;

bool stageOne(Runtime& runtime, const Request& request,
              const std::string& symbolId, const std::string& contractId,
              const void* implementation, Runtime::StagedGeneration& staged,
              std::string& phases, std::string& error,
              bool initializerSucceeds = true) {
    return runtime.stage(
        request,
        [&](const Request&, std::string&) {
            phases += 'V';
            return true;
        },
        [&](const Request&, std::vector<Binding>& bindings, std::string&) {
            phases += 'R';
            bindings.push_back({symbolId, contractId, implementation});
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
        thirdLeaseDestructions.load() != 2)
        return fail("runtime did not release all leases at process-scope teardown");
    return 0;
}
