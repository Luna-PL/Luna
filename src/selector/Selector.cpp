#include "Selector.h"
#include "../core/TypeRelations.h"

#include <unordered_set>

namespace luna::selector {

SymbolCatalog::SymbolCatalog(std::vector<CatalogSymbol> symbols)
    : mSymbols(std::make_shared<const std::vector<CatalogSymbol>>(
          std::move(symbols))) {
    std::unordered_set<std::string> symbolIds;
    for (const auto& symbol : *mSymbols) {
        if (symbol.declarationId.empty()) {
            mValid = false;
            mError = "Symbol Catalog row has no canonical declaration identity";
            return;
        }
        if (symbol.symbolId.empty() ||
            symbol.symbolId != luna::identity::symbolIdFromCanonical(
                symbol.declarationId)) {
            mValid = false;
            mError = "Symbol Catalog row has a missing or inconsistent SymbolId";
            return;
        }
        if (symbol.familyDeclarationId.empty() ||
            symbol.familyId.empty() ||
            symbol.familyId != luna::identity::symbolIdFromCanonical(
                symbol.familyDeclarationId)) {
            mValid = false;
            mError = "Symbol Catalog row has a missing or inconsistent family SymbolId";
            return;
        }
        if (symbol.canonicalContract.empty() || symbol.contractId.empty() ||
            symbol.contractId != luna::identity::contractIdFromCanonical(
                symbol.canonicalContract)) {
            mValid = false;
            mError = "Symbol Catalog row has a missing or inconsistent ContractId";
            return;
        }
        if (!symbol.type || symbol.typeId.empty() ||
            symbol.typeId != luna::types::typeId(symbol.type)) {
            mValid = false;
            mError = "Symbol Catalog row has a missing or inconsistent TypeId";
            return;
        }
        if (!symbolIds.insert(symbol.symbolId.value).second) {
            mValid = false;
            mError = "Symbol Catalog contains duplicate SymbolId '" +
                     symbol.symbolId.value + "'";
            return;
        }
    }
}

const CatalogSymbol* SymbolCatalog::find(
    const luna::identity::SymbolId& symbolId) const {
    for (const auto& symbol : *mSymbols)
        if (symbol.symbolId == symbolId) return &symbol;
    return nullptr;
}

const CatalogSymbol* SymbolCatalog::findDeclaration(
    const std::string& declarationId) const {
    for (const auto& symbol : *mSymbols)
        if (symbol.declarationId == declarationId) return &symbol;
    return nullptr;
}

SymbolSet SymbolCatalog::query(const SymbolQuery& query) const {
    if (!mValid)
        return SymbolSet(mError, TerminalKind::InvalidCatalog);
    std::vector<const CatalogSymbol*> matches;
    for (const auto& symbol : *mSymbols) {
        if (symbol.kind != query.kind) continue;
        if (query.phase == QueryPhase::Runtime &&
            symbol.retention == Retention::CompileTime)
            continue;
        if (query.familyId && symbol.familyId != *query.familyId) continue;
        if (query.typeId && symbol.typeId != *query.typeId) continue;
        matches.push_back(&symbol);
    }
    return SymbolSet(mSymbols, std::move(matches));
}

const CatalogSymbol* SymbolSet::find(
    const luna::identity::SymbolId& symbolId) const {
    for (const auto* symbol : mSymbols)
        if (symbol && symbol->symbolId == symbolId) return symbol;
    return nullptr;
}

const CatalogSymbol* SymbolSet::findDeclaration(
    const std::string& declarationId) const {
    for (const auto* symbol : mSymbols)
        if (symbol && symbol->declarationId == declarationId) return symbol;
    return nullptr;
}

SymbolSet SymbolSet::select(
    const std::vector<luna::identity::SymbolId>& symbolIds) const {
    if (!mValid) return SymbolSet(mError, mFailureKind);
    std::vector<const CatalogSymbol*> selected;
    std::unordered_set<std::string> seen;
    for (const auto& symbolId : symbolIds) {
        const auto* symbol = find(symbolId);
        if (!symbol) {
            return SymbolSet(
                "selector returned a declaration outside its supplied candidate view",
                TerminalKind::InvalidCandidate);
        }
        if (!seen.insert(symbolId.value).second) {
            return SymbolSet(
                "selector returned the same declaration more than once",
                TerminalKind::Ambiguous);
        }
        selected.push_back(symbol);
    }
    return SymbolSet(mOwner, std::move(selected));
}

SymbolSet SymbolSet::filterMetadata(
    const std::string& schemaId,
    const std::vector<MetadataValue>& values) const {
    if (!mValid) return SymbolSet(mError, mFailureKind);
    std::vector<const CatalogSymbol*> matches;
    for (const auto* symbol : mSymbols) {
        if (!symbol) continue;
        for (const auto& metadata : symbol->metadata) {
            if (metadata.schemaId == schemaId && metadata.values == values) {
                matches.push_back(symbol);
                break;
            }
        }
    }
    return SymbolSet(mOwner, std::move(matches));
}

TerminalResult SymbolSet::one() const {
    if (!mValid) return {mFailureKind, std::nullopt, mError};
    if (mSymbols.empty())
        return {TerminalKind::NoMatch, std::nullopt,
                "symbol query .one() matched no declarations"};
    if (mSymbols.size() != 1)
        return {TerminalKind::Ambiguous, std::nullopt,
                "symbol query .one() matched multiple declarations"};
    return {TerminalKind::Unique, *mSymbols.front(), {}};
}

TerminalResult SymbolSet::optional() const {
    if (!mValid) return {mFailureKind, std::nullopt, mError};
    if (mSymbols.empty())
        return {TerminalKind::EmptyOptional, std::nullopt, {}};
    if (mSymbols.size() != 1)
        return {TerminalKind::Ambiguous, std::nullopt,
                "symbol query .optional() matched multiple declarations"};
    return {TerminalKind::Unique, *mSymbols.front(), {}};
}

DeclarationView::DeclarationView(std::vector<Candidate> candidates)
    : mCandidates(std::move(candidates)) {
    std::unordered_set<std::string> ids;
    for (const auto& candidate : mCandidates) {
        if (candidate.declarationId.empty()) {
            mValid = false;
            mError = "selector candidate has no declaration identity";
            return;
        }
        if (!ids.insert(candidate.declarationId).second) {
            mValid = false;
            mError = "selector view contains duplicate candidate '" +
                     candidate.declarationId + "'";
            return;
        }
        if (mFamilyId.empty()) mFamilyId = candidate.familyId;
        else if (mFamilyId != candidate.familyId) {
            mValid = false;
            mError = "selector view crosses declaration family boundaries";
            return;
        }
    }
}

const Candidate* DeclarationView::find(const std::string& declarationId) const {
    for (const auto& candidate : mCandidates)
        if (candidate.declarationId == declarationId) return &candidate;
    return nullptr;
}

Result Engine::validate(const DeclarationView& view,
                        const std::vector<std::string>& returnedIds) const {
    if (!view.valid())
        return {ResultKind::InvalidView, std::nullopt, view.error()};
    if (returnedIds.empty())
        return {ResultKind::NoMatch, std::nullopt,
                "selector returned no legal declaration"};
    if (returnedIds.size() != 1)
        return {ResultKind::Ambiguous, std::nullopt,
                "selector must return exactly one declaration"};
    const auto* selected = view.find(returnedIds.front());
    if (!selected)
        return {ResultKind::InvalidCandidate, std::nullopt,
                "selector returned a declaration outside its supplied candidate view"};
    return {ResultKind::Unique, *selected, {}};
}

std::optional<DynamicPlan> Engine::planDynamic(
    const DeclarationView& view, const std::string& selectorDeclarationId,
    std::string& error) const {
    error.clear();
    if (!view.valid()) {
        error = view.error();
        return std::nullopt;
    }
    if (selectorDeclarationId.empty()) {
        error = "dynamic selector has no declaration identity";
        return std::nullopt;
    }
    DynamicPlan plan;
    plan.familyId = view.familyId();
    plan.selectorDeclarationId = selectorDeclarationId;
    for (const auto& candidate : view.candidates()) {
        if (candidate.retention == Retention::CompileTime) {
            error = "dynamic select candidate '" + candidate.declarationId +
                    "' has no runtime descriptor";
            return std::nullopt;
        }
        plan.candidateIds.push_back(candidate.declarationId);
    }
    if (plan.candidateIds.empty()) {
        error = "dynamic select has an empty candidate view";
        return std::nullopt;
    }
    return plan;
}

} // namespace luna::selector
