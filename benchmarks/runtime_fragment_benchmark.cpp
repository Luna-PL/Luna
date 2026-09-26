#include "runtime/RuntimeFragment.h"
#include "runtime/RuntimeDescriptorABI.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using luna::runtime::MoonRuntime;
using luna::runtime::RuntimeFragmentBindingSet;
using luna::runtime::RuntimeFragmentCandidateSnapshot;
using luna::runtime::RuntimeFragmentExecutionContext;
using luna::runtime::RuntimeFragmentRef;
using luna::runtime::RuntimeSlotRequirement;

constexpr std::size_t MatchingCandidates = 4;
constexpr const char* SlotId = "benchmark.slot";
constexpr const char* SlotContractId = "benchmark.slot.contract";
constexpr const char* ArgumentsLayoutId = "benchmark.slot.arguments";

struct DescriptorRow {
    std::string fragmentId;
    std::string contractId;
    std::string slotId;
    LunaRuntimeFragmentDescriptorV1 descriptor{};
};

struct DispatchCounters {
    std::uint64_t continuations = 0;
    std::uint64_t fragments = 0;
};

struct Arguments {
    std::int32_t value;
    DispatchCounters* counters;
};

int32_t baseContinuation(void* context) {
    ++static_cast<DispatchCounters*>(context)->continuations;
    return LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1;
}

void executeResumingFragment(void*, void* activation) {
    const auto* arguments = static_cast<const Arguments*>(
        luna_runtime_fragment_activation_arguments_v1(
            activation, SlotId, SlotContractId, ArgumentsLayoutId,
            sizeof(Arguments), alignof(Arguments)));
    if (!arguments || arguments->value != 42 || !arguments->counters)
        std::abort();
    ++arguments->counters->fragments;
    if (luna_runtime_fragment_activation_resume_v1(activation) !=
        LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1)
        std::abort();
}

void require(bool condition, const char* message,
             const std::string& error = {}) {
    if (!condition)
        throw std::runtime_error(message + (error.empty() ? "" : ": " + error));
}

template <typename Function>
double measure(std::size_t iterations, Function&& function) {
    const auto warmup = std::min<std::size_t>(iterations, 1000);
    for (std::size_t index = 0; index < warmup; ++index) function(index);
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < iterations; ++index) function(index);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return std::chrono::duration<double, std::nano>(elapsed).count() /
        static_cast<double>(iterations);
}

void report(const char* name, double nanoseconds) {
    std::cout << std::left << std::setw(29) << name << std::right
              << std::fixed << std::setprecision(1) << nanoseconds
              << " ns/op\n";
}

void fillDescriptor(DescriptorRow& row, std::size_t index) {
    row.fragmentId = "benchmark.fragment." + std::to_string(index);
    row.contractId = "benchmark.fragment.contract." + std::to_string(index);
    row.slotId = index < MatchingCandidates
        ? SlotId : "benchmark.other.slot." + std::to_string(index);
    row.descriptor = {
        LUNA_RUNTIME_FRAGMENT_MAGIC_V1,
        LUNA_RUNTIME_FRAGMENT_ABI_V1,
        sizeof(LunaRuntimeFragmentDescriptorV1),
        LUNA_RUNTIME_FRAGMENT_CAPTURE_FREE_V1,
        0, 0, 0, 0,
        row.fragmentId.c_str(),
        row.contractId.c_str(),
        row.slotId.c_str(),
        SlotContractId,
        ArgumentsLayoutId,
        sizeof(Arguments),
        alignof(Arguments),
        "",
        "benchmark.empty.environment",
        0,
        1,
        nullptr,
        nullptr,
        executeResumingFragment,
    };
}

RuntimeFragmentRef makeRef(
    const MoonRuntime::PinnedBinding& binding,
    const RuntimeSlotRequirement& slot,
    std::string& error) {
    RuntimeFragmentRef ref;
    require(luna::runtime::makeOwnedRuntimeFragmentRef(
                binding, slot, {}, ref, error),
            "could not create benchmark Fragment ref", error);
    return ref;
}

