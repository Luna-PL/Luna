#pragma once

#include "runtime/MoonRuntime.h"

#include <cstdint>
#include <string>
#include <vector>

namespace luna::driver {

// Stages a host-matched, fully verified Moon Container with a retained ORC JIT
// lease for function publications and descriptor-backed non-function exports.
// TBD-EV004's public source/API spelling remains intentionally outside this
// internal adapter.
bool stageVerifiedMoonGeneration(
    luna::runtime::MoonRuntime& runtime,
    const std::vector<uint8_t>& containerBytes,
    const std::string& expectedTargetTriple,
    const std::string& expectedDataLayout,
    const luna::runtime::GenerationInitializer& initializer,
    luna::runtime::MoonRuntime::StagedGeneration& staged,
    std::string& error);

} // namespace luna::driver
