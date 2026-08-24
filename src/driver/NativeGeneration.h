#pragma once

#include "runtime/MoonRuntime.h"

#include <string>

namespace luna::driver {

// Internal artifact-to-generation adapter. This deliberately does not define
// the public activation spelling deferred by TBD-EV004.
bool stageVerifiedNativeGeneration(
    luna::runtime::MoonRuntime& runtime,
    const std::string& artifactPath, const std::string& trustStorePath,
    const luna::runtime::GenerationInitializer& initializer,
    luna::runtime::MoonRuntime::StagedGeneration& staged,
    std::string& error);

} // namespace luna::driver