RuntimeFragmentBindingSet makeChain(
    const RuntimeFragmentCandidateSnapshot& candidates,
    const RuntimeSlotRequirement& slot, std::size_t length,
    std::string& error) {
    std::vector<RuntimeFragmentRef> refs;
    for (std::size_t index = 0; index < length; ++index) {
        const auto* binding = candidates.at(index);
        require(binding != nullptr, "benchmark chain has no candidate");
        refs.push_back(makeRef(*binding, slot, error));
    }
    RuntimeFragmentBindingSet chain;
    require(luna::runtime::makeRuntimeFragmentChainBindingSet(
                std::move(refs), chain, error),
            "could not build benchmark chain", error);
    require(chain.chainSize(slot) == length, "benchmark chain length is invalid");
    return chain;
}

int run(std::size_t iterations, std::size_t fragmentRows) {
    auto rows = std::make_shared<std::vector<DescriptorRow>>(fragmentRows);
    for (std::size_t index = 0; index < rows->size(); ++index)
        fillDescriptor((*rows)[index], index);

    MoonRuntime runtime;
    std::string error;
    const luna::runtime::GenerationStagingRequest request{
        "benchmark.fragment.module", std::string(64, 'b'), rows};
    MoonRuntime::StagedGeneration staged;
    require(runtime.stage(
                request,
                [](const auto&, std::string&) { return true; },
                [rows](const auto&, auto& bindings, std::string&) {
                    for (const auto& row : *rows)
                        bindings.push_back({
                            row.fragmentId,
                            row.contractId,
                            &row.descriptor,
                            LUNA_RUNTIME_DECLARATION_FRAGMENT_V1,
                            luna::runtime::GenerationBindingFragmentExecutable |
                                luna::runtime::GenerationBindingPublicControl});
                    return true;
                },
                {}, staged, error),
            "could not stage benchmark generation", error);
    MoonRuntime::PinnedGeneration generation;
    require(runtime.loadOnce(staged, generation, error),
            "could not load benchmark generation", error);

    const RuntimeSlotRequirement slot{SlotId, SlotContractId};
    RuntimeFragmentCandidateSnapshot candidates;
    require(luna::runtime::snapshotRuntimeFragmentCandidates(
                generation, slot, candidates, error) &&
                candidates.size() == MatchingCandidates,
            "benchmark candidate set is invalid", error);
    const auto* selected = candidates.at(0);
    require(selected != nullptr, "benchmark has no selected candidate");

    RuntimeFragmentBindingSet none;
    require(luna::runtime::makeRuntimeFragmentBindingSet({}, none, error),
            "could not build empty BindingSet", error);
    std::vector<RuntimeFragmentRef> selectedRefs;
    selectedRefs.push_back(makeRef(*selected, slot, error));
    RuntimeFragmentBindingSet one;
    require(luna::runtime::makeRuntimeFragmentBindingSet(
                std::move(selectedRefs), one, error),
            "could not build selected BindingSet", error);
    const auto chainTwo = makeChain(candidates, slot, 2, error);
    const auto chainFour = makeChain(candidates, slot, 4, error);
    RuntimeFragmentBindingSet overrideNone;
    require(luna::runtime::makeRuntimeFragmentBindingOverride(
                chainFour, slot, {}, overrideNone, error) &&
                overrideNone.chainSize(slot) == 0 && chainFour.chainSize(slot) == 4,
            "could not build isolated local None override", error);
    RuntimeFragmentExecutionContext noneContext;
    RuntimeFragmentExecutionContext oneContext;
    RuntimeFragmentExecutionContext chainTwoContext;
    RuntimeFragmentExecutionContext chainFourContext;
    RuntimeFragmentExecutionContext overrideNoneContext;
    require(luna::runtime::makeRuntimeFragmentExecutionContext(
                none, noneContext, error) &&
                luna::runtime::makeRuntimeFragmentExecutionContext(
                    one, oneContext, error) &&
                luna::runtime::makeRuntimeFragmentExecutionContext(
                    chainTwo, chainTwoContext, error) &&
                luna::runtime::makeRuntimeFragmentExecutionContext(
                    chainFour, chainFourContext, error) &&
                luna::runtime::makeRuntimeFragmentExecutionContext(
                    overrideNone, overrideNoneContext, error),
            "could not build dispatch contexts", error);

    std::uint64_t checksum = 0;
    DispatchCounters counters;
    const Arguments argument{42, &counters};
    auto dispatch = [&](const RuntimeFragmentExecutionContext& context,
                        std::uint64_t expectedFragments) {
        const auto beforeContinuations = counters.continuations;
        const auto beforeFragments = counters.fragments;
        const auto status = luna_runtime_fragment_dispatch_v1(
            context.opaque(), SlotId, SlotContractId, ArgumentsLayoutId,
            sizeof(argument), alignof(Arguments), &argument,
            baseContinuation, &counters);
        require(status == LUNA_RUNTIME_FRAGMENT_DISPATCH_SUCCESS_V1,
                "benchmark dispatch failed");
        require(counters.continuations == beforeContinuations + 1 &&
                    counters.fragments == beforeFragments + expectedFragments,
                "benchmark dispatch did not execute the selected policy");
    };

    std::cout << "runtime-fragment benchmark: iterations=" << iterations
              << ", fragment_rows=" << fragmentRows
              << ", matching_candidates=" << MatchingCandidates << '\n';
    report("candidate_snapshot", measure(iterations, [&](std::size_t) {
        RuntimeFragmentCandidateSnapshot snapshot;
        require(luna::runtime::snapshotRuntimeFragmentCandidates(
                    generation, slot, snapshot, error),
                "candidate query failed", error);
        checksum += snapshot.size();
    }));
    report("ref_plus_binding_set", measure(iterations, [&](std::size_t) {
        std::vector<RuntimeFragmentRef> refs;
        refs.push_back(makeRef(*selected, slot, error));
        RuntimeFragmentBindingSet bindings;
        require(luna::runtime::makeRuntimeFragmentBindingSet(
                    std::move(refs), bindings, error),
                "binding construction failed", error);
        checksum += bindings.bindingCount();
    }));
    report("refs_plus_chain_4", measure(iterations, [&](std::size_t) {
        const auto bindings = makeChain(candidates, slot, 4, error);
        checksum += bindings.bindingCount();
    }));
    report("local_override_none", measure(iterations, [&](std::size_t) {
        RuntimeFragmentBindingSet bindings;
        require(luna::runtime::makeRuntimeFragmentBindingOverride(
                    chainFour, slot, {}, bindings, error) &&
                    bindings.chainSize(slot) == 0 && chainFour.chainSize(slot) == 4,
                "local override construction changed its base", error);
        checksum += bindings.bindingCount() + 1;
    }));
    report("safe_point_activate_and_pin", measure(iterations, [&](std::size_t index) {
        auto safePoint = runtime.safePoint();
        require(runtime.activateFragmentBindings(
                    index % 2 == 0 ? one : none, safePoint, error),
                "binding activation failed", error);
        checksum += runtime.pinFragmentBindings().bindingCount();
    }));
    report("dispatch_none", measure(iterations, [&](std::size_t) {
        dispatch(noneContext, 0);
    }));
    report("dispatch_one", measure(iterations, [&](std::size_t) {
        dispatch(oneContext, 1);
    }));
    report("dispatch_chain_2", measure(iterations, [&](std::size_t) {
        dispatch(chainTwoContext, 2);
    }));
    report("dispatch_chain_4", measure(iterations, [&](std::size_t) {
        dispatch(chainFourContext, 4);
    }));
    report("dispatch_override_none", measure(iterations, [&](std::size_t) {
        dispatch(overrideNoneContext, 0);
    }));
    const auto warmup = std::min<std::size_t>(iterations, 1000);
    const auto calls = iterations + warmup;
    require(counters.continuations == 5 * calls,
            "dispatch lost a continuation call");
    require(counters.fragments == 7 * calls,
            "dispatch lost a selected Fragment call");
    require(checksum == 10 * calls + (iterations + 1) / 2 + (warmup + 1) / 2,
            "benchmark control-plane checksum is invalid");
    std::cout << "checksum=" << checksum
              << ", continuation_calls=" << counters.continuations
              << ", fragment_calls=" << counters.fragments << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto iterations = argc == 1
            ? std::size_t{100000} : static_cast<std::size_t>(std::stoull(argv[1]));
        const auto fragmentRows = argc < 3
            ? std::size_t{64} : static_cast<std::size_t>(std::stoull(argv[2]));
        if (argc > 3 || iterations == 0 || iterations > 10000000 ||
            fragmentRows < MatchingCandidates || fragmentRows > 4096)
            throw std::runtime_error(
                "usage: runtime-fragment-benchmark [1..10000000 iterations] "
                "[4..4096 fragment rows]");
        return run(iterations, fragmentRows);
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
