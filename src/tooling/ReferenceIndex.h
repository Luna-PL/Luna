#pragma once

#include "tooling/SymbolIndex.h"

#include <string>
#include <vector>

class SemanticAnalyzer;

namespace luna::tooling {

struct IndexedReference {
    std::string targetId;
    SymbolSourceLocation source;
};

class ReferenceIndex {
public:
    static ReferenceIndex build(const SemanticAnalyzer& semanticAnalyzer,
                                const SymbolIndex& symbols);

    const std::vector<IndexedReference>& references() const {
        return mReferences;
    }
    std::vector<const IndexedReference*> inDocument(
        const std::string& path) const;

private:
    std::vector<IndexedReference> mReferences;
};

} // namespace luna::tooling
