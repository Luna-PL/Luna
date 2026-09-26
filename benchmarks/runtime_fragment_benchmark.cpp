#include "runtime/RuntimeFragment.h"
#include "runtime/RuntimeDescriptorABI.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <locale>
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

constexpr std::array<const char*, 10> CaseNames{
    "candidate_snapshot", "ref_plus_binding_set", "refs_plus_chain_4",
    "local_override_none", "safe_point_activate_and_pin", "dispatch_none",
    "dispatch_one", "dispatch_chain_2", "dispatch_chain_4", "dispatch_override_none"};

struct Sample {
    double nanoseconds;
    std::uint64_t checksum;
    DispatchCounters counters;
};

// One stable fixture per catalog size. Its explicit contexts and argument
// storage outlive every sample; fixture setup is never inside a timer.
class BenchmarkFixture {
public:
    explicit BenchmarkFixture(std::size_t fragmentRows) {
        auto rows = std::make_shared<std::vector<DescriptorRow>>(fragmentRows);
        for (std::size_t index = 0; index < rows->size(); ++index)
            fillDescriptor((*rows)[index], index);

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
        require(runtime.loadOnce(staged, generation, error),
                "could not load benchmark generation", error);

        require(luna::runtime::snapshotRuntimeFragmentCandidates(
                    generation, slot, candidates, error) &&
                    candidates.size() == MatchingCandidates,
                "benchmark candidate set is invalid", error);
        selected = candidates.at(0);
        require(selected != nullptr, "benchmark has no selected candidate");

        require(luna::runtime::makeRuntimeFragmentBindingSet({}, none, error),
                "could not build empty BindingSet", error);
        std::vector<RuntimeFragmentRef> selectedRefs;
        selectedRefs.push_back(makeRef(*selected, slot, error));
        require(luna::runtime::makeRuntimeFragmentBindingSet(
                    std::move(selectedRefs), one, error),
                "could not build selected BindingSet", error);
        chainTwo = makeChain(candidates, slot, 2, error);
        chainFour = makeChain(candidates, slot, 4, error);
        require(luna::runtime::makeRuntimeFragmentBindingOverride(
                    chainFour, slot, {}, overrideNone, error) &&
                    overrideNone.chainSize(slot) == 0 && chainFour.chainSize(slot) == 4,
                "could not build isolated local None override", error);
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
    }

