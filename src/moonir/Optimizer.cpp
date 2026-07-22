#include "Optimizer.h"

namespace moon {

bool Optimizer::run(Module& module, const OptimizationRequest& request) {
    mErrors.clear();

    // The first implementation intentionally performs only representation
    // canonicalization. Language-level transforms and runtime hotspot
    // versioning will be added as verified MoonIR-to-MoonIR passes; LLVM
    // optimization remains a separate backend concern.
    (void)request;
    canonicalize(module);
    return mErrors.empty();
}

void Optimizer::canonicalize(Module& module) {
    // Lookup maps are derived state and never part of a Moon container. Always
    // rebuild them at this boundary so a deserialized or transformed module
    // cannot carry stale frontend pointers into a backend.
    module.rebuildIndexes();
}

} // namespace moon
