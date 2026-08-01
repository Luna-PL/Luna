#include "tooling/ReferenceIndex.h"

#include "sema/SemanticAnalyzer.h"

#include <algorithm>
#include <unordered_map>

namespace luna::tooling {

ReferenceIndex ReferenceIndex::build(
    const SemanticAnalyzer& semanticAnalyzer, const SymbolIndex& symbols) {
    ReferenceIndex result;
    std::unordered_map<std::string, std::string> idsByLinkage;
    for (const auto& symbol : symbols.declarations())
        idsByLinkage.emplace(symbol.linkageName, symbol.id);

    for (const auto& reference : semanticAnalyzer.declarationReferences()) {
        const auto target = idsByLinkage.find(reference.targetLinkageName);
        if (target == idsByLinkage.end()) continue;
        result.mReferences.push_back({
            target->second,
            {
                reference.sourcePath,
                reference.line,
                reference.column,
                reference.byteLength,
            },
        });
    }

    std::sort(result.mReferences.begin(), result.mReferences.end(),
              [](const IndexedReference& left, const IndexedReference& right) {
                  if (left.source.path != right.source.path)
                      return left.source.path < right.source.path;
                  if (left.source.line != right.source.line)
                      return left.source.line < right.source.line;
                  if (left.source.column != right.source.column)
                      return left.source.column < right.source.column;
                  return left.targetId < right.targetId;
              });
    result.mReferences.erase(
        std::unique(result.mReferences.begin(), result.mReferences.end(),
                    [](const IndexedReference& left,
                       const IndexedReference& right) {
                        return left.targetId == right.targetId &&
                            left.source.path == right.source.path &&
                            left.source.line == right.source.line &&
                            left.source.column == right.source.column;
                    }),
        result.mReferences.end());
    return result;
}

std::vector<const IndexedReference*> ReferenceIndex::inDocument(
    const std::string& path) const {
    std::vector<const IndexedReference*> result;
    for (const auto& reference : mReferences)
        if (reference.source.path == path) result.push_back(&reference);
    return result;
}

} // namespace luna::tooling
