#pragma once

#include "../moonir/MoonIR.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace luna::codegen {

std::optional<uint64_t> knownArrayIndexUpperBound(
    const moon::Expr* expression,
    const std::unordered_map<std::string, uint64_t>& knownUpperBounds);

bool isProvablySafeArrayIndex(
    const moon::Expr* expression,
    uint64_t length,
    const std::unordered_map<std::string, uint64_t>& knownUpperBounds);

} // namespace luna::codegen
