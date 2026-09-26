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

int32_t baseContinuation(void* context) {
    ++*static_cast<std::uint64_t*>(context);
    return LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1;
}

void executeResumingFragment(void*, void* activation) {
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
        sizeof(std::int32_t),
        alignof(std::int32_t),
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
    RuntimeFragmentExecutionContext noneContext;
    RuntimeFragmentExecutionContext oneContext;
    require(luna::runtime::makeRuntimeFragmentExecutionContext(
                none, noneContext, error) &&
                luna::runtime::makeRuntimeFragmentExecutionContext(
                    one, oneContext, error),
            "could not build dispatch contexts", error);

    std::uint64_t checksum = 0;
    std::uint64_t continuationCalls = 0;
    const std::int32_t argument = 42;
    auto dispatch = [&](const RuntimeFragmentExecutionContext& context) {
        const auto status = luna_runtime_fragment_dispatch_v1(
            context.opaque(), SlotId, SlotContractId, ArgumentsLayoutId,
            sizeof(argument), alignof(std::int32_t), &argument,
            baseContinuation, &continuationCalls);
        require(status == LUNA_RUNTIME_FRAGMENT_DISPATCH_SUCCESS_V1,
                "benchmark dispatch failed");
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
    report("safe_point_activate_and_pin", measure(iterations, [&](std::size_t index) {
        auto safePoint = runtime.safePoint();
        require(runtime.activateFragmentBindings(
                    index % 2 == 0 ? one : none, safePoint, error),
                "binding activation failed", error);
        checksum += runtime.pinFragmentBindings().bindingCount();
    }));
    report("dispatch_none", measure(iterations, [&](std::size_t) {
        dispatch(noneContext);
    }));
    report("dispatch_one", measure(iterations, [&](std::size_t) {
        dispatch(oneContext);
    }));
    require(continuationCalls ==
                2 * (iterations + std::min<std::size_t>(iterations, 1000)),
            "dispatch lost a continuation call");
    std::cout << "checksum=" << checksum
              << ", continuation_calls=" << continuationCalls << '\n';
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
