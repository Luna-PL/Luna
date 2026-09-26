#pragma once

#include "runtime/MoonRuntime.h"
#include "moonir/MoonIRModule.h"

#include <cstdint>
#include <string>
#include <vector>

namespace luna::driver {

// Compiler-owned LLVM middle-end profile; not a source keyword or container
// capability. The legacy/default path remains O0.
enum class MoonJitOptimization { O0, O2, O3 };

// Stages a host-matched, fully verified Moon Container with a retained ORC JIT
// lease for function publications and descriptor-backed non-function exports.
// This compiler-owned artifact adapter feeds the public MoonRuntime control
// plane; packaging it as a standalone host SDK is outside the 0.3 surface.
// Explicit dependency Slot evidence is checked before JIT/staging and even
// before a load-once cache hit. It does not select or activate any Fragment.
// The LLVM version/IR profile/ORC policy form a separate materialization key;
// load-once rejects different keys rather than silently reusing old code.
bool stageVerifiedMoonGeneration(
    luna::runtime::MoonRuntime& runtime,
    const std::vector<uint8_t>& containerBytes,
    const std::string& expectedTargetTriple,
    const std::string& expectedDataLayout,
    const luna::runtime::GenerationInitializer& initializer,
    luna::runtime::MoonRuntime::StagedGeneration& staged,
    std::string& error,
    const moon::SlotPublicationDependencies& dependencies = {},
    MoonJitOptimization optimization = MoonJitOptimization::O0);

bool loadVerifiedMoonGenerationOnce(
    luna::runtime::MoonRuntime& runtime,
    const std::vector<uint8_t>& containerBytes,
    const std::string& expectedTargetTriple,
    const std::string& expectedDataLayout,
    luna::runtime::MoonRuntime::PinnedGeneration& loaded,
    std::string& error,
    const moon::SlotPublicationDependencies& dependencies = {},
    MoonJitOptimization optimization = MoonJitOptimization::O0);

} // namespace luna::driver