    Sample sample(std::size_t caseIndex, std::size_t iterations) {
        checksum = 0;
        counters = {};
        double nanoseconds = 0;
        switch (caseIndex) {
        case 0:
            nanoseconds = measure(iterations, [&](std::size_t) {
                RuntimeFragmentCandidateSnapshot snapshot;
                require(luna::runtime::snapshotRuntimeFragmentCandidates(
                            generation, slot, snapshot, error),
                        "candidate query failed", error);
                checksum += snapshot.size();
            });
            break;
        case 1:
            nanoseconds = measure(iterations, [&](std::size_t) {
                std::vector<RuntimeFragmentRef> refs;
                refs.push_back(makeRef(*selected, slot, error));
                RuntimeFragmentBindingSet bindings;
                require(luna::runtime::makeRuntimeFragmentBindingSet(
                            std::move(refs), bindings, error),
                        "binding construction failed", error);
                checksum += bindings.bindingCount();
            });
            break;
        case 2:
            nanoseconds = measure(iterations, [&](std::size_t) {
                const auto bindings = makeChain(candidates, slot, 4, error);
                checksum += bindings.bindingCount();
            });
            break;
        case 3:
            nanoseconds = measure(iterations, [&](std::size_t) {
                RuntimeFragmentBindingSet bindings;
                require(luna::runtime::makeRuntimeFragmentBindingOverride(
                            chainFour, slot, {}, bindings, error) &&
                            bindings.chainSize(slot) == 0 && chainFour.chainSize(slot) == 4,
                        "local override construction changed its base", error);
                checksum += bindings.bindingCount() + 1;
            });
            break;
        case 4:
            nanoseconds = measure(iterations, [&](std::size_t index) {
                auto safePoint = runtime.safePoint();
                require(runtime.activateFragmentBindings(
                            index % 2 == 0 ? one : none, safePoint, error),
                        "binding activation failed", error);
                checksum += runtime.pinFragmentBindings().bindingCount();
            });
            break;
        case 5:
            nanoseconds = measure(iterations, [&](std::size_t) { dispatch(noneContext, 0); });
            break;
        case 6:
            nanoseconds = measure(iterations, [&](std::size_t) { dispatch(oneContext, 1); });
            break;
        case 7:
            nanoseconds = measure(iterations, [&](std::size_t) { dispatch(chainTwoContext, 2); });
            break;
        case 8:
            nanoseconds = measure(iterations, [&](std::size_t) { dispatch(chainFourContext, 4); });
            break;
        case 9:
            nanoseconds = measure(iterations, [&](std::size_t) { dispatch(overrideNoneContext, 0); });
            break;
        default:
            throw std::runtime_error("invalid benchmark case");
        }
        const auto warmup = std::min<std::size_t>(iterations, 1000);
        const auto calls = iterations + warmup;
        const std::array<std::uint64_t, 5> controlCounts{4, 1, 4, 1, 0};
        const std::array<std::uint64_t, 5> fragmentCounts{0, 1, 2, 4, 0};
        const auto expectedChecksum = caseIndex < 4 ? controlCounts[caseIndex] * calls
            : (caseIndex == 4 ? (iterations + 1) / 2 + (warmup + 1) / 2 : 0);
        const auto expectedContinuations = caseIndex >= 5 ? calls : 0;
        const auto expectedFragments = caseIndex >= 5 ? fragmentCounts[caseIndex - 5] * calls : 0;
        require(checksum == expectedChecksum && counters.continuations == expectedContinuations &&
                    counters.fragments == expectedFragments,
                "benchmark sample counters are invalid");
        return {nanoseconds, checksum, counters};
    }

private:
    void dispatch(const RuntimeFragmentExecutionContext& context,
                  std::uint64_t expectedFragments) {
        const auto beforeContinuations = counters.continuations;
        const auto beforeFragments = counters.fragments;
        const auto status = luna_runtime_fragment_dispatch_v1(
            context.opaque(), SlotId, SlotContractId, ArgumentsLayoutId,
            sizeof(argument), alignof(Arguments), &argument, baseContinuation, &counters);
        require(status == LUNA_RUNTIME_FRAGMENT_DISPATCH_SUCCESS_V1, "benchmark dispatch failed");
        require(counters.continuations == beforeContinuations + 1 &&
                    counters.fragments == beforeFragments + expectedFragments,
                "benchmark dispatch did not execute the selected policy");
    }

    MoonRuntime runtime;
    std::string error;
    MoonRuntime::PinnedGeneration generation;
    const RuntimeSlotRequirement slot{SlotId, SlotContractId};
    RuntimeFragmentCandidateSnapshot candidates;
    const MoonRuntime::PinnedBinding* selected = nullptr;
    RuntimeFragmentBindingSet none, one, chainTwo, chainFour, overrideNone;
    RuntimeFragmentExecutionContext noneContext, oneContext, chainTwoContext,
        chainFourContext, overrideNoneContext;
    std::uint64_t checksum = 0;
    DispatchCounters counters;
    const Arguments argument{42, &counters};
};

int run(std::size_t iterations, std::size_t fragmentRows) {
    BenchmarkFixture fixture(fragmentRows);
    std::uint64_t checksum = 0;
    DispatchCounters counters;
    std::cout << "runtime-fragment benchmark: iterations=" << iterations
              << ", fragment_rows=" << fragmentRows
              << ", matching_candidates=" << MatchingCandidates << '\n';
    for (std::size_t caseIndex = 0; caseIndex < CaseNames.size(); ++caseIndex) {
        const auto sample = fixture.sample(caseIndex, iterations);
        report(CaseNames[caseIndex], sample.nanoseconds);
        checksum += sample.checksum;
        counters.continuations += sample.counters.continuations;
        counters.fragments += sample.counters.fragments;
    }
    std::cout << "checksum=" << checksum
              << ", continuation_calls=" << counters.continuations
              << ", fragment_calls=" << counters.fragments << '\n';
    return 0;
}

