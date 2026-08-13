#pragma once

#include "MoonIR.h"

#include <string>
#include <vector>

namespace moon {

// Transactional construction boundary for concrete function bodies. Generic,
// selector, deferred-kernel, and fragment recipes remain compiler input until
// their dedicated canonicalization slices are complete.
class Sealer {
public:
    bool sealFunctionBodies(Module& module);

    const std::vector<std::string>& errors() const { return mErrors; }

private:
    std::vector<std::string> mErrors;
};

} // namespace moon
