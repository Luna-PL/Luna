#include "fragment_thread_affinity.h"
#include "compiled_fragment_benchmark.h"

#include "diagnostics/Diagnostic.h"
#include "driver/CompilerPipeline.h"
#include "driver/MoonGeneration.h"
#include "moonir/ContainerModel.h"
#include "runtime/RuntimeFragment.h"

#include <llvm/Config/llvm-config.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/Support/Compiler.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <locale>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace luna::benchmarks {
namespace {
using Clock = std::chrono::steady_clock;
using runtime::MoonRuntime;
using runtime::RuntimeFragmentBindingSet;
using runtime::RuntimeFragmentExecutionContext;
using runtime::RuntimeFragmentRef;
using driver::MoonJitOptimization;
constexpr std::array<MoonJitOptimization, 3> Profiles{
    MoonJitOptimization::O0, MoonJitOptimization::O2, MoonJitOptimization::O3};

const char* profileName(MoonJitOptimization profile) {
    switch (profile) {
    case MoonJitOptimization::O0: return "O0";
    case MoonJitOptimization::O2: return "O2";
    case MoonJitOptimization::O3: return "O3";
    }
    throw std::runtime_error("invalid compiled probe profile");
}
constexpr std::array<const char*, 9> Cases{
    "plain", "private_erased", "static_resume", "static_discard", "dynamic_none",
    "dynamic_one", "dynamic_chain_2", "dynamic_chain_4", "dynamic_override_none"};

void require(bool condition, const std::string& message, const std::string& error = {}) {
    if (!condition) throw std::runtime_error(message + (error.empty() ? "" : ": " + error));
}

// ORC entries lack native compiler-emitted UBSan function metadata. Exempt
// only these indirect JIT call boundaries, not compiler/runtime validation.
#if defined(__clang__)
LLVM_NO_SANITIZE("function")
#endif
int32_t invokePlain(const void* entry, int32_t value) {
    return reinterpret_cast<int32_t (*)(int32_t)>(const_cast<void*>(entry))(value);
}
#if defined(__clang__)
LLVM_NO_SANITIZE("function")
#endif
int32_t invokeDynamic(const void* entry, const void* context, int32_t value) {
    return reinterpret_cast<int32_t (*)(const void*, int32_t)>(
        const_cast<void*>(entry))(context, value);
}

double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

const moon::DeclarationRecord& findRecord(const moon::Module& module,
                                        moon::DeclarationKind kind, const std::string& name) {
    const auto found = std::find_if(module.declarationTable.begin(), module.declarationTable.end(),
        [&](const auto& row) { return row.kind == kind && row.sourceName == name; });
    require(found != module.declarationTable.end(), "missing compiled workload declaration: " + name);
    return *found;
}

void compile(const std::filesystem::path& path, driver::CompilerPipeline& pipeline) {
    driver::CompilerPipelineOptions options;
    options.inputPath = path.string();
    options.optimizationLevel = LunaOptimizationLevel::O2;
    if (pipeline.compileToMoonIR(options)) return;
    std::string error;
    for (const auto& item : pipeline.errors()) error += diagnostic::render(item) + "\n";
    throw std::runtime_error("compiled Fragment fixture failed: " + error);
}

struct Sample { double nanoseconds; uint64_t calls; uint64_t checksum; };

class Fixture {
public:
    explicit Fixture(MoonJitOptimization profile, bool observeSetup = false) {
        const auto compileStart = observeSetup ? Clock::now() : Clock::time_point{};
        const auto root = std::filesystem::path(LUNA_TEST_SOURCE_DIR) / "benchmarks/compiled_fragment";
        driver::CompilerPipeline hostPipeline, pluginPipeline;
        compile(root / "host", hostPipeline);
        compile(root / "plugin", pluginPipeline);
        initializeLunaLLVMTargets();
        auto target = llvm::orc::JITTargetMachineBuilder::detectHost();
        require(static_cast<bool>(target), "compiled probe could not detect host");
        auto layout = target->getDefaultDataLayoutForTarget();
        require(static_cast<bool>(layout), "compiled probe could not detect data layout");
        targetTriple = target->getTargetTriple().str();
        dataLayout = layout->getStringRepresentation();
        const auto manifestFor = [&](const moon::Module& module) {
            moon::ContainerManifest manifest;
            manifest.packageId = module.name;
            manifest.packageVersion = "0.3.0";
            manifest.packageKind = moon::ContainerPackageKind::Library;
            manifest.targetTriple = targetTriple;
            manifest.dataLayout = dataLayout;
            manifest.features = module.features;
            return manifest;
        };
        const auto hostManifest = manifestFor(hostPipeline.moonModule());
        const auto pluginManifest = manifestFor(pluginPipeline.moonModule());
        std::string error;
        std::vector<uint8_t> hostBytes, pluginBytes;
        require(moon::ContainerModelCodec::encodeContainer(hostManifest,
            hostPipeline.moonModule(), hostBytes, error), "host encode failed", error);
        require(moon::ContainerModelCodec::encodeContainer(pluginManifest,
            pluginPipeline.moonModule(), pluginBytes, error), "plugin encode failed", error);
        moon::ContainerManifest decodedManifest;
        moon::Module owner;
        require(moon::ContainerModelCodec::decodeContainerForTarget(hostBytes,
            targetTriple, dataLayout, decodedManifest, owner, error), "owner decode failed", error);
        const moon::SlotPublicationDependencies dependencies{owner.localSlotPublication};
        const auto& slot = findRecord(owner, moon::DeclarationKind::Slot, "open_hook");
        requirement = {slot.symbolId.value, slot.contractId.value};
        if (observeSetup) compileEncodeMs = milliseconds(compileStart);
        const auto loadStart = observeSetup ? Clock::now() : Clock::time_point{};
        MoonRuntime runtime;
        MoonRuntime::PinnedGeneration host, plugin;
        require(driver::loadVerifiedMoonGenerationOnce(runtime, hostBytes,
            targetTriple, dataLayout, host, error, {}, profile), "host verified load failed", error);
        require(driver::loadVerifiedMoonGenerationOnce(runtime, pluginBytes,
            targetTriple, dataLayout, plugin, error, dependencies, profile), "plugin verified load failed", error);
        require(host.generationId() != plugin.generationId(), "compiled generations were not independent");
        hostDigest = host.contentDigest();
        pluginDigest = plugin.contentDigest();
        materializationKey = host.materializationKey();
        require(!materializationKey.empty() && materializationKey == plugin.materializationKey(),
            "host/plugin JIT configuration identity mismatch");
        if (observeSetup) loadMs = milliseconds(loadStart);
        const auto bindStart = observeSetup ? Clock::now() : Clock::time_point{};
        for (size_t index = 0; index < 4; ++index) {
            const auto& row = findRecord(owner, moon::DeclarationKind::Function, Cases[index]);
            entries[index] = host.find(row.symbolId.value, row.contractId.value);
            require(entries[index] && entries[index].implementation() &&
                (entries[index].flags() & runtime::GenerationBindingFragmentContext) == 0,
                "baseline unexpectedly requires a context");
        }
        const auto& dynamic = findRecord(owner, moon::DeclarationKind::Function, "dynamic_probe");
        entries[4] = host.find(dynamic.symbolId.value, dynamic.contractId.value);
        require(entries[4] && entries[4].implementation() &&
            (entries[4].flags() & runtime::GenerationBindingFragmentContext) != 0,
            "dynamic workload lost context ABI");
        for (size_t index = 5; index < entries.size(); ++index) entries[index] = entries[4];
        runtime::RuntimeFragmentCandidateSnapshot candidates;
        require(runtime::snapshotRuntimeFragmentCandidates(plugin, requirement, candidates, error) &&
            candidates.size() == 4, "compiled candidate discovery failed", error);
        // Host policy names its order explicitly; discovery order is not policy.
        std::array<const MoonRuntime::PinnedBinding*, 4> chosen{};
        const std::array<const char*, 4> names{"resume_a", "resume_b", "resume_c", "resume_d"};
        for (size_t index = 0; index < names.size(); ++index) {
            const auto& row = findRecord(pluginPipeline.moonModule(), moon::DeclarationKind::Fragment, names[index]);
            for (size_t candidate = 0; candidate < candidates.size(); ++candidate)
                if (candidates.at(candidate)->symbolId() == row.symbolId.value &&
                    candidates.at(candidate)->contractId() == row.contractId.value)
                    chosen[index] = candidates.at(candidate);
            require(chosen[index] != nullptr, "host-named candidate was missing");
        }
        const auto chain = [&](size_t length, int32_t mask) {
            std::vector<RuntimeFragmentRef> refs;
            for (size_t index = 0; index < length; ++index) {
                const auto* descriptor = static_cast<const LunaRuntimeFragmentDescriptorV1*>(chosen[index]->implementation());
                require(descriptor && descriptor->environment_size == sizeof(mask) &&
                    descriptor->environment_alignment == alignof(int32_t), "Copy factory layout mismatch");
                RuntimeFragmentRef ref;
                require(runtime::makeOwnedRuntimeFragmentRef(*chosen[index], requirement,
                    {descriptor->factory_contract_id, &mask}, ref, error), "compiled factory failed", error);
                refs.push_back(std::move(ref));
            }
            RuntimeFragmentBindingSet bindings;
            const bool built = length == 1
                ? runtime::makeRuntimeFragmentBindingSet(std::move(refs), bindings, error)
                : runtime::makeRuntimeFragmentChainBindingSet(std::move(refs), bindings, error);
            require(built &&
                bindings.chainSize(requirement) == length, "compiled chain failed", error);
            return bindings;
        };
        RuntimeFragmentBindingSet none;
        require(runtime::makeRuntimeFragmentBindingSet({}, none, error), "None policy failed", error);
        const auto one = chain(1, 0), two = chain(2, 0), four = chain(4, 0);
        RuntimeFragmentBindingSet overrideNone;
        require(runtime::makeRuntimeFragmentBindingOverride(four, requirement, {}, overrideNone, error) &&
            overrideNone.chainSize(requirement) == 0 && four.chainSize(requirement) == 4,
            "local override changed its parent", error);
        const std::array<const RuntimeFragmentBindingSet*, 5> bindings{&none, &one, &two, &four, &overrideNone};
        for (size_t index = 0; index < bindings.size(); ++index) {
            auto safePoint = runtime.safePoint();
            require(runtime.activateFragmentBindings(*bindings[index], safePoint, error) &&
                runtime::makeRuntimeFragmentExecutionContext(runtime.pinFragmentBindings(), contexts[index], error),
                "compiled policy did not activate/pin", error);
        }
        const auto conditional = chain(1, 1);
        require(runtime::makeRuntimeFragmentExecutionContext(conditional, conditionalContext, error),
            "conditional factory context failed", error);
        if (observeSetup) discoveryBindingMs = milliseconds(bindStart);
        if (!observeSetup) {
            // These are non-timed loading gates, not setup observations.
            MoonRuntime::PinnedGeneration duplicate;
            require(driver::loadVerifiedMoonGenerationOnce(runtime, hostBytes,
                targetTriple, dataLayout, duplicate, error, {}, profile) &&
                duplicate.generationId() == host.generationId(), "same-profile cache did not reuse code", error);
            MoonRuntime::PinnedGeneration pluginDuplicate;
            require(driver::loadVerifiedMoonGenerationOnce(runtime, pluginBytes,
                targetTriple, dataLayout, pluginDuplicate, error, dependencies, profile) &&
                pluginDuplicate.generationId() == plugin.generationId(), "plugin profile cache did not reuse code", error);
            MoonRuntime::PinnedGeneration unverified;
            require(!driver::loadVerifiedMoonGenerationOnce(runtime, pluginBytes,
                targetTriple, dataLayout, unverified, error, {}, profile) && !unverified && !error.empty(),
                "profile cache bypassed owner Slot evidence");
            auto corrupt = pluginBytes;
            corrupt.back() ^= 1;
            require(!driver::loadVerifiedMoonGenerationOnce(runtime, corrupt,
                targetTriple, dataLayout, unverified, error, dependencies, profile) &&
                !unverified && !error.empty(), "profile cache bypassed container integrity");
            require(!driver::loadVerifiedMoonGenerationOnce(runtime, pluginBytes,
                "not-the-host-target", dataLayout, unverified, error, dependencies, profile) &&
                !unverified && !error.empty(), "profile cache bypassed target validation");
            const auto other = profile == MoonJitOptimization::O0 ? MoonJitOptimization::O2 : MoonJitOptimization::O0;
            MoonRuntime::PinnedGeneration rejected;
            require(!driver::loadVerifiedMoonGenerationOnce(runtime, hostBytes,
                targetTriple, dataLayout, rejected, error, {}, other) && !rejected &&
                error.find("materialization configuration") != std::string::npos,
                "cross-profile cache returned old code");
            // A separately staged candidate exercises Runtime's locked loadOnce
            // path as well as the adapter's early cache check.
            MoonRuntime::StagedGeneration staged;
            require(driver::stageVerifiedMoonGeneration(runtime, hostBytes,
                targetTriple, dataLayout, {}, staged, error, {}, other), "alternate profile did not stage", error);
            const auto stagedId = staged.generationId();
            require(!runtime.loadOnce(staged, rejected, error) && !rejected &&
                staged.generationId() == stagedId &&
                error.find("materialization configuration") != std::string::npos &&
                runtime.activeGenerationId(host.moduleId()) == host.generationId() &&
                runtime.retainedGenerationCount(host.moduleId()) == 1,
                "locked cross-profile rejection changed generation state");
            auto generationPoint = runtime.safePoint();
            require(runtime.activate(staged, generationPoint, error) && !staged,
                "explicit JIT profile activation failed", error);
            const auto switched = runtime.pin(host.moduleId());
            const auto switchedDynamic = switched.find(dynamic.symbolId.value, dynamic.contractId.value);
            require(switched.generationId() == stagedId && switched.materializationKey() != materializationKey &&
                switched.contentDigest() == hostDigest && switchedDynamic &&
                invokeDynamic(switchedDynamic.implementation(), contexts[3].opaque(), 7) == 38 &&
                invokeDynamic(entries[4].implementation(), contexts[3].opaque(), 7) == 38,
                "profile activation changed contracts, context interop or old pinned code");
            auto rollbackPoint = runtime.safePoint();
            require(runtime.rollback(host.moduleId(), host.generationId(), rollbackPoint, error) &&
                runtime.pin(host.moduleId()).materializationKey() == materializationKey,
                "real JIT profile rollback failed", error);
            MoonRuntime::PinnedGeneration defaultLoad;
            const bool defaultAccepted = driver::loadVerifiedMoonGenerationOnce(runtime, hostBytes,
                targetTriple, dataLayout, defaultLoad, error);
            require(defaultAccepted == (profile == MoonJitOptimization::O0) &&
                (defaultAccepted ? defaultLoad.generationId() == host.generationId() : !defaultLoad),
                "legacy/default loader did not remain O0");
            MoonRuntime::StagedGeneration invalid;
            require(!driver::stageVerifiedMoonGeneration(runtime, hostBytes,
                targetTriple, dataLayout, {}, invalid, error, {}, static_cast<MoonJitOptimization>(99)) &&
                !invalid && error.find("invalid verified Moon JIT") != std::string::npos,
                "invalid JIT profile reached staging");
            require(!driver::loadVerifiedMoonGenerationOnce(runtime, hostBytes,
                targetTriple, dataLayout, rejected, error, {}, static_cast<MoonJitOptimization>(99)) &&
                !rejected && error.find("invalid verified Moon JIT") != std::string::npos,
                "invalid JIT profile reached cache");
        }
        // Runtime and pipelines die here. Entries and contexts alone own the
        // generated code/factory environments during correctness and timing.
    }

