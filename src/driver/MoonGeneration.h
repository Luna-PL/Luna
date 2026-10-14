#pragma once

#include "runtime/MoonRuntime.h"
#include "moonir/MoonIRModule.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifdef LUNA_PRIVATE_REF_JIT_TEST
class LunaPrivateRefUnitApplyLoadedEntry;
#endif

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

#ifdef LUNA_PRIVATE_REF_JIT_TEST
// Quarantined test-only staging of a prepared 0.4 Ref/unit consumer. The
// retained entry is reachable only through the returned weak reference while
// the staged or pinned generation owns its module lease. The verified owner
// bytes must identify the pinned owner generation and its exported Slot. No
// callable binding or Ref/unit candidate is published to the Runtime.
bool stagePrivateNativeRefUnitPreparationForTest(
    luna::runtime::MoonRuntime& runtime,
    const std::vector<uint8_t>& containerBytes,
    const std::vector<uint8_t>& ownerContainerBytes,
    const luna::runtime::MoonRuntime::PinnedGeneration& ownerGeneration,
    const std::string& expectedTargetTriple,
    const std::string& expectedDataLayout,
    luna::runtime::MoonRuntime::StagedGeneration& staged,
    std::weak_ptr<LunaPrivateRefUnitApplyLoadedEntry>& entry,
    std::string& error);
#endif

} // namespace luna::driver
