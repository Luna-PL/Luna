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

// Complete verified artifact -> pinned module loop for the non-evolving case.
// Re-loading identical content returns the existing generation; another image
// with the same Package ID is rejected until the evolution API is requested.
bool loadVerifiedNativeGenerationOnce(
    luna::runtime::MoonRuntime& runtime,
    const std::string& artifactPath, const std::string& trustStorePath,
    luna::runtime::MoonRuntime::PinnedGeneration& loaded,
    std::string& error);

} // namespace luna::driver