    int32_t invoke(size_t index, int32_t value) const {
        return index < 4 ? invokePlain(entries[index].implementation(), value) :
            invokeDynamic(entries[index].implementation(), contexts[index - 4].opaque(), value);
    }
    void check() const {
        for (int32_t value = 0; value < 32; ++value) {
            for (size_t index = 0; index < Cases.size(); ++index)
                require(invoke(index, value) == value * 3 + (index == 3 ? 0 : 17),
                    "compiled result mismatch: " + std::string(Cases[index]));
            require(invokeDynamic(entries[4].implementation(), conditionalContext.opaque(), value) ==
                value * 3 + ((value & 1) == 0 ? 17 : 0), "generated factory lost conditional resume/discard");
        }
    }
    Sample sample(size_t index, size_t iterations) const {
        uint64_t checksum = 0;
        const auto execute = [&](size_t count) {
            for (size_t call = 0; call < count; ++call)
                checksum += static_cast<uint64_t>(invoke(index, static_cast<int32_t>(call % 1024)));
        };
        const auto warmup = std::min<size_t>(iterations, 1000);
        execute(warmup);
        const auto start = Clock::now();
        execute(iterations);
        const auto elapsed = Clock::now() - start;
        const auto expected = [&](size_t count) {
            const auto full = count / 1024, remainder = count % 1024;
            const auto tail = remainder == 0 ? uint64_t{0} : remainder * (remainder - 1) / 2;
            return uint64_t{3} * (full * uint64_t{523776} + tail) +
                (index == 3 ? 0 : uint64_t{17} * count);
        };
        require(checksum == expected(iterations) + expected(warmup), "compiled checksum mismatch");
        return {std::chrono::duration<double, std::nano>(elapsed).count() / iterations,
                iterations + warmup, checksum};
    }
    std::string targetTriple, dataLayout, hostDigest, pluginDigest, materializationKey;
    double compileEncodeMs = 0, loadMs = 0, discoveryBindingMs = 0;
private:
    runtime::RuntimeSlotRequirement requirement;
    std::array<MoonRuntime::PinnedBinding, Cases.size()> entries;
    std::array<RuntimeFragmentExecutionContext, 5> contexts;
    RuntimeFragmentExecutionContext conditionalContext;
};

size_t count(const char* argument, size_t maximum) {
    const std::string value(argument);
    size_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    require(result.ec == std::errc{} && result.ptr == value.data() + value.size() && parsed >= 1 && parsed <= maximum,
            "invalid compiled probe count: " + value);
    return parsed;
}
} // namespace

