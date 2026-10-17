#pragma once

#include "runtime/MoonRuntime.h"

#include <string>

namespace luna::driver {

// Compiler-owned trusted-artifact adapter for the public MoonRuntime control
// plane. It is not a second activation API.
bool stageVerifiedNativeGeneration(
    luna::runtime::MoonRuntime& runtime,
    const std::string& artifactPath, const std::string& trustStorePath,
    const luna::runtime::GenerationInitializer& initializer,
    luna::runtime::MoonRuntime::StagedGeneration& staged,
    std::string& error);

#ifdef LUNA_NATIVE_ADMISSION_TEST
// Read-only probe of the production verifier against a verified artifact.
bool verifyNativeRequestMutationForTest(
    const std::string& artifactPath, const std::string& trustStorePath,
    const std::function<void(luna::runtime::GenerationStagingRequest&)>& mutate,
    std::string& error);
bool verifyAlternateNativeCallerForTest(
    const std::string& artifactPath, const std::string& trustStorePath,
    std::string& error);
// Publish a competing generation between verified staging and load-once.
bool loadVerifiedNativeGenerationOnceWithPrePublishHookForTest(
    luna::runtime::MoonRuntime& runtime,
    const std::string& artifactPath, const std::string& trustStorePath,
    luna::runtime::MoonRuntime::PinnedGeneration& loaded,
    std::string& error,
    const std::function<bool(
        const luna::runtime::GenerationStagingRequest&,
        const std::vector<luna::runtime::GenerationBinding>&,
        std::string&)>& beforePublication);
#endif

// Complete verified artifact -> pinned module loop for the non-evolving case.
// Re-loading reuses only an active generation previously issued by this
// adapter, with the exact verified library lease and full snapshot. A foreign
// same-digest generation is rejected.
bool loadVerifiedNativeGenerationOnce(
    luna::runtime::MoonRuntime& runtime,
    const std::string& artifactPath, const std::string& trustStorePath,
    luna::runtime::MoonRuntime::PinnedGeneration& loaded,
    std::string& error);

} // namespace luna::driver
