#pragma once

// Public C++17 host API for Luna 0.3's stateless generation-evolution loop.
// This control-plane API is source-level C++; it is not part of Runtime ABI v1's
// C binary-compatibility contract.
#include "MoonRuntime.h"

namespace luna::runtime {

inline constexpr uint32_t EvolutionApiVersion = 1;

} // namespace luna::runtime