int checkCompiledFragmentWorkload() {
    try {
        std::string hostDigest, pluginDigest;
        for (const auto profile : Profiles) {
            Fixture fixture(profile);
            fixture.check();
            if (hostDigest.empty()) { hostDigest = fixture.hostDigest; pluginDigest = fixture.pluginDigest; }
            require(fixture.hostDigest == hostDigest && fixture.pluginDigest == pluginDigest,
                "LLVM profiles did not load identical containers");
        }
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

int runCompiledFragmentBenchmark(int argc, char** argv) {
    try {
        std::cout.imbue(std::locale::classic());
        if (argc >= 2 && std::string(argv[1]) == "--compiled-fragment-affinity-info") {
            require(argc == 2, "usage: moonir-canonical-test --compiled-fragment-affinity-info");
            return affinity::reportAffinity();
        }
        const bool pinRequested = argc >= 2 &&
            std::string(argv[1]) == "--compiled-fragment-cost-pinned-thread";
        require(argc >= (pinRequested ? 3 : 2) && argc <= (pinRequested ? 6 : 5) &&
            (pinRequested || std::string(argv[1]) == "--compiled-fragment-cost"),
            "usage: moonir-canonical-test --compiled-fragment-cost [iterations] [rounds] [O0|O2|O3], "
            "or --compiled-fragment-cost-pinned-thread CPU [iterations] [rounds] [O0|O2|O3]");
        const auto cpu = pinRequested ? affinity::parseCpu(argv[2]) : 0;
        // Reject unsupported/disallowed CPUs before compiling or loading anything,
        // but do not pin setup (including any ORC workers created there).
        if (pinRequested) (void)affinity::requireAllowedCpu(cpu);
        const int first = pinRequested ? 3 : 2;
        const auto iterations = argc > first ? count(argv[first], 10000000) : 10000;
        const auto rounds = argc > first + 1 ? count(argv[first + 1], 90) : 9;
        MoonJitOptimization profile = MoonJitOptimization::O0;
        if (argc > first + 2) {
            const std::string name(argv[first + 2]);
            require(name == "O0" || name == "O2" || name == "O3", "invalid compiled JIT profile: " + name);
            profile = name == "O0" ? MoonJitOptimization::O0 :
                name == "O2" ? MoonJitOptimization::O2 : MoonJitOptimization::O3;
        }
        Fixture fixture(profile, true);
        fixture.check();
        std::unique_ptr<affinity::PinnedThread> pinned;
        if (pinRequested) pinned = std::make_unique<affinity::PinnedThread>(cpu);
        std::cout << "# protocol=" << (pinned ? "luna.compiled-fragment-cost.pinned-thread.v1"
                                            : "luna.compiled-fragment-cost.v2") << '\n'
            << "# git_commit=" << LUNA_COMPILED_PROBE_COMMIT << '\n'
            << "# probe_sha256=" << LUNA_COMPILED_PROBE_SHA256 << '\n'
            << "# workload_sha256=" << LUNA_COMPILED_WORKLOAD_SHA256 << '\n'
            << "# build_type=" << LUNA_COMPILED_PROBE_BUILD_TYPE << '\n'
            << "# compiler=" << LUNA_COMPILED_PROBE_COMPILER << '\n'
            << "# cxx=" << __cplusplus << ",llvm=" << LLVM_VERSION_STRING << '\n'
            << "# target=" << fixture.targetTriple << '\n'
            << "# data_layout=" << fixture.dataLayout << '\n'
            << "# host_container_sha256=" << fixture.hostDigest << '\n'
            << "# plugin_container_sha256=" << fixture.pluginDigest << '\n'
            << "# moonir_optimization=O2,llvm_optimization=" << profileName(profile) << '\n'
            << "# orc_codegen=default\n"
            << "# materialization_key=" << fixture.materializationKey << '\n'
            << "# candidate_count=4,host_order=resume_a/resume_b/resume_c/resume_d\n"
            << "# correctness_checks=320,runtime_alive_during_samples=no\n"
            << "# compile_encode_decode_ms=" << std::fixed << std::setprecision(3) << fixture.compileEncodeMs << '\n'
            << "# verified_load_jit_ms=" << fixture.loadMs << '\n'
            << "# discovery_binding_ms=" << fixture.discoveryBindingMs << '\n'
            << "# iterations=" << iterations << ",warmup=" << std::min<size_t>(iterations, 1000) << ",rounds=" << rounds << '\n';
        if (pinned) {
            pinned->report(LUNA_FRAGMENT_AFFINITY_SHA256);
            std::cout << "# setup_affinity=uncontrolled,measurement_scope=dispatch_samples\n";
        } else std::cout << "# affinity=uncontrolled,power_policy=uncontrolled\n";
        std::cout << "# timed_harness=case_branch+input_modulo+JIT_call+checksum\n"
            << "round,position,case,ns_per_op,calls,checksum\n";
        std::cout << std::setprecision(1);
        std::array<std::array<unsigned, Cases.size()>, Cases.size()> visits{};
        for (size_t round = 0; round < rounds; ++round) {
            for (size_t position = 0; position < Cases.size(); ++position) {
                const auto index = (position + round * 4) % Cases.size();
                ++visits[index][position];
                if (pinned) pinned->verify();
                const auto sample = fixture.sample(index, iterations);
                if (pinned) pinned->verify();
                std::cout << round + 1 << ',' << position + 1 << ',' << Cases[index] << ','
                    << sample.nanoseconds << ',' << sample.calls << ',' << sample.checksum << '\n';
            }
        }
        if (rounds % Cases.size() == 0)
            for (const auto& row : visits)
                for (const auto visited : row)
                    require(visited == rounds / Cases.size(), "compiled schedule is not balanced");
        std::cout << "# verified_samples=" << rounds * Cases.size() << ",position_balanced="
                  << (rounds % Cases.size() == 0 ? "yes" : "no") << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
} // namespace luna::benchmarks
