#include "runtime/Evolution.h"
#include "runtime/RuntimeDescriptorABI.h"

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
              uint32_t flags = luna::runtime::GenerationBindingCallable,
              uint32_t entryAbi = luna::runtime::GenerationEntryAbiUnprofiled) {
    return runtime.stage(
        request,
        [&](const Request&, std::string&) {
            phases += 'V';
            return true;
        },
        [&](const Request&, std::vector<Binding>& bindings, std::string&) {
            phases += 'R';
            bindings.push_back({symbolId, contractId, implementation,
                                declarationKind, flags, entryAbi});
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
        const std::shared_ptr<const void> sameAddressDifferentOwner(
            request.moduleLease.get(), [](const void*) {});
        int alternateAddress = 0;
        const std::shared_ptr<const void> sameOwnerDifferentAddress(
            request.moduleLease, &alternateAddress);
        if (!loaded.matchesModuleLease(request.moduleLease) ||
            loaded.matchesModuleLease(sameAddressDifferentOwner) ||
            loaded.matchesModuleLease(sameOwnerDifferentAddress) ||
            Runtime::PinnedGeneration{}.matchesModuleLease(request.moduleLease))
            return fail("generation lease identity ignored ownership or address");
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

    // A staged generation's module exclusion is checked at publication, in
    // both directions, and again when rolling back to a retained generation.
    {
        constexpr const char* OwnerId = "org.luna.test.owner";
        Runtime runtime;
        Request consumer{ModuleId, std::string(64, 'c'),
                         std::make_shared<int>(1)};
        consumer.requiredAbsentModuleIds = {OwnerId};
        Request owner{OwnerId, std::string(64, 'd'),
                      std::make_shared<int>(2)};
        Runtime::StagedGeneration consumerStage;
        Runtime::StagedGeneration ownerStage;
        std::string phases;
        if (!stageOne(runtime, consumer, SymbolId, ContractId, &first,
                      consumerStage, phases, error) ||
            !stageOne(runtime, owner, "symbol:owner", ContractId, &second,
                      ownerStage, phases, error))
            return fail("module exclusion fixture did not stage");
        Runtime::PinnedGeneration ownerPin;
        if (!runtime.loadOnce(ownerStage, ownerPin, error))
            return fail("owner did not load after dependent staging");
        auto conflictPoint = runtime.safePoint();
        if (runtime.activate(consumerStage, conflictPoint, error) ||
            error != "generation conflicts with an active module" ||
            !consumerStage || runtime.activeGenerationId(ModuleId) != 0 ||
            runtime.activeGenerationId(OwnerId) != ownerPin.generationId())
            return fail("activation ignored a newly active excluded module");
    }
    {
        constexpr const char* OwnerId = "org.luna.test.owner";
        Runtime runtime;
        Request consumer{ModuleId, std::string(64, 'c'),
                         std::make_shared<int>(1)};
        consumer.requiredAbsentModuleIds = {OwnerId};
        Request owner{OwnerId, std::string(64, 'd'),
                      std::make_shared<int>(2)};
        Runtime::StagedGeneration consumerStage;
        Runtime::StagedGeneration ownerStage;
        std::string phases;
        if (!stageOne(runtime, consumer, SymbolId, ContractId, &first,
                      consumerStage, phases, error) ||
            !stageOne(runtime, owner, "symbol:owner", ContractId, &second,
                      ownerStage, phases, error))
            return fail("reverse module exclusion fixture did not stage");
        Runtime::PinnedGeneration consumerPin;
        if (!runtime.loadOnce(consumerStage, consumerPin, error))
            return fail("excluded-module consumer did not load");
        Request changedExclusion = consumer;
        changedExclusion.requiredAbsentModuleIds.clear();
        Runtime::StagedGeneration changedStage;
        if (!stageOne(runtime, changedExclusion, SymbolId, ContractId,
                      &first, changedStage, phases, error))
            return fail("changed module exclusion fixture did not stage");
        Runtime::PinnedGeneration rejectedCache;
        if (runtime.loadOnce(changedStage, rejectedCache, error) ||
            error != "same-content generation changed module exclusions" ||
            rejectedCache || !changedStage)
            return fail("load-once reused a generation with different exclusions");
        Runtime::PinnedGeneration rejectedOwner;
        if (runtime.loadOnce(ownerStage, rejectedOwner, error) ||
            error != "generation conflicts with an active module" ||
            rejectedOwner || !ownerStage)
            return fail("load-once ignored an active module exclusion");
        auto conflictPoint = runtime.safePoint();
        if (runtime.activate(ownerStage, conflictPoint, error) ||
            error != "generation conflicts with an active module" ||
            runtime.activeGenerationId(OwnerId) != 0)
            return fail("activation ignored the active generation's exclusion");
        Request replacement{ModuleId, std::string(64, 'e'),
                            std::make_shared<int>(3)};
        Runtime::StagedGeneration replacementStage;
        if (!stageOne(runtime, replacement, SymbolId, ContractId, &third,
                      replacementStage, phases, error))
            return fail("module exclusion replacement did not stage");
        const auto replacementId = replacementStage.generationId();
        auto replacePoint = runtime.safePoint();
        if (!runtime.activate(replacementStage, replacePoint, error))
            return fail("module exclusion replacement did not activate");
        Runtime::PinnedGeneration ownerPin;
        if (!runtime.loadOnce(ownerStage, ownerPin, error))
            return fail("owner did not load after exclusion became inactive");
        auto rollbackPoint = runtime.safePoint();
        if (runtime.rollback(ModuleId, consumerPin.generationId(),
                             rollbackPoint, error) ||
            error != "generation conflicts with an active module" ||
            runtime.activeGenerationId(ModuleId) != replacementId ||
            implementationOf(consumerPin.find(SymbolId, ContractId)) != &first)
            return fail("rollback restored an excluded module or lost the old pin");
    }
    {
        Runtime runtime;
        Request request{ModuleId, std::string(64, 'c'),
                        std::make_shared<int>(1)};
        for (const auto& invalid : {
                 std::vector<std::string>{ModuleId},
                 std::vector<std::string>{"org.luna.test.owner", "org.luna.test.owner"},
                 std::vector<std::string>{"bad\nmodule"}}) {
            request.requiredAbsentModuleIds = invalid;
            Runtime::StagedGeneration staged;
            std::string phases;
            if (stageOne(runtime, request, SymbolId, ContractId, &first,
                         staged, phases, error) || staged || !phases.empty() ||
                error != "generation required-absent module list is invalid")
                return fail("invalid module exclusion reached generation verification");
        }
    }
    {
        constexpr const char* OwnerId = "org.luna.test.owner";
        Runtime runtime;
        Request owner{OwnerId, std::string(64, 'a'),
                      std::make_shared<int>(1)};
        Runtime::StagedGeneration ownerStage;
        std::string phases;
        if (!stageOne(runtime, owner, "symbol:owner", ContractId, &first,
                      ownerStage, phases, error))
            return fail("required-active owner did not stage");
        Request consumer{ModuleId, std::string(64, 'b'),
                         std::make_shared<int>(2)};
        consumer.requiredActiveModules = {
            {OwnerId, ownerStage.generationId()}};
        Runtime::StagedGeneration consumerStage;
        if (!stageOne(runtime, consumer, SymbolId, ContractId, &second,
                      consumerStage, phases, error))
            return fail("required-active consumer did not stage");
        auto missingOwnerPoint = runtime.safePoint();
        if (runtime.activate(consumerStage, missingOwnerPoint, error) ||
            error != "generation requires a different active module generation" ||
            !consumerStage || runtime.activeGenerationId(ModuleId) != 0)
            return fail("consumer activated before its exact owner generation");
        Runtime::PinnedGeneration ownerPin;
        if (!runtime.loadOnce(ownerStage, ownerPin, error))
            return fail("required-active owner did not load");
        auto consumerPoint = runtime.safePoint();
        if (!runtime.activate(consumerStage, consumerPoint, error))
            return fail("consumer did not activate with its exact owner");
        const auto consumerPin = runtime.pin(ModuleId);
        Request changedDependency = consumer;
        changedDependency.requiredActiveModules.clear();
        Runtime::StagedGeneration changedStage;
        if (!stageOne(runtime, changedDependency, SymbolId, ContractId,
                      &second, changedStage, phases, error))
            return fail("changed dependency fixture did not stage");
        Runtime::PinnedGeneration rejectedCache;
        if (runtime.loadOnce(changedStage, rejectedCache, error) ||
            error != "same-content generation changed module dependencies" ||
            rejectedCache || !changedStage)
            return fail("load-once reused a generation with different dependencies");
        Request newOwner{OwnerId, std::string(64, 'c'),
                         std::make_shared<int>(3)};
        Runtime::StagedGeneration newOwnerStage;
        if (!stageOne(runtime, newOwner, "symbol:owner", ContractId, &third,
                      newOwnerStage, phases, error))
            return fail("replacement owner did not stage");
        auto blockedOwnerPoint = runtime.safePoint();
        if (runtime.activate(newOwnerStage, blockedOwnerPoint, error) ||
            error != "generation replacement breaks an active module dependency" ||
            !newOwnerStage ||
            runtime.activeGenerationId(OwnerId) != ownerPin.generationId())
            return fail("owner changed while an active consumer required it");
        Request inert{ModuleId, std::string(64, 'd'),
                      std::make_shared<int>(4)};
        Runtime::StagedGeneration inertStage;
        if (!stageOne(runtime, inert, SymbolId, ContractId, &third,
                      inertStage, phases, error))
            return fail("independent consumer replacement did not stage");
        auto inertPoint = runtime.safePoint();
        if (!runtime.activate(inertStage, inertPoint, error))
            return fail("independent consumer replacement did not activate");
        const auto inertId = runtime.activeGenerationId(ModuleId);
        auto newOwnerPoint = runtime.safePoint();
        if (!runtime.activate(newOwnerStage, newOwnerPoint, error))
            return fail("owner did not switch after its consumer became inactive");
        const auto newOwnerId = runtime.activeGenerationId(OwnerId);
        if (!newOwnerId || newOwnerId == ownerPin.generationId())
            return fail("owner replacement reused the previous generation");
        auto blockedRollback = runtime.safePoint();
        if (runtime.rollback(ModuleId, consumerPin.generationId(),
                             blockedRollback, error) ||
            error != "generation requires a different active module generation" ||
            runtime.activeGenerationId(ModuleId) != inertId ||
            implementationOf(consumerPin.find(SymbolId, ContractId)) != &second)
            return fail("rollback ignored a changed owner or lost the old pin");
        auto ownerRollback = runtime.safePoint();
        if (!runtime.rollback(OwnerId, ownerPin.generationId(),
                              ownerRollback, error) ||
            runtime.activeGenerationId(OwnerId) != ownerPin.generationId())
            return fail("owner did not roll back after consumer replacement");
        auto consumerRollback = runtime.safePoint();
        if (!runtime.rollback(ModuleId, consumerPin.generationId(),
                              consumerRollback, error) ||
            runtime.activeGenerationId(ModuleId) != consumerPin.generationId())
            return fail("consumer did not roll back with its exact owner restored");
    }
    {
        constexpr const char* OwnerId = "org.luna.test.owner";
        Runtime runtime;
        Request request{ModuleId, std::string(64, 'a'),
                        std::make_shared<int>(1)};
        for (const auto& invalid : {
                 std::vector<luna::runtime::GenerationActiveModuleRequirement>{{ModuleId, 1}},
                 std::vector<luna::runtime::GenerationActiveModuleRequirement>{{OwnerId, 0}},
                 std::vector<luna::runtime::GenerationActiveModuleRequirement>{{OwnerId, 1}, {OwnerId, 2}}}) {
            request.requiredActiveModules = invalid;
            Runtime::StagedGeneration staged;
            std::string phases;
            if (stageOne(runtime, request, SymbolId, ContractId, &first,
                         staged, phases, error) || staged || !phases.empty() ||
                error != "generation required-active module list is invalid")
                return fail("invalid module dependency reached generation verification");
        }
        request.requiredActiveModules = {{OwnerId, 1}};
        request.requiredAbsentModuleIds = {OwnerId};
        Runtime::StagedGeneration staged;
        std::string phases;
        if (stageOne(runtime, request, SymbolId, ContractId, &first,
                     staged, phases, error) || staged || !phases.empty() ||
            error != "generation required-active module list is invalid")
            return fail("conflicting module requirements reached verification");
    }

    // A v4 metadata pin needs a matching public Slot in the same generation;
    // it never becomes a callable binding by sharing a SymbolId.
    {
        Runtime runtime;
        Request request{ModuleId, std::string(64, 'a'),
                        std::make_shared<int>(1)};
        luna::runtime::GenerationRefUnitCandidate candidate{
            "symbol:ref-unit", "contract:ref-unit", "ref_unit_entry",
            ModuleId, ModuleId, "type:ref-unit-fn", "type:ref-slot",
            "symbol:slot", "contract:slot", "type:slot",
            luna::runtime::GenerationEntryAbiRefUnitV1, 1, 2, 1};
        request.refUnitCandidates.push_back(candidate);
        Runtime::StagedGeneration staged;
        std::string phases;
        if (!stageOne(runtime, request, "symbol:slot", "contract:slot",
                      &first, staged, phases, error, true,
                      LUNA_RUNTIME_DECLARATION_SLOT_V1, 0))
            return fail("matching Ref/unit Slot could not stage");
        Runtime::PinnedGeneration loaded;
        if (!runtime.loadOnce(staged, loaded, error) ||
            !loaded.findRefUnitCandidate(candidate.symbolId,
                                         candidate.contractId) ||
            loaded.find(candidate.symbolId, candidate.contractId))
            return fail("Ref/unit metadata did not stay separate from callable bindings");
        phases.clear();
        if (!stageOne(runtime, request, "symbol:slot", "contract:slot",
                      &first, staged, phases, error, true,
                      LUNA_RUNTIME_DECLARATION_SLOT_V1, 0) ||
            runtime.loadOnce(staged, loaded, error) || !staged ||
            error != "load-once output already owns a generation")
            return fail("Ref/unit metadata reuse changed a pinned output");
        loaded = {};
        if (runtime.loadOnce(staged, loaded, error) || loaded || !staged ||
            error != "Ref/unit candidate cannot use the load-once cache")
            return fail("Ref/unit metadata reused a load-once cache entry");
        staged = {};
        request.refUnitCandidates.front().slotContractId = "contract:other";
        phases.clear();
        if (stageOne(runtime, request, "symbol:slot", "contract:slot",
                     &first, staged, phases, error, true,
                     LUNA_RUNTIME_DECLARATION_SLOT_V1, 0) ||
            staged || phases != "VR" || error.empty())
            return fail("Ref/unit metadata accepted a mismatched public Slot");
    }

    // A typed adapter needs the exact module lease and an explicit safe-point
    // activation; a cache hit must never substitute a different adapter.
    Runtime::PinnedRefUnitCandidate retainedCandidate;
    std::weak_ptr<const void> retainedLease;
    {
        Runtime runtime;
        Request request{ModuleId, std::string(64, 'a'),
                        std::make_shared<int>(1)};
        retainedLease = request.moduleLease;
        const luna::runtime::GenerationRefUnitCandidate candidate{
            "symbol:ref-unit", "contract:ref-unit", "ref_unit_entry",
            ModuleId, ModuleId, "type:ref-unit-fn", "type:ref-slot",
            "symbol:slot", "contract:slot", "type:slot",
            luna::runtime::GenerationEntryAbiRefUnitV1, 1, 2, 1};
        request.refUnitCandidates.push_back(candidate);
        request.refUnitCaller = [](
            const std::string&, const std::string&,
            const luna::runtime::RuntimeFragmentExecutionContext&,
            const luna::runtime::RuntimeFragmentRefHandle&,
            int32_t& status, std::string& callError) {
            callError.clear();
            status = 0;
            return true;
        };
        Runtime::StagedGeneration staged;
        std::string phases;
        if (stageOne(runtime, request, "symbol:slot", "contract:slot",
                     &first, staged, phases, error, true,
                     LUNA_RUNTIME_DECLARATION_SLOT_V1, 0) ||
            staged || !phases.empty() ||
            error != "generation Ref/unit call adapter has no exact module lease")
            return fail("Ref/unit adapter staged without its lease witness");
        const std::shared_ptr<const void> differentOwner(
            request.moduleLease.get(), [](const void*) {});
        request.refUnitCallerLease = differentOwner;
        if (stageOne(runtime, request, "symbol:slot", "contract:slot",
                     &first, staged, phases, error, true,
                     LUNA_RUNTIME_DECLARATION_SLOT_V1, 0) ||
            staged || !phases.empty())
            return fail("Ref/unit adapter accepted a separate lease owner");
        int alternateAddress = 0;
        const std::shared_ptr<const void> differentAddress(
            request.moduleLease, &alternateAddress);
        request.refUnitCallerLease = differentAddress;
        if (stageOne(runtime, request, "symbol:slot", "contract:slot",
                     &first, staged, phases, error, true,
                     LUNA_RUNTIME_DECLARATION_SLOT_V1, 0) ||
            staged || !phases.empty())
            return fail("Ref/unit adapter accepted a different lease address");
        request.refUnitCallerLease = request.moduleLease;
        if (!stageOne(runtime, request, "symbol:slot", "contract:slot",
                      &first, staged, phases, error, true,
                      LUNA_RUNTIME_DECLARATION_SLOT_V1, 0))
            return fail("Ref/unit adapter with exact lease did not stage");
        Runtime::PinnedGeneration loaded;
        if (runtime.loadOnce(staged, loaded, error) || loaded || !staged ||
            runtime.pin(ModuleId) ||
            error != "callable Ref/unit candidate requires explicit activation")
            return fail("callable Ref/unit adapter entered load-once");
        auto safePoint = runtime.safePoint();
        if (!runtime.activate(staged, safePoint, error))
            return fail("callable Ref/unit adapter did not activate explicitly");
        loaded = runtime.pin(ModuleId);
        retainedCandidate = loaded.findRefUnitCandidate(
            candidate.symbolId, candidate.contractId);
        if (!retainedCandidate)
            return fail("activated Ref/unit adapter lost its candidate");
        loaded = {};
        request.refUnitCaller = {};
        request.refUnitCallerLease.reset();
        request.moduleLease.reset();
    }
    const auto retainedFacts = retainedCandidate.facts();
    if (retainedLease.expired() || !retainedFacts ||
        retainedFacts->symbolId != "symbol:ref-unit")
        return fail("Ref/unit adapter lost its pinned lease after Runtime teardown");
    retainedCandidate = {};
    if (!retainedLease.expired())
        return fail("Ref/unit adapter retained its lease after pin release");

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

        Runtime typedRuntime;
        Runtime::StagedGeneration profiledStage;
        phases.clear();
        if (!stageOne(typedRuntime, request, SymbolId, ContractId, &first,
                      profiledStage, phases, error, true, 1,
                      luna::runtime::GenerationBindingCallable,
                      luna::runtime::GenerationEntryAbiCI32NoArgsV1))
            return fail("runtime rejected a valid profiled function binding");
        Runtime::PinnedGeneration profiled;
        if (!typedRuntime.loadOnce(profiledStage, profiled, error))
            return fail("profiled generation did not load once");
        auto exactProfile = typedFunction;
        exactProfile.entryAbi =
            luna::runtime::GenerationEntryAbiCI32NoArgsV1;
        auto exactUnprofiled = typedFunction;
        exactUnprofiled.entryAbi =
            luna::runtime::GenerationEntryAbiUnprofiled;
        if (!profiled.find(exactProfile) ||
            !profiled.find(typedFunction) ||
            profiled.find(exactUnprofiled) ||
            profiled.find(exactProfile).entryAbi() !=
                luna::runtime::GenerationEntryAbiCI32NoArgsV1)
            return fail("profiled generation requirement was not exact");
        Runtime::SwitchableBinding profiledSwitchable;
        if (!typedRuntime.makeSwitchable(ModuleId, exactProfile,
                                         profiledSwitchable, error))
            return fail("profiled switchable requirement was rejected");
        Runtime::SwitchableBinding legacySwitchable;
        if (!typedRuntime.makeSwitchable(ModuleId, typedFunction,
                                         legacySwitchable, error))
            return fail("legacy requirement did not capture the active profile");
        Request changedProfileRequest = request;
        changedProfileRequest.contentDigest = std::string(64, 'c');
        Runtime::StagedGeneration unprofiledReplacement;
        phases.clear();
        if (!stageOne(typedRuntime, changedProfileRequest, SymbolId,
                      ContractId, &second, unprofiledReplacement,
                      phases, error))
            return fail("unprofiled replacement fixture did not stage");
        auto profileSafePoint = typedRuntime.safePoint();
        if (typedRuntime.activate(unprofiledReplacement,
                                  profileSafePoint, error) ||
            error.find("switchable binding") == std::string::npos ||
            profiledSwitchable.pin().entryAbi() !=
                luna::runtime::GenerationEntryAbiCI32NoArgsV1 ||
            legacySwitchable.pin().entryAbi() !=
                luna::runtime::GenerationEntryAbiCI32NoArgsV1)
            return fail("profiled switchable accepted an unprofiled generation");
        Runtime::StagedGeneration unknownProfile;
        phases.clear();
        if (stageOne(typedRuntime, request, "symbol:unknown-profile",
                     ContractId, &first, unknownProfile, phases, error,
                     true, 1, luna::runtime::GenerationBindingCallable, 99) ||
            error.find("invalid or duplicate binding") == std::string::npos)
            return fail("runtime accepted an unknown entry profile");

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
