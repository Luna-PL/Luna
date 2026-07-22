#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace luna::macro {

struct ExpansionLimits {
    size_t maxDepth = 64;
    size_t maxGeneratedBytes = 16 * 1024 * 1024;
    size_t maxExpansions = 10000;
};

struct SourceUnit {
    std::string path;
    std::string source;
};

struct Expansion {
    std::string source;
    // The no-op implementation has a one-to-one mapping. A real hygienic
    // expander will append a provenance entry for every generated range.
    std::vector<std::string> provenance;
    size_t expansionCount = 0;
};

class MacroProcessor {
public:
    explicit MacroProcessor(ExpansionLimits limits = {});

    bool process(const SourceUnit& input, Expansion& output,
                 std::vector<std::string>& errors) const;

    const ExpansionLimits& limits() const { return mLimits; }

private:
    ExpansionLimits mLimits;
};

} // namespace luna::macro
