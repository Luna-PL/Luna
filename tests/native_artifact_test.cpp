#include "driver/NativeArtifact.h"
#include "driver/NativeGeneration.h"
#include "driver/NativeTypedDescriptor.h"

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

int prepareIndependentFixture(int argc, char** argv) {
    if (argc != 4) return 2;
    const bool invalidUtf8 =
        std::string(argv[1]) == "--prepare-legacy-invalid-utf8";
    const bool typedV2 =
        std::string(argv[1]) == "--prepare-v2-fixture";
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
    auto binding = loaded.find(unprofiled);
    int32_t ignored = 0;
    if (!binding || binding.entryAbi() !=
            luna::runtime::GenerationEntryAbiUnprofiled ||
        loaded.find(typed) || binding.callI32NoArgs(ignored))
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
                     std::string(argv[1]) == "--prepare-v2-fixture"))
        return prepareIndependentFixture(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "--generation-switch")
        return generationSwitch(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "--pinned-binding-outlives-runtime")
        return pinnedBindingOutlivesRuntime(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "--load-call")
        return loadAndCall(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "--load-typed-call")
        return loadTypedAndCall(argc, argv);
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
