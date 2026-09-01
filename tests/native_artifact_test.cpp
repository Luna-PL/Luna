#include "driver/NativeArtifact.h"
#include "driver/NativeGeneration.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace {

int fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
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

int callGenerationBinding(
    const luna::runtime::MoonRuntime::PinnedBinding& binding) {
    if (!binding ||
        binding.declarationKind() != LUNA_NATIVE_DECLARATION_FUNCTION_V1 ||
        (binding.flags() & LUNA_NATIVE_EXPORT_CALLABLE_V1) == 0 ||
        !binding.implementation())
        return -1;
    using AnswerFunction = int32_t (*)();
    static_assert(sizeof(AnswerFunction) == sizeof(const void*),
                  "test host cannot represent a Native entry pointer");
    const void* entry = binding.implementation();
    AnswerFunction answer = nullptr;
    std::memcpy(&answer, &entry, sizeof(answer));
    return answer();
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
        LUNA_NATIVE_EXPORT_CALLABLE_V1};
    const auto firstLoadedBinding = loadedOnce.find(typedFunction);
    if (callGenerationBinding(firstLoadedBinding) != 42 ||
        loadOnceRuntime.retainedGenerationCount(loadedOnce.moduleId()) != 1)
        return fail("Native load-once did not expose one typed generation");
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
            moduleId, argv[6], argv[7],
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
    if (argc > 1 && std::string(argv[1]) == "--generation-switch")
        return generationSwitch(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "--load-call")
        return loadAndCall(argc, argv);
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
