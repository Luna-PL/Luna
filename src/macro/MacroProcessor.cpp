#include "MacroProcessor.h"

#include "../diagnostics/Diagnostic.h"

namespace luna::macro {

MacroProcessor::MacroProcessor(ExpansionLimits limits)
    : mLimits(limits) {}

bool MacroProcessor::process(const SourceUnit& input, Expansion& output,
                             std::vector<std::string>& errors) const {
    output = {};
    if (input.source.size() > mLimits.maxGeneratedBytes) {
        errors.push_back(diagnostic::format(
            "macro", "source exceeds the configured macro expansion byte limit",
            input.path, 0, 0,
            "split the source unit or raise the explicit macro processing limit"));
        return false;
    }

    // Phase A intentionally performs no expansion. Keeping this pass in the
    // live pipeline fixes its ownership and diagnostic boundary before macro
    // syntax and hygiene are introduced.
    output.source = input.source;
    output.provenance.push_back(input.path);
    return true;
}

} // namespace luna::macro
