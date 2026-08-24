#pragma once

#include "../core/StableIdentity.h"
#include "../core/TypeSystem.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace luna::selector {

enum class Retention : uint8_t {
    CompileTime,
    Runtime,
    Dynamic,
};

using MetadataValue = std::variant<int64_t, double, bool, std::string>;

struct Metadata {
    std::string schemaId;
    std::vector<MetadataValue> values;
    Retention retention = Retention::CompileTime;
};

struct Candidate {
    std::string declarationId;
    std::string symbolName;
    std::string familyId;
    TypePtr callableType;
    Retention retention = Retention::CompileTime;
    std::vector<Metadata> metadata;
};

using CatalogSymbolKind = luna::sysmeta::DeclarationKind;

enum class QueryPhase : uint8_t {
    CompileTime,
    Runtime,
};

// Stable, typed catalog row. `declarationId` is the canonical payload from
// which SymbolId is derived; linkage/source names are never lookup keys.
struct CatalogSymbol {
    luna::identity::SymbolId symbolId;
    luna::identity::SymbolId familyId;
    luna::identity::ContractId contractId;
    luna::identity::TypeId typeId;
    std::string declarationId;
    std::string familyDeclarationId;
    std::string canonicalContract;
    std::string symbolName;
    CatalogSymbolKind kind = CatalogSymbolKind::Function;
    TypePtr type;
    Retention retention = Retention::CompileTime;
    std::vector<Metadata> metadata;
};

struct SymbolQuery {
    QueryPhase phase = QueryPhase::CompileTime;
    CatalogSymbolKind kind = CatalogSymbolKind::Function;
    std::optional<luna::identity::SymbolId> familyId;
    std::optional<luna::identity::TypeId> typeId;
};

enum class TerminalKind : uint8_t {
    Unique,
    EmptyOptional,
    NoMatch,
    Ambiguous,
    InvalidCatalog,
    InvalidCandidate,
};

struct TerminalResult {
    TerminalKind kind = TerminalKind::NoMatch;
    std::optional<CatalogSymbol> selected;
    std::string message;

    bool oneSucceeded() const {
        return kind == TerminalKind::Unique && selected.has_value();
    }
    bool optionalSucceeded() const {
        return oneSucceeded() || kind == TerminalKind::EmptyOptional;
    }
};

class SymbolCatalog;

// A typed finite set produced by a catalog query. It intentionally exposes no
// `.all()` terminal until TBD-Q004 freezes ordering. `legacyTraversal()` only
// preserves the existing source selector evaluator and is not a public query
// ordering promise.
class SymbolSet {
public:
    bool valid() const { return mValid; }
    const std::string& error() const { return mError; }
    size_t size() const { return mSymbols.size(); }
    const CatalogSymbol* find(
        const luna::identity::SymbolId& symbolId) const;
    const CatalogSymbol* findDeclaration(
        const std::string& declarationId) const;
    const std::vector<const CatalogSymbol*>& legacyTraversal() const {
        return mSymbols;
    }

    SymbolSet select(
        const std::vector<luna::identity::SymbolId>& symbolIds) const;
    SymbolSet filterMetadata(
        const std::string& schemaId,
        const std::vector<MetadataValue>& values) const;
    TerminalResult one() const;
    TerminalResult optional() const;

private:
    friend class SymbolCatalog;
    SymbolSet(
        std::shared_ptr<const std::vector<CatalogSymbol>> owner,
        std::vector<const CatalogSymbol*> symbols)
        : mOwner(std::move(owner)), mSymbols(std::move(symbols)) {}
    SymbolSet(std::string error, TerminalKind failureKind)
        : mValid(false), mFailureKind(failureKind),
          mError(std::move(error)) {}

    std::shared_ptr<const std::vector<CatalogSymbol>> mOwner;
    std::vector<const CatalogSymbol*> mSymbols;
    bool mValid = true;
    TerminalKind mFailureKind = TerminalKind::InvalidCatalog;
    std::string mError;
};

// Immutable after construction: all rows and identities are validated once, and
// callers receive only const views/typed sets.
class SymbolCatalog {
public:
    explicit SymbolCatalog(std::vector<CatalogSymbol> symbols = {});

    bool valid() const { return mValid; }
    const std::string& error() const { return mError; }
    size_t size() const { return mSymbols->size(); }
    const CatalogSymbol* find(
        const luna::identity::SymbolId& symbolId) const;
    const CatalogSymbol* findDeclaration(
        const std::string& declarationId) const;
    SymbolSet query(const SymbolQuery& query) const;

private:
    std::shared_ptr<const std::vector<CatalogSymbol>> mSymbols;
    bool mValid = true;
    std::string mError;
};

// Immutable finite view supplied by a select expression through the public
// selector protocol. Frontend built-ins expose iteration and reflection while
// this component owns the view's membership/same-family invariants.
class DeclarationView {
public:
    explicit DeclarationView(std::vector<Candidate> candidates = {});

    const std::vector<Candidate>& candidates() const { return mCandidates; }
    const Candidate* find(const std::string& declarationId) const;
    const std::string& familyId() const { return mFamilyId; }
    bool valid() const { return mValid; }
    const std::string& error() const { return mError; }

private:
    std::vector<Candidate> mCandidates;
    std::string mFamilyId;
    bool mValid = true;
    std::string mError;
};

enum class ResultKind {
    Unique,
    NoMatch,
    Ambiguous,
    InvalidCandidate,
    InvalidView,
};

struct Result {
    ResultKind kind = ResultKind::NoMatch;
    std::optional<Candidate> selected;
    std::string message;

    bool success() const { return kind == ResultKind::Unique && selected.has_value(); }
};

struct DynamicPlan {
    std::string familyId;
    std::vector<std::string> candidateIds;
    std::string selectorDeclarationId;
};

class Engine {
public:
    // The ordinary selector function returns declaration identities. Engine
    // enforces the public membership rule and accepts exactly one member of
    // the supplied view.
    Result validate(const DeclarationView& view,
                    const std::vector<std::string>& returnedIds) const;

    std::optional<DynamicPlan> planDynamic(
        const DeclarationView& view,
        const std::string& selectorDeclarationId,
        std::string& error) const;
};

} // namespace luna::selector
