#include "driver/NativeArtifact.h"
#include "driver/NativeGeneration.h"
#include "driver/NativeTypedDescriptor.h"
#include "runtime/RuntimeFragment.h"

#include <llvm/TargetParser/Host.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

namespace {

int fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

void fixtureFragmentExecute(void*, void*) {}

struct ProducedRefProbe {
    std::string slotId;
    std::string slotContractId;
    std::string layoutId;
    uint64_t argumentsSize = 0;
    uint64_t argumentsAlignment = 0;
    unsigned calls = 0;
    bool sawExpectedValue = false;
    bool repeatResume = false;
    int32_t resumeStatus = 0;
    int32_t repeatedResumeStatus = 0;
};

thread_local ProducedRefProbe* activeProducedRefProbe = nullptr;

void executeProducedRefFragment(void*, void* activation) {
    auto* probe = activeProducedRefProbe;
    if (!probe) return;
    ++probe->calls;
    const void* arguments = luna_runtime_fragment_activation_arguments_v1(
        activation, probe->slotId.c_str(), probe->slotContractId.c_str(),
        probe->layoutId.c_str(), probe->argumentsSize,
        probe->argumentsAlignment);
    if (arguments && probe->argumentsSize >= sizeof(int32_t)) {
        int32_t value = 0;
        std::memcpy(&value, arguments, sizeof(value));
        probe->sawExpectedValue = value == 1;
    }
    probe->resumeStatus = luna_runtime_fragment_activation_resume_v1(activation);
    if (probe->repeatResume)
        probe->repeatedResumeStatus =
            luna_runtime_fragment_activation_resume_v1(activation);
}

int prepareIndependentFixture(int argc, char** argv) {
    if (argc != 4) return 2;
    const bool invalidUtf8 =
        std::string(argv[1]) == "--prepare-legacy-invalid-utf8";
    const bool typedV2 =
        std::string(argv[1]) == "--prepare-v2-fixture";
    const bool typedV3 =
        std::string(argv[1]) == "--prepare-v3-fixture" ||
        std::string(argv[1]) == "--prepare-v3-dependency-fixture";
    const bool dependencyV3 =
        std::string(argv[1]) == "--prepare-v3-dependency-fixture";
    const bool typedV4 =
        std::string(argv[1]) == "--prepare-v4-fixture" ||
        std::string(argv[1]) == "--prepare-v4-unbound-fixture" ||
        std::string(argv[1]) == "--prepare-v4-dependency-fixture" ||
        std::string(argv[1]) == "--prepare-v4-bad-module-fixture" ||
        std::string(argv[1]) == "--prepare-v4-missing-slot-fixture";
    const bool dependencyV4 =
        std::string(argv[1]) == "--prepare-v4-dependency-fixture";
    const bool unboundV4 =
        std::string(argv[1]) == "--prepare-v4-unbound-fixture";
    const bool missingV4Slot =
        std::string(argv[1]) == "--prepare-v4-missing-slot-fixture";
    const bool badV4Module =
        std::string(argv[1]) == "--prepare-v4-bad-module-fixture";
    const std::string targetAbi = llvm::sys::getProcessTriple();
    if (targetAbi.empty() || targetAbi.size() >= 128)
        return fail("independent Native fixture target ABI exceeds its bounded field");
    std::ifstream source(argv[2], std::ios::binary);
    if (!source) return fail("cannot read independent Native fixture");
    std::vector<uint8_t> bytes(
        (std::istreambuf_iterator<char>(source)),
        std::istreambuf_iterator<char>());
    const std::string marker = "LUNA_TEST_TARGET_ABI_PLACEHOLDER";
    const auto target = std::search(
        bytes.begin(), bytes.end(), marker.begin(), marker.end());
    if (target == bytes.end() ||
        std::search(target + 1, bytes.end(),
                    marker.begin(), marker.end()) != bytes.end() ||
        static_cast<size_t>(bytes.end() - target) < 128)
        return fail("independent Native fixture has no unique target ABI field");
    std::fill_n(target, 128, 0);
    std::copy(targetAbi.begin(), targetAbi.end(), target);

    if (typedV2) {
        const std::string marker = "V2_DIGEST_PLACEHOLDER_0123456789";
        static_assert(sizeof("V2_DIGEST_PLACEHOLDER_0123456789") - 1 ==
                      LUNA_NATIVE_DESCRIPTOR_DIGEST_SIZE_V2);
        const auto position = std::search(
            bytes.begin(), bytes.end(), marker.begin(), marker.end());
        if (position == bytes.end() ||
            std::search(position + 1, bytes.end(), marker.begin(),
                        marker.end()) != bytes.end())
            return fail("independent v2 fixture has no unique descriptor digest placeholder");
        const auto canonical = luna::driver::canonicalNativeTypedExport(
            LUNA_NATIVE_DECLARATION_FUNCTION_V1,
            LUNA_NATIVE_EXPORT_CALLABLE_V1,
            LUNA_NATIVE_ENTRY_ABI_C_I32_NOARGS_V1,
            "symbol:legacy-answer", "contract:legacy-v1", "legacy_answer");
        const auto digest = luna::driver::digestNativeTypedExports({canonical});
        std::copy(digest.begin(), digest.end(), position);
    }
    if (typedV3) {
        const std::string marker = "V3_DIGEST_PLACEHOLDER_0123456789";
        static_assert(sizeof("V3_DIGEST_PLACEHOLDER_0123456789") - 1 ==
                      LUNA_NATIVE_DESCRIPTOR_DIGEST_SIZE_V3);
        const auto position = std::search(
            bytes.begin(), bytes.end(), marker.begin(), marker.end());
        if (position == bytes.end() ||
            std::search(position + 1, bytes.end(), marker.begin(),
                        marker.end()) != bytes.end())
            return fail("independent v3 fixture has no unique descriptor digest placeholder");
        const std::array<std::string, 10> identifiers = {
            "symbol:owned-answer", "contract:owned-v3",
            dependencyV3 ? "getpid" : "owned_answer",
            "symbol:slot-checkpoint", "contract:slot-checkpoint",
            "type:result-i32-owned", "type:owned-error",
            "layout:owned-error-v1", "symbol:drop-owned-error",
            "contract:drop-owned-error"};
        const auto canonical = luna::driver::canonicalNativeOwnedResultExportV3(
            LUNA_NATIVE_DECLARATION_FUNCTION_V1,
            LUNA_NATIVE_EXPORT_CALLABLE_V1,
            LUNA_NATIVE_ENTRY_ABI_REF_RESULT_OWNER_V1,
            LUNA_NATIVE_REF_SHARED_BORROW_V1,
            LUNA_NATIVE_RESULT_I32_OWNED_ERROR_V1,
            LUNA_NATIVE_STATUS_DOMAIN_REF_RESULT_OWNER_V1,
            8, 8, identifiers);
        const auto digest = luna::driver::digestNativeTypedExports({canonical});
        std::copy(digest.begin(), digest.end(), position);
    }
    std::string v4Canonical;
    if (typedV4) {
        const std::string marker = "V4_DIGEST_PLACEHOLDER_0123456789";
        static_assert(sizeof("V4_DIGEST_PLACEHOLDER_0123456789") - 1 ==
                      LUNA_NATIVE_DESCRIPTOR_DIGEST_SIZE_V4);
        const auto position = std::search(
            bytes.begin(), bytes.end(), marker.begin(), marker.end());
        if (position == bytes.end() ||
            std::search(position + 1, bytes.end(), marker.begin(),
                        marker.end()) != bytes.end())
            return fail("independent v4 fixture has no unique descriptor digest placeholder");
        const std::array<std::string, 10> identifiers = {
            "symbol:unit-apply", "contract:unit-v4",
            dependencyV4 ? "getpid" : "unit_apply",
            badV4Module ? "org.luna.fixture.other" :
                          "org.luna.fixture.native_v1",
            "org.luna.fixture.native_v1",
            "type:unit-fn", "type:ref-checkpoint",
            "symbol:slot-checkpoint", "contract:slot-checkpoint",
            "type:slot-checkpoint"};
        v4Canonical = luna::driver::canonicalNativeRefUnitExportV4(
            LUNA_NATIVE_DECLARATION_FUNCTION_V1,
            LUNA_NATIVE_EXPORT_CALLABLE_V1,
            LUNA_NATIVE_ENTRY_ABI_REF_UNIT_V1,
            LUNA_NATIVE_REF_SHARED_BORROW_V1,
            LUNA_NATIVE_RESULT_UNIT_V1,
            LUNA_NATIVE_STATUS_DOMAIN_REF_UNIT_V1, identifiers);
        const auto digest = luna::driver::digestNativeTypedExports({v4Canonical});
        std::copy(digest.begin(), digest.end(), position);
    }

    const uint8_t magic[] = {'L', 'U', 'N', 'A', 'N', 'P', '1', 0};
    const auto proof = std::search(
        bytes.begin(), bytes.end(), std::begin(magic), std::end(magic));
    if (proof == bytes.end() ||
        std::search(proof + 1, bytes.end(),
                    std::begin(magic), std::end(magic)) != bytes.end() ||
        static_cast<size_t>(bytes.end() - proof) < sizeof(LunaNativeProofV1))
        return fail("independent Native fixture has no unique proof placeholder");
    luna::driver::NativeExportSpec exported;
    exported.declarationKind = LUNA_NATIVE_DECLARATION_FUNCTION_V1;
    exported.flags = LUNA_NATIVE_EXPORT_CALLABLE_V1;
    exported.symbolId = invalidUtf8
        ? "symbol:\xc0\xaf" : "symbol:legacy-answer";
    exported.contractId = "contract:legacy-v1";
    exported.linkageName = "legacy_answer";
    luna::driver::NativeProofSpec spec;
    spec.packageId = "org.luna.fixture.native_v1";
    spec.packageVersion = "1.0.0";
    spec.targetAbi = targetAbi;
    spec.compilerIdentity = "luna-v1-compat-fixture";
    spec.exportedDescriptors.push_back(
        luna::driver::canonicalNativeExport(exported));
    if (typedV4 && !missingV4Slot) {
        luna::driver::NativeExportSpec slot;
        slot.declarationKind = LUNA_NATIVE_DECLARATION_SLOT_V1;
        slot.symbolId = "symbol:slot-checkpoint";
        slot.contractId = "contract:slot-checkpoint";
        slot.linkageName = "checkpoint";
        spec.exportedDescriptors.push_back(
            luna::driver::canonicalNativeExport(slot));
    }
    if (typedV4 && !unboundV4)
        spec.exportedDescriptors.push_back(v4Canonical);
    std::vector<uint8_t> record;
    std::string error;
    if (!luna::driver::makeNativeProofPlaceholder(spec, record, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::copy(record.begin(), record.end(), proof);
    std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) return fail("cannot write independent Native fixture proof");
    luna::driver::NativeProofInfo sealed;
    if (!luna::driver::sealNativeArtifact(argv[2], argv[3],
                                          sealed, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    return 0;
}

struct LoadPause {
    std::string readyPath;
    std::string releasePath;
    bool timedOut = false;
};

void pauseAfterVerification(void* context) {
    auto& pause = *static_cast<LoadPause*>(context);
    std::ofstream ready(pause.readyPath, std::ios::trunc);
    ready << "verified\n";
    ready.close();
    if (!ready) {
        pause.timedOut = true;
        return;
    }
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
        if (std::filesystem::exists(pause.releasePath)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    pause.timedOut = true;
}

int loadAndCall(int argc, char** argv) {
    if (argc != 6 && argc != 8) {
        std::cerr << "usage: native-artifact-test --load-call <artifact> "
                     "<trust-store> <symbol-id> <contract-id> "
                     "[<ready-file> <release-file>]\n";
        return 2;
    }
    LoadPause pause;
    void (*hook)(void*) = nullptr;
    if (argc == 8) {
        pause.readyPath = argv[6];
        pause.releasePath = argv[7];
        hook = pauseAfterVerification;
    }
    luna::driver::VerifiedNativeLibrary library;
    std::string error;
    if (!luna::driver::loadVerifiedNativeLibrary(
            argv[2], argv[3], library, error, hook,
            hook ? &pause : nullptr)) {
        std::cerr << error << '\n';
        return 1;
    }
    if (pause.timedOut) {
        std::cerr << "timed out waiting for the TOCTOU test release\n";
        return 1;
    }
    const auto* exported = library.findExport(argv[4], argv[5]);
    if (!exported ||
        (exported->flags & LUNA_NATIVE_EXPORT_CALLABLE_V1) == 0 ||
        !exported->entry) {
        std::cerr << "requested typed Native export is not callable\n";
        return 1;
    }
    using AnswerFunction = int32_t (*)();
    static_assert(sizeof(AnswerFunction) == sizeof(exported->entry),
                  "test host cannot represent a Native entry pointer");
    AnswerFunction answer = nullptr;
    std::memcpy(&answer, &exported->entry, sizeof(answer));
    std::cout << answer() << '\n';
    return 0;
}

int loadTypedAndCall(int argc, char** argv) {
    if (argc != 6) return 2;
    luna::driver::VerifiedNativeLibrary library;
    std::string error;
    if (!luna::driver::loadVerifiedNativeLibrary(
            argv[2], argv[3], library, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    luna::driver::VerifiedNativeLibrary moved(std::move(library));
    if (library || !moved) return fail("Native typed library move lost its lease");
    int32_t answer = 0;
    if (!moved.callI32NoArgs(argv[4], argv[5], answer, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    int32_t ignored = 0;
    std::string absentError;
    if (moved.callI32NoArgs(argv[4], "wrong-contract", ignored,
                            absentError) || absentError.empty())
        return fail("Native typed lookup accepted a wrong contract");
    std::cout << answer << '\n';
    return 0;
}

int inspectV3Candidate(int argc, char** argv) {
    if (argc != 4) return 2;
    luna::driver::VerifiedNativeLibrary library;
    std::string error;
    if (!luna::driver::loadVerifiedNativeLibrary(
            argv[2], argv[3], library, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    luna::driver::VerifiedNativeLibrary moved(std::move(library));
    if (library || !moved ||
        library.hasRefResultOwnerCandidate(
            "symbol:owned-answer", "contract:owned-v3") ||
        !moved.hasRefResultOwnerCandidate(
            "symbol:owned-answer", "contract:owned-v3") ||
        moved.hasRefResultOwnerCandidate(
            "symbol:owned-answer", "contract:legacy-v1") ||
        moved.hasRefResultOwnerCandidate(
            "symbol:legacy-answer", "contract:legacy-v1") ||
        moved.findExport("symbol:owned-answer", "contract:owned-v3") ||
        moved.entryAbiForExport("symbol:owned-answer", "contract:owned-v3"))
        return fail("Native v3 candidate escaped its separate metadata lookup");
    int32_t ignored = 0;
    if (moved.callI32NoArgs("symbol:owned-answer", "contract:owned-v3",
                            ignored, error) || error.empty())
        return fail("Native v3 candidate acquired an i32() call profile");
    std::cout << "v3-metadata-only\n";
    return 0;
}

int callProducedRefUnit(int argc, char** argv) {
    if (argc != 7) return 2;
    uint64_t argumentsSize = 0;
    uint64_t argumentsAlignment = 0;
    try {
        argumentsSize = std::stoull(argv[5]);
        argumentsAlignment = std::stoull(argv[6]);
    } catch (...) {
        return fail("Native Ref/unit consumer received an invalid Slot layout");
    }
    if (!*argv[4] || argumentsSize < sizeof(int32_t) ||
        argumentsSize > 4096 || argumentsAlignment == 0 ||
        argumentsAlignment > 4096)
        return fail("Native Ref/unit consumer received an unsupported Slot layout");

    std::string error;
    luna::driver::VerifiedNativeLibrary library;
    if (!luna::driver::loadVerifiedNativeLibrary(
            argv[2], argv[3], library, error) ||
        library.refUnitCandidateCount() != 1)
        return fail("produced Native Ref/unit image has no unique verified candidate");
    luna::runtime::GenerationRefUnitCandidate facts;
    if (!library.refUnitCandidateAt(0, facts))
        return fail("produced Native Ref/unit candidate could not be read");
    if (!luna::driver::verifyNativeRequestMutationForTest(
            argv[2], argv[3], [](auto&) {}, error))
        return fail("verified Native request rejected its artifact proof");
    const std::string lostProof =
        "verified Native generation request lost its artifact proof";
    if (luna::driver::verifyNativeRequestMutationForTest(
            argv[2], argv[3],
            [](auto& request) {
                request.refUnitCandidates.front().slotTypeId =
                    "type:forged-slot";
            }, error) || error != lostProof)
        return fail("forged Native Ref/unit facts passed request verification");
    if (luna::driver::verifyNativeRequestMutationForTest(
            argv[2], argv[3],
            [](auto& request) {
                request.moduleLease = std::shared_ptr<const void>(
                    request.moduleLease.get(), [](const void*) {});
            }, error) || error != lostProof)
        return fail("Native module lease with foreign ownership passed request verification");
    if (luna::driver::verifyNativeRequestMutationForTest(
            argv[2], argv[3],
            [](auto& request) {
                request.refUnitCaller = [](
                    const std::string&, const std::string&,
                    const luna::runtime::RuntimeFragmentExecutionContext&,
                    const luna::runtime::RuntimeFragmentRefHandle&,
                    int32_t&, std::string&) { return true; };
            }, error) || error != lostProof)
        return fail("forged Native Ref/unit caller passed request verification");
    if (luna::driver::verifyAlternateNativeCallerForTest(
            argv[2], argv[3], error) || error != lostProof)
        return fail("alternate same-type Native caller passed request verification");
    if (luna::driver::verifyNativeRequestMutationForTest(
            argv[2], argv[3],
            [](auto& request) {
                request.requiredAbsentModuleIds.push_back("package:forged");
            }, error) || error != lostProof)
        return fail("hidden Native module relation passed request verification");
    const auto* slot = library.findExport(
        facts.slotSymbolId, facts.slotContractId);
    if (!slot || slot->declaration_kind != LUNA_NATIVE_DECLARATION_SLOT_V1 ||
        slot->flags != 0 || slot->entry ||
        library.findExport(facts.symbolId, facts.contractId))
        return fail("produced Native Ref/unit candidate lost its separate Slot");

    luna::runtime::MoonRuntime::PinnedRefUnitCandidate candidate;
    {
        luna::runtime::MoonRuntime runtime;
        luna::runtime::MoonRuntime::PinnedGeneration generation;
        if (!luna::driver::loadVerifiedNativeGenerationOnce(
                runtime, argv[2], argv[3], generation, error))
            return fail("produced Native Ref/unit image could not enter a generation");
        candidate = generation.findRefUnitCandidate(
            facts.symbolId, facts.contractId);
        if (!candidate ||
            generation.find(facts.symbolId, facts.contractId))
            return fail("produced Native Ref/unit candidate escaped its typed lookup");
    }
    library = luna::driver::VerifiedNativeLibrary{};

    const LunaRuntimeFragmentDescriptorV1 fragment = {
        LUNA_RUNTIME_FRAGMENT_MAGIC_V1,
        LUNA_RUNTIME_FRAGMENT_ABI_V1,
        sizeof(LunaRuntimeFragmentDescriptorV1),
        LUNA_RUNTIME_FRAGMENT_CAPTURE_FREE_V1,
        0, 0, 0, 0,
        "fragment:produced-ref-consumer",
        "contract:produced-ref-consumer",
        facts.slotSymbolId.c_str(), facts.slotContractId.c_str(),
        argv[4], argumentsSize, argumentsAlignment,
        "", "layout:unit", 0, 1,
        nullptr, nullptr, executeProducedRefFragment};
    const std::string foreignSlot = facts.slotSymbolId + ".foreign";
    const std::string foreignContract = facts.slotContractId + ".foreign";
    auto foreignFragment = fragment;
    foreignFragment.fragment_id = "fragment:produced-ref-foreign";
    foreignFragment.slot_id = foreignSlot.c_str();
    foreignFragment.slot_contract_id = foreignContract.c_str();
    if (!luna::runtime::validateRuntimeFragmentDescriptor(fragment, error) ||
        !luna::runtime::validateRuntimeFragmentDescriptor(
            foreignFragment, error))
        return fail("produced Native Ref/unit consumer has an invalid fragment");

    luna::runtime::MoonRuntime hostRuntime;
    const luna::runtime::GenerationStagingRequest hostRequest{
        "org.luna.native.ref-consumer", std::string(64, 'b'),
        std::make_shared<int>(1)};
    luna::runtime::MoonRuntime::StagedGeneration hostStaged;
    constexpr uint32_t fragmentFlags =
        luna::runtime::GenerationBindingFragmentExecutable |
        luna::runtime::GenerationBindingPublicControl;
    if (!hostRuntime.stage(
            hostRequest,
            [](const auto&, std::string&) { return true; },
            [&](const auto&, auto& bindings, std::string&) {
                const LunaRuntimeFragmentDescriptorV1* descriptors[] = {
                    &fragment, &foreignFragment};
                for (const auto* descriptor : descriptors)
                    bindings.push_back({descriptor->fragment_id,
                        descriptor->fragment_contract_id, descriptor,
                        LUNA_NATIVE_DECLARATION_FRAGMENT_V1, fragmentFlags});
                return true;
            }, {}, hostStaged, error))
        return fail("produced Native Ref/unit consumer host did not stage");
    luna::runtime::MoonRuntime::PinnedGeneration hostPinned;
    if (!hostRuntime.loadOnce(hostStaged, hostPinned, error))
        return fail("produced Native Ref/unit consumer host did not load");
    auto makeHandle = [&](const LunaRuntimeFragmentDescriptorV1& descriptor,
                          luna::runtime::RuntimeFragmentRefHandle& output) {
        const luna::runtime::GenerationBindingRequirement requirement{
            descriptor.fragment_id, descriptor.fragment_contract_id,
            LUNA_NATIVE_DECLARATION_FRAGMENT_V1, fragmentFlags};
        const auto binding = hostPinned.find(requirement);
        const luna::runtime::RuntimeSlotRequirement target{
            descriptor.slot_id, descriptor.slot_contract_id};
        luna::runtime::RuntimeFragmentRef reference;
        return binding && luna::runtime::makeOwnedRuntimeFragmentRef(
                binding, target, {}, reference, error) &&
            luna::runtime::makeRuntimeFragmentRefHandle(
                reference, target, output, error);
    };
    luna::runtime::RuntimeFragmentRefHandle reference, foreignReference;
    if (!makeHandle(fragment, reference) ||
        !makeHandle(foreignFragment, foreignReference))
        return fail("produced Native Ref/unit consumer could not issue Ref handles");
    luna::runtime::RuntimeFragmentBindingSet empty;
    luna::runtime::RuntimeFragmentExecutionContext parent, missingParent;
    luna::runtime::RuntimeFragmentRefHandle missingReference;
    if (!luna::runtime::makeRuntimeFragmentBindingSet({}, empty, error) ||
        !luna::runtime::makeRuntimeFragmentExecutionContext(
            empty, parent, error))
        return fail("produced Native Ref/unit consumer could not issue a parent");

    ProducedRefProbe probe{
        facts.slotSymbolId, facts.slotContractId, argv[4],
        argumentsSize, argumentsAlignment};
    struct ScopedProbe {
        ProducedRefProbe* previous;
        explicit ScopedProbe(ProducedRefProbe& current)
            : previous(activeProducedRefProbe) {
            activeProducedRefProbe = &current;
        }
        ~ScopedProbe() { activeProducedRefProbe = previous; }
    } active(probe);
    int32_t status = -1;
    const auto rejectsBeforeEntry = [&](const auto& testParent,
                                        const auto& testReference) {
        error.clear();
        status = -1;
        return !candidate.call(testParent, testReference, status, error) &&
            !error.empty() && status == -1 && probe.calls == 0;
    };
    if (!rejectsBeforeEntry(missingParent, reference) ||
        !rejectsBeforeEntry(parent, missingReference) ||
        !rejectsBeforeEntry(parent, foreignReference))
        return fail("produced Native Ref/unit consumer admitted an invalid capability");
    error.clear();
    status = -1;
    if (!candidate.call(parent, reference, status, error) ||
        status != LUNA_NATIVE_REF_UNIT_SUCCESS_V1 || !error.empty() ||
        probe.calls != 1 || !probe.sawExpectedValue ||
        probe.resumeStatus != LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1)
        return fail("produced Native Ref/unit consumer did not complete Apply");
    probe.calls = 0;
    probe.sawExpectedValue = false;
    probe.repeatResume = true;
    probe.repeatedResumeStatus = 0;
    error.clear();
    status = -1;
    if (!candidate.call(parent, reference, status, error) ||
        status != LUNA_NATIVE_REF_UNIT_EXECUTION_FAILURE_V1 ||
        !error.empty() || probe.calls != 1 || !probe.sawExpectedValue ||
        probe.resumeStatus != LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1 ||
        probe.repeatedResumeStatus !=
            LUNA_RUNTIME_FRAGMENT_DISPATCH_EXECUTION_FAILED_V1)
        return fail("produced Native Ref/unit consumer lost execution failure");
    probe.calls = 0;
    probe.sawExpectedValue = false;
    probe.repeatResume = false;
    error.clear();
    status = -1;
    if (!candidate.call(parent, reference, status, error) ||
        status != LUNA_NATIVE_REF_UNIT_SUCCESS_V1 || !error.empty() ||
        probe.calls != 1 || !probe.sawExpectedValue ||
        probe.resumeStatus != LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1)
        return fail("produced Native Ref/unit consumer did not recover");
    std::cout << "produced-v4-host-call\n";
    return 0;
}

int inspectV4Candidate(int argc, char** argv) {
    if (argc != 4) return 2;
    luna::driver::VerifiedNativeLibrary library;
    std::string error;
    if (!luna::driver::loadVerifiedNativeLibrary(
            argv[2], argv[3], library, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    luna::driver::VerifiedNativeLibrary moved(std::move(library));
    const auto* slot = moved.findExport(
        "symbol:slot-checkpoint", "contract:slot-checkpoint");
    if (library || !moved ||
        !slot || slot->declaration_kind != LUNA_NATIVE_DECLARATION_SLOT_V1 ||
        slot->flags != 0 || slot->entry != nullptr ||
        library.hasRefUnitCandidate("symbol:unit-apply", "contract:unit-v4") ||
        !moved.hasRefUnitCandidate("symbol:unit-apply", "contract:unit-v4") ||
        moved.hasRefUnitCandidate("symbol:unit-apply", "wrong-contract") ||
        moved.hasRefUnitCandidate("symbol:legacy-answer", "contract:legacy-v1") ||
        moved.findExport("symbol:unit-apply", "contract:unit-v4") ||
        moved.entryAbiForExport("symbol:unit-apply", "contract:unit-v4"))
        return fail("Native v4 candidate escaped its separate metadata lookup");
    int32_t ignored = 0;
    if (moved.callI32NoArgs("symbol:unit-apply", "contract:unit-v4",
                            ignored, error) || error.empty())
        return fail("Native v4 candidate acquired an i32() call profile");
    std::shared_ptr<const luna::driver::VerifiedNativeLibrary> callLibrary =
        std::make_shared<luna::driver::VerifiedNativeLibrary>(std::move(moved));
    const LunaRuntimeFragmentDescriptorV1 fragment = {
        LUNA_RUNTIME_FRAGMENT_MAGIC_V1,
        LUNA_RUNTIME_FRAGMENT_ABI_V1,
        sizeof(LunaRuntimeFragmentDescriptorV1),
        LUNA_RUNTIME_FRAGMENT_CAPTURE_FREE_V1,
        0, 0, 0, 0,
        "fragment:native-ref-fixture", "contract:native-ref-fixture",
        "symbol:slot-checkpoint", "contract:slot-checkpoint",
        "layout:unit", 0, 1, "", "layout:unit", 0, 1,
        nullptr, nullptr, fixtureFragmentExecute};
    auto otherFragment = fragment;
    otherFragment.fragment_id = "fragment:native-ref-other";
    otherFragment.slot_id = "symbol:other-slot";
    luna::runtime::MoonRuntime hostRuntime;
    const luna::runtime::GenerationStagingRequest hostRequest{
        "org.luna.fixture.ref-host", std::string(64, 'a'),
        std::make_shared<int>(1)};
    luna::runtime::MoonRuntime::StagedGeneration hostStaged;
    if (!hostRuntime.stage(
            hostRequest,
            [](const auto&, std::string&) { return true; },
            [&](const auto&, auto& bindings, std::string&) {
                const LunaRuntimeFragmentDescriptorV1* descriptors[] = {
                    &fragment, &otherFragment};
                for (const auto* descriptor : descriptors)
                    bindings.push_back({
                        descriptor->fragment_id,
                        descriptor->fragment_contract_id,
                        descriptor, LUNA_NATIVE_DECLARATION_FRAGMENT_V1,
                        luna::runtime::GenerationBindingFragmentExecutable |
                            luna::runtime::GenerationBindingPublicControl});
                return true;
            }, {}, hostStaged, error))
        return fail("Native Ref/unit fixture host generation did not stage");
    luna::runtime::MoonRuntime::PinnedGeneration hostPinned;
    if (!hostRuntime.loadOnce(hostStaged, hostPinned, error))
        return fail("Native Ref/unit fixture host generation did not load");
    auto makeHandle = [&](const LunaRuntimeFragmentDescriptorV1& descriptor,
                          luna::runtime::RuntimeFragmentRefHandle& output) {
        const luna::runtime::GenerationBindingRequirement requirement{
            descriptor.fragment_id, descriptor.fragment_contract_id,
            LUNA_NATIVE_DECLARATION_FRAGMENT_V1,
            luna::runtime::GenerationBindingFragmentExecutable |
                luna::runtime::GenerationBindingPublicControl};
        const auto binding = hostPinned.find(requirement);
        const luna::runtime::RuntimeSlotRequirement slot{
            descriptor.slot_id, descriptor.slot_contract_id};
        luna::runtime::RuntimeFragmentRef reference;
        return binding && luna::runtime::makeOwnedRuntimeFragmentRef(
                binding, slot, {}, reference, error) &&
            luna::runtime::makeRuntimeFragmentRefHandle(
                reference, slot, output, error);
    };
    luna::runtime::RuntimeFragmentRefHandle reference, wrongReference;
    luna::runtime::RuntimeFragmentRefHandle missingReference;
    if (!makeHandle(fragment, reference) ||
        !makeHandle(otherFragment, wrongReference))
        return fail("Native Ref/unit fixture could not issue live Ref handles");
    luna::runtime::RuntimeFragmentBindingSet emptySet;
    luna::runtime::RuntimeFragmentExecutionContext parent, missingParent;
    if (!luna::runtime::makeRuntimeFragmentBindingSet(
            {}, emptySet, error) ||
        !luna::runtime::makeRuntimeFragmentExecutionContext(
            emptySet, parent, error))
        return fail("Native Ref/unit fixture could not issue a live parent");
    int32_t status = -1;
    if (luna::driver::callVerifiedNativeRefUnit(
            callLibrary, "symbol:unit-apply", "wrong-contract",
            parent, reference, status, error) || error.empty() ||
        luna::driver::callVerifiedNativeRefUnit(
            callLibrary, "symbol:unit-apply", "contract:unit-v4",
            missingParent, reference, status, error) || error.empty() ||
        luna::driver::callVerifiedNativeRefUnit(
            callLibrary, "symbol:unit-apply", "contract:unit-v4",
            parent, missingReference, status, error) || error.empty() ||
        luna::driver::callVerifiedNativeRefUnit(
            callLibrary, "symbol:unit-apply", "contract:unit-v4",
            parent, wrongReference, status, error) || error.empty() ||
        status != -1)
        return fail("Native Ref/unit call accepted an invalid capability or target");
    if (!luna::driver::callVerifiedNativeRefUnit(
            callLibrary, "symbol:unit-apply", "contract:unit-v4",
            parent, reference, status, error) ||
        status != LUNA_NATIVE_REF_UNIT_SUCCESS_V1 || !error.empty())
        return fail("Native Ref/unit call rejected live Runtime-issued capabilities");
    callLibrary.reset();
    luna::runtime::MoonRuntime::PinnedRefUnitCandidate pinnedCandidate;
    {
        luna::runtime::MoonRuntime runtime;
        luna::runtime::MoonRuntime::PinnedGeneration loaded;
        if (!luna::driver::loadVerifiedNativeGenerationOnce(
                runtime, argv[2], argv[3], loaded, error))
            return fail("Native v4 candidate could not enter a pinned generation");
        pinnedCandidate = loaded.findRefUnitCandidate(
            "symbol:unit-apply", "contract:unit-v4");
        if (!pinnedCandidate ||
            pinnedCandidate.generationId() != loaded.generationId() ||
            loaded.findRefUnitCandidate("symbol:unit-apply", "wrong-contract") ||
            loaded.find("symbol:unit-apply", "contract:unit-v4"))
            return fail("Native v4 candidate escaped its separate pinned lookup");
    }
    moved = luna::driver::VerifiedNativeLibrary{};
    const auto pinnedFacts = pinnedCandidate.facts();
    if (!pinnedFacts ||
        pinnedFacts->entryAbi !=
            luna::runtime::GenerationEntryAbiRefUnitV1 ||
        pinnedFacts->functionTypeId != "type:unit-fn" ||
        pinnedFacts->refTypeId != "type:ref-checkpoint" ||
        pinnedFacts->slotSymbolId != "symbol:slot-checkpoint" ||
        pinnedFacts->slotContractId != "contract:slot-checkpoint" ||
        pinnedFacts->slotTypeId != "type:slot-checkpoint")
        return fail("Native v4 pinned facts lost their verified target or lease");
    status = -1;
    if (pinnedCandidate.call(missingParent, reference, status, error) ||
        error.empty() ||
        pinnedCandidate.call(parent, wrongReference, status, error) ||
        error.empty() || status != -1)
        return fail("pinned Native Ref/unit call accepted an invalid capability");
    if (!pinnedCandidate.call(parent, reference, status, error) ||
        status != LUNA_NATIVE_REF_UNIT_SUCCESS_V1 || !error.empty())
        return fail("pinned Native Ref/unit call lost its generation library");
    std::cout << "v4-typed-host-call\n";
    return 0;
}

int loadLegacyGeneration(int argc, char** argv) {
    if (argc != 6) return 2;
    luna::runtime::MoonRuntime runtime;
    luna::runtime::MoonRuntime::PinnedGeneration loaded;
    std::string error;
    if (!luna::driver::loadVerifiedNativeGenerationOnce(
            runtime, argv[2], argv[3], loaded, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    const luna::runtime::GenerationBindingRequirement unprofiled{
        argv[4], argv[5], LUNA_NATIVE_DECLARATION_FUNCTION_V1,
        LUNA_NATIVE_EXPORT_CALLABLE_V1,
        luna::runtime::GenerationEntryAbiUnprofiled};
    auto typed = unprofiled;
    typed.entryAbi = luna::runtime::GenerationEntryAbiCI32NoArgsV1;
    const luna::runtime::GenerationBindingRequirement v4Candidate{
        "symbol:unit-apply", "contract:unit-v4",
        LUNA_NATIVE_DECLARATION_FUNCTION_V1,
        LUNA_NATIVE_EXPORT_CALLABLE_V1,
        luna::runtime::GenerationEntryAbiUnprofiled};
    auto binding = loaded.find(unprofiled);
    int32_t ignored = 0;
    if (!binding || binding.entryAbi() !=
            luna::runtime::GenerationEntryAbiUnprofiled ||
        loaded.find(typed) || loaded.find(v4Candidate) ||
        binding.callI32NoArgs(ignored))
        return fail("v1-only generation acquired a typed entry profile");
    std::cout << "v1-only\n";
    return 0;
}

int callGenerationBinding(
    const luna::runtime::MoonRuntime::PinnedBinding& binding) {
    if (!binding ||
        binding.declarationKind() != LUNA_NATIVE_DECLARATION_FUNCTION_V1 ||
        (binding.flags() & LUNA_NATIVE_EXPORT_CALLABLE_V1) == 0 ||
        binding.entryAbi() !=
            luna::runtime::GenerationEntryAbiCI32NoArgsV1)
        return -1;
    int32_t result = 0;
    return binding.callI32NoArgs(result) ? result : -1;
}

int pinnedBindingOutlivesRuntime(int argc, char** argv) {
    if (argc != 6) return 2;
    luna::runtime::MoonRuntime::PinnedBinding pinned;
    {
        luna::runtime::MoonRuntime runtime;
        luna::runtime::MoonRuntime::PinnedGeneration loaded;
        std::string error;
        if (!luna::driver::loadVerifiedNativeGenerationOnce(
                runtime, argv[2], argv[3], loaded, error)) {
            std::cerr << error << '\n';
            return 1;
        }
        const luna::runtime::GenerationBindingRequirement requirement{
            argv[4], argv[5], LUNA_NATIVE_DECLARATION_FUNCTION_V1,
            LUNA_NATIVE_EXPORT_CALLABLE_V1,
            luna::runtime::GenerationEntryAbiCI32NoArgsV1};
        pinned = loaded.find(requirement);
        if (callGenerationBinding(pinned) != 7)
            return fail("independent v2 generation did not expose a pinned call");
    }
    if (pinned.symbolId() != argv[4] ||
        pinned.contractId() != argv[5] ||
        callGenerationBinding(pinned) != 7)
        return fail("pinned Native binding lost its library after runtime destruction");
    std::cout << "pinned-v2\n";
    return 0;
}

int generationSwitch(int argc, char** argv) {
    if (argc != 8) {
        std::cerr << "usage: native-artifact-test --generation-switch "
                     "<first-artifact> <first-trust> <second-artifact> "
                     "<second-trust> <symbol-id> <contract-id>\n";
        return 2;
    }
    luna::runtime::MoonRuntime loadOnceRuntime;
    luna::runtime::MoonRuntime::PinnedGeneration loadedOnce;
    std::string error;
    if (!luna::driver::loadVerifiedNativeGenerationOnce(
            loadOnceRuntime, argv[2], argv[3], loadedOnce, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    const luna::runtime::GenerationBindingRequirement typedFunction{
        argv[6], argv[7], LUNA_NATIVE_DECLARATION_FUNCTION_V1,
        LUNA_NATIVE_EXPORT_CALLABLE_V1,
        luna::runtime::GenerationEntryAbiCI32NoArgsV1};
    const auto firstLoadedBinding = loadedOnce.find(typedFunction);
    if (callGenerationBinding(firstLoadedBinding) != 42 ||
        loadOnceRuntime.retainedGenerationCount(loadedOnce.moduleId()) != 1)
        return fail("Native load-once did not expose one typed generation");
    auto unprofiled = typedFunction;
    unprofiled.entryAbi = luna::runtime::GenerationEntryAbiUnprofiled;
    if (loadedOnce.find(unprofiled))
        return fail("Native typed generation matched an unprofiled requirement");
    luna::runtime::MoonRuntime::PinnedGeneration duplicateLoaded;
    if (!luna::driver::loadVerifiedNativeGenerationOnce(
            loadOnceRuntime, argv[2], argv[3], duplicateLoaded, error) ||
        duplicateLoaded.generationId() != loadedOnce.generationId() ||
        callGenerationBinding(duplicateLoaded.find(typedFunction)) != 42 ||
        loadOnceRuntime.retainedGenerationCount(loadedOnce.moduleId()) != 1)
        return fail("Native same-content load did not reuse its first generation");
    luna::runtime::MoonRuntime::PinnedGeneration changedLoaded;
    if (luna::driver::loadVerifiedNativeGenerationOnce(
            loadOnceRuntime, argv[4], argv[5], changedLoaded, error) ||
        error.find("different content") == std::string::npos ||
        callGenerationBinding(loadedOnce.find(typedFunction)) != 42)
        return fail("Native load-once allowed a different image to replace the module");
    luna::runtime::MoonRuntime::StagedGeneration loadProbe;
    if (!luna::driver::stageVerifiedNativeGeneration(
            loadOnceRuntime, argv[2], argv[3], {}, loadProbe, error) ||
        loadProbe.generationId() != loadedOnce.generationId() + 1)
        return fail("Native load-once materialized a discarded generation");
    luna::runtime::MoonRuntime::PinnedGeneration probeLoaded;
    if (!loadOnceRuntime.loadOnce(loadProbe, probeLoaded, error) ||
        probeLoaded.generationId() != loadedOnce.generationId())
        return fail("Native load-once probe did not reuse the first generation");

    luna::runtime::MoonRuntime runtime;
    luna::runtime::MoonRuntime::StagedGeneration first;
    if (!luna::driver::stageVerifiedNativeGeneration(
            runtime, argv[2], argv[3], {}, first, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    const uint64_t firstId = first.generationId();
    const std::string moduleId = first.moduleId();
    auto firstSafePoint = runtime.safePoint();
    if (!runtime.activate(first, firstSafePoint, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    // Generation module identity comes from the proof, never from a path.
    auto pinnedFirst = runtime.pin(moduleId).find(argv[6], argv[7]);
    luna::runtime::MoonRuntime::SwitchableBinding switchable;
    if (!runtime.makeSwitchable(
            moduleId, typedFunction,
            switchable, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    luna::runtime::MoonRuntime::StagedGeneration second;
    if (!luna::driver::stageVerifiedNativeGeneration(
            runtime, argv[4], argv[5], {}, second, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    auto secondSafePoint = runtime.safePoint();
    if (!runtime.activate(second, secondSafePoint, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    auto pinnedSecond = switchable.pin();
    auto rollbackSafePoint = runtime.safePoint();
    if (!runtime.rollback(
            moduleId, firstId,
            rollbackSafePoint, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << callGenerationBinding(pinnedFirst) << ' '
              << callGenerationBinding(pinnedSecond) << ' '
              << callGenerationBinding(switchable.pin()) << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && (std::string(argv[1]) == "--prepare-legacy" ||
                     std::string(argv[1]) == "--prepare-legacy-invalid-utf8" ||
                     std::string(argv[1]) == "--prepare-v2-fixture" ||
                     std::string(argv[1]) == "--prepare-v3-fixture" ||
                     std::string(argv[1]) == "--prepare-v3-dependency-fixture" ||
                     std::string(argv[1]) == "--prepare-v4-fixture" ||
                     std::string(argv[1]) == "--prepare-v4-unbound-fixture" ||
                     std::string(argv[1]) == "--prepare-v4-dependency-fixture" ||
                     std::string(argv[1]) == "--prepare-v4-bad-module-fixture" ||
                     std::string(argv[1]) == "--prepare-v4-missing-slot-fixture"))
        return prepareIndependentFixture(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "--generation-switch")
        return generationSwitch(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "--pinned-binding-outlives-runtime")
        return pinnedBindingOutlivesRuntime(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "--load-call")
        return loadAndCall(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "--load-typed-call")
        return loadTypedAndCall(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "--inspect-v3-candidate")
        return inspectV3Candidate(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "--inspect-v4-candidate")
        return inspectV4Candidate(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "--call-produced-ref-unit")
        return callProducedRefUnit(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "--load-legacy-generation")
        return loadLegacyGeneration(argc, argv);
    if (argc == 4 && std::string(argv[1]) == "--load-only") {
        luna::driver::VerifiedNativeLibrary library;
        std::string error;
        if (!luna::driver::loadVerifiedNativeLibrary(
                argv[2], argv[3], library, error)) {
            std::cerr << error << '\n';
            return 1;
        }
        return 0;
    }
    if (argc != 3) {
        std::cerr << "usage: native-artifact-test <artifact> <trust-store>\n";
        return 2;
    }
    luna::driver::NativeProofInfo info;
    std::string error;
    if (!luna::driver::verifyNativeArtifact(
            argv[1], argv[2], info, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << info.packageId << '\n'
              << info.packageVersion << '\n'
              << info.targetAbi << '\n'
              << info.compilerIdentity << '\n'
              << luna::driver::nativeDigestHex(info.artifactDigest) << '\n';
    return 0;
}
