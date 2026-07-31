#pragma once

#include "diagnostics/Diagnostic.h"

#include "MoonIR.h"

#include <string>
#include <vector>

namespace moon {

// MoonIR optimization is deliberately independent from LLVM optimization.
// The same boundary can later be reused by MoonRuntime after container
// validation, without importing the Luna frontend or an LLVM context.
enum class OptimizationLevel : uint8_t {
    None,
    Standard,
    Aggressive,
};

enum class OptimizationPurpose : uint8_t {
    AheadOfTime,
    JustInTime,
    RuntimeHotspot,
};

struct OptimizationRequest {
    OptimizationLevel level = OptimizationLevel::None;
    OptimizationPurpose purpose = OptimizationPurpose::AheadOfTime;
};

class Optimizer {
public:
    bool run(Module& module, const OptimizationRequest& request = {});
    const std::vector<diagnostic::Diagnostic>& errors() const { return mErrors; }

private:
    void canonicalize(Module& module);

    std::vector<diagnostic::Diagnostic> mErrors;
};

} // namespace moon
