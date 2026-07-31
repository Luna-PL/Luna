#include "CodeGeneratorRangeAnalysis.h"

using moon::BinaryExpr;
using moon::IdentifierExpr;
using moon::IntLiteralExpr;
using moon::Operator;

namespace luna::codegen {

// A non-negative bit-mask index is statically bounded: x & mask is always in
// [0, mask], even when x is signed. The map contains exclusive upper bounds
// for simple locals whose initializers established such a range. Preserve
// runtime checks for every other expression so safety is never inferred from
// an incomplete range proof.
std::optional<uint64_t> knownArrayIndexUpperBound(
    const moon::Expr* expression,
    const std::unordered_map<std::string, uint64_t>& knownUpperBounds) {
    if (const auto* literal = dynamic_cast<const IntLiteralExpr*>(expression)) {
        if (literal->value < 0) return std::nullopt;
        return static_cast<uint64_t>(literal->value) + 1;
    }
    if (const auto* identifier =
            dynamic_cast<const IdentifierExpr*>(expression)) {
        auto it = knownUpperBounds.find(identifier->name);
        if (it != knownUpperBounds.end()) return it->second;
    }
    const auto* binary = dynamic_cast<const BinaryExpr*>(expression);
    if (!binary || binary->op != Operator::BitAnd) return std::nullopt;
    const auto* mask = dynamic_cast<const IntLiteralExpr*>(binary->rhs.get());
    if (!mask || mask->value < 0) return std::nullopt;
    return static_cast<uint64_t>(mask->value) + 1;
}

bool isProvablySafeArrayIndex(
    const moon::Expr* expression,
    uint64_t length,
    const std::unordered_map<std::string, uint64_t>& knownUpperBounds) {
    auto upperBound =
        knownArrayIndexUpperBound(expression, knownUpperBounds);
    return upperBound && *upperBound <= length;
}

} // namespace luna::codegen
