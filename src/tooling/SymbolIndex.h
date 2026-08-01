#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

struct Program;

namespace luna::tooling {

enum class IndexedSymbolKind {
    Function,
    Kernel,
    Method,
    Fragment,
    Struct,
    Enum,
    Trait,
    Metadata,
    Constraint,
};

struct SymbolSourceLocation {
    std::string path;
    int line = 0;
    int column = 0;
    size_t byteLength = 0;
};

struct IndexedSymbol {
    // Versioned, length-delimited identity assembled from package, module,
    // kind, and the compiler-resolved declaration/linkage identity.
    std::string id;
    std::string name;
    std::string qualifiedName;
    std::string packageId;
    std::string modulePath;
    std::string linkageName;
    std::string signature;
    IndexedSymbolKind kind = IndexedSymbolKind::Function;
    SymbolSourceLocation selection;
    bool exported = false;
    bool external = false;
};

class SymbolIndex {
public:
    static SymbolIndex build(const Program& program);

    const std::vector<IndexedSymbol>& declarations() const {
        return mDeclarations;
    }
    const IndexedSymbol* findById(const std::string& id) const;
    std::vector<const IndexedSymbol*> findByName(const std::string& name) const;
    std::vector<const IndexedSymbol*> inDocument(const std::string& path) const;

private:
    void add(IndexedSymbol symbol);
    void finalize();

    std::vector<IndexedSymbol> mDeclarations;
    std::unordered_map<std::string, size_t> mById;
    std::unordered_multimap<std::string, size_t> mByName;
    std::unordered_multimap<std::string, size_t> mByDocument;
};

const char* indexedSymbolKindName(IndexedSymbolKind kind);

} // namespace luna::tooling
