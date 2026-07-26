#pragma once

#include "TypeSystem.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace luna::layout {

inline constexpr uint32_t InlineAdtAbiVersion = 1;
inline constexpr uint64_t InlineTagStorageSize = 8;
inline constexpr uint64_t InlinePayloadAlignment = 8;

uint64_t alignTo(uint64_t value, uint64_t alignment);

// Size/alignment of a Luna value in the compiler/MoonIR ABI. Nominal product
// types are pointer represented; arrays, slices, Result and enum sums are
// inline values.
uint64_t valueSize(const TypePtr& type);
uint64_t valueAlignment(const TypePtr& type);

uint64_t variantFieldOffset(const TypeVariant& variant, size_t fieldIndex);
uint64_t variantPayloadSize(const TypeVariant& variant);
uint64_t enumPayloadSize(const TypePtr& type);

// A target-independent description used by diagnostics, documentation and
// future Moon container compatibility checks.
std::string inlineAdtLayoutSignature(const TypePtr& type);

} // namespace luna::layout