int runInterleaved(std::size_t iterations, std::size_t rounds) {
    constexpr std::array<std::size_t, 3> RowCounts{4, 64, 256};
    constexpr auto SamplesPerRound = RowCounts.size() * CaseNames.size();
    std::array<std::unique_ptr<BenchmarkFixture>, RowCounts.size()> fixtures;
    for (std::size_t index = 0; index < fixtures.size(); ++index)
        fixtures[index] = std::make_unique<BenchmarkFixture>(RowCounts[index]);
    std::cout << "# protocol=luna.fragment-cost.interleaved.v1\n"
              << "# git_commit=" << LUNA_FRAGMENT_PROBE_GIT_COMMIT << '\n'
              << "# probe_sha256=" << LUNA_FRAGMENT_PROBE_SHA256 << '\n'
              << "# build_type=" << LUNA_FRAGMENT_PROBE_BUILD_TYPE << '\n'
              << "# compiler=" << LUNA_FRAGMENT_PROBE_COMPILER << '\n'
              << "# cxx=" << __cplusplus << '\n'
              << "# iterations=" << iterations << ",warmup="
              << std::min<std::size_t>(iterations, 1000) << ",rounds=" << rounds << '\n'
              << "# affinity=uncontrolled,power_policy=uncontrolled\n"
              << "round,position,fragment_rows,case,ns_per_op,checksum,continuation_calls,fragment_calls\n";
    std::array<std::array<unsigned, SamplesPerRound>, SamplesPerRound> visits{};
    for (std::size_t round = 0; round < rounds; ++round) {
        for (std::size_t position = 0; position < SamplesPerRound; ++position) {
            // Seven is coprime with thirty: over thirty rounds every fixture/
            // case pair occupies each temporal position exactly once.
            const auto pair = (position + round * 7) % SamplesPerRound;
            const auto fixtureIndex = pair / CaseNames.size();
            const auto caseIndex = pair % CaseNames.size();
            ++visits[pair][position];
            const auto sample = fixtures[fixtureIndex]->sample(caseIndex, iterations);
            std::cout << round + 1 << ',' << position + 1 << ',' << RowCounts[fixtureIndex]
                      << ',' << CaseNames[caseIndex] << ',' << std::fixed << std::setprecision(1)
                      << sample.nanoseconds << ',' << sample.checksum << ','
                      << sample.counters.continuations << ',' << sample.counters.fragments << '\n';
        }
    }
    if (rounds % SamplesPerRound == 0) {
        for (const auto& pair : visits)
            for (const auto count : pair)
                require(count == rounds / SamplesPerRound, "interleaved schedule is not balanced");
    }
    std::cout << "# verified_samples=" << rounds * SamplesPerRound
              << ",position_balanced=" << (rounds % SamplesPerRound == 0 ? "yes" : "no") << '\n';
    return 0;
}

std::size_t parseCount(const char* text, std::size_t minimum, std::size_t maximum) {
    const std::string value(text);
    std::size_t count = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), count);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        count < minimum || count > maximum)
        throw std::runtime_error("invalid benchmark count: " + value);
    return count;
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::cout.imbue(std::locale::classic());
        if (argc >= 2 && std::string(argv[1]) == "--interleaved") {
            if (argc > 4)
                throw std::runtime_error(
                    "usage: runtime-fragment-benchmark --interleaved [iterations] [1..300 rounds]");
            const auto iterations = argc >= 3 ? parseCount(argv[2], 1, 10000000) : 10000;
            const auto rounds = argc >= 4 ? parseCount(argv[3], 1, 300) : 30;
            return runInterleaved(iterations, rounds);
        }
        if (argc > 3)
            throw std::runtime_error(
                "usage: runtime-fragment-benchmark [1..10000000 iterations] [4..4096 fragment rows]");
        const auto iterations = argc >= 2 ? parseCount(argv[1], 1, 10000000) : 100000;
        const auto fragmentRows = argc >= 3 ? parseCount(argv[2], MatchingCandidates, 4096) : 64;
        return run(iterations, fragmentRows);
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
