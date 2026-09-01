#include "selector/Selector.h"
#include "core/TypeRelations.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

int fail(const std::string& message) {
    std::cerr << message << '\n';
    return 1;
}

luna::selector::CatalogSymbol makeSymbol(
    const std::string& declarationId,
    const std::string& familyId,
    TypePtr type,
    luna::selector::Retention retention =
        luna::selector::Retention::CompileTime) {
    luna::selector::CatalogSymbol symbol;
    symbol.declarationId = declarationId;
    symbol.symbolName = declarationId;
    symbol.symbolId = luna::identity::symbolIdFromCanonical(declarationId);
    symbol.familyDeclarationId = familyId;
    symbol.familyId = luna::identity::symbolIdFromCanonical(familyId);
    symbol.typeId = luna::types::typeId(type);
    symbol.canonicalContract =
        "test-contract:" + declarationId + ":" + symbol.typeId.value;
    symbol.contractId = luna::identity::contractIdFromCanonical(
        symbol.canonicalContract);
    symbol.retention = retention;
    symbol.type = std::move(type);
    return symbol;
}

} // namespace

int main() {
    using namespace luna::selector;

    const auto callable = Type::makeFunction({}, TyI32);
    const auto otherType = Type::makeFunction({}, TyBool);
    const auto first = makeSymbol("pkg::fn::answer_a", "pkg::fn::answer",
                                  callable);
    const auto second = makeSymbol("pkg::fn::answer_b", "pkg::fn::answer",
                                   callable, Retention::Runtime);
    const auto third = makeSymbol("pkg::fn::other", "pkg::fn::other",
                                  otherType, Retention::Runtime);

    SymbolCatalog catalog({first, second, third});
    if (!catalog.valid()) return fail("valid Symbol Catalog was rejected");
    if (!catalog.find(second.symbolId) ||
        catalog.find(second.symbolId)->contractId != second.contractId)
        return fail("SymbolId lookup lost the typed ContractId");

    SymbolQuery familyQuery;
    familyQuery.familyId = first.familyId;
    familyQuery.typeId = first.typeId;
    auto family = catalog.query(familyQuery);
    if (!family.valid() || family.size() != 2)
        return fail("typed family query returned the wrong membership");
    if (family.one().kind != TerminalKind::Ambiguous)
        return fail(".one() accepted an ambiguous typed set");
    if (family.optional().kind != TerminalKind::Ambiguous)
        return fail(".optional() accepted an ambiguous typed set");

    auto selected = family.select({second.symbolId}).one();
    if (!selected.oneSucceeded() ||
        selected.selected->symbolId != second.symbolId ||
        selected.selected->contractId != second.contractId)
        return fail(".one() did not preserve stable symbol/contract identity");

    SymbolQuery missingQuery;
    missingQuery.typeId = luna::identity::TypeId{"missing_type"};
    auto missing = catalog.query(missingQuery);
    if (missing.one().kind != TerminalKind::NoMatch)
        return fail(".one() did not reject an empty typed set");
    if (!missing.optional().optionalSucceeded() ||
        missing.optional().selected)
        return fail(".optional() did not accept an empty typed set");

    SymbolQuery runtimeQuery;
    runtimeQuery.phase = QueryPhase::Runtime;
    runtimeQuery.familyId = first.familyId;
    runtimeQuery.typeId = first.typeId;
    auto runtime = catalog.query(runtimeQuery);
    auto runtimeOne = runtime.one();
    if (!runtimeOne.oneSucceeded() ||
        runtimeOne.selected->symbolId != second.symbolId)
        return fail("runtime query exposed a compile-time-only symbol");

    auto taggedFirst = first;
    taggedFirst.metadata.push_back(
        {"meta::version", {int64_t{1}}, Retention::CompileTime});
    auto taggedSecond = second;
    taggedSecond.metadata.push_back(
        {"meta::version", {int64_t{2}}, Retention::Runtime});
    auto duplicateAttachment = taggedFirst;
    duplicateAttachment.metadata.push_back(taggedFirst.metadata.front());
    SymbolCatalog metadataCatalog({duplicateAttachment, taggedSecond});
    auto metadataFamily = metadataCatalog.query(familyQuery);
    auto metadataOne = metadataFamily
        .filterMetadata("meta::version", {int64_t{1}})
        .one();
    if (!metadataOne.oneSucceeded() ||
        metadataOne.selected->symbolId != first.symbolId)
        return fail("metadata filter did not preserve set membership semantics");
    if (metadataFamily.filterMetadata(
            "meta::version", {int64_t{3}}).one().kind != TerminalKind::NoMatch)
        return fail("metadata filter did not produce an empty typed set");
    taggedSecond.metadata.clear();
    taggedSecond.metadata.push_back(
        {"meta::version", {int64_t{1}}, Retention::Runtime});
    SymbolCatalog ambiguousMetadata({taggedFirst, taggedSecond});
    if (ambiguousMetadata.query(familyQuery)
            .filterMetadata("meta::version", {int64_t{1}})
            .one().kind != TerminalKind::Ambiguous)
        return fail("metadata filter accepted multiple matching declarations");

    auto orderedFirst = first;
    orderedFirst.metadata.push_back(
        {"meta::rank", {int64_t{2}, std::string{"z"}},
         Retention::CompileTime});
    auto orderedSecond = second;
    orderedSecond.metadata.push_back(
        {"meta::rank", {int64_t{1}, std::string{"a"}},
         Retention::CompileTime});
    SymbolCatalog orderingCatalog({orderedFirst, orderedSecond});
    auto metadataOrdered = orderingCatalog.query(familyQuery)
        .allByMetadata("meta::rank");
    if (!metadataOrdered.valid() ||
        metadataOrdered.orderedSymbols().size() != 2 ||
        metadataOrdered.orderedSymbols()[0]->symbolId != second.symbolId ||
        metadataOrdered.orderedSymbols()[1]->symbolId != first.symbolId)
        return fail("metadata order did not use schema field order lexicographically");

    auto duplicateRank = orderedSecond;
    duplicateRank.metadata.front().values =
        orderedFirst.metadata.front().values;
    if (SymbolCatalog({orderedFirst, duplicateRank}).query(familyQuery)
            .allByMetadata("meta::rank").valid())
        return fail("metadata order accepted a duplicate key");
    if (metadataCatalog.query(familyQuery)
            .allByMetadata("meta::version").valid())
        return fail("metadata order accepted duplicate schema attachments");
    if (SymbolCatalog({orderedFirst, second}).query(familyQuery)
            .allByMetadata("meta::rank").valid())
        return fail("metadata order accepted a missing schema attachment");
    auto floatingRank = first;
    floatingRank.metadata.push_back(
        {"meta::rank", {1.5}, Retention::CompileTime});
    if (SymbolCatalog({floatingRank}).query(familyQuery)
            .allByMetadata("meta::rank").valid())
        return fail("metadata order accepted a floating-point key");

    auto outside = family.select({third.symbolId}).one();
    if (outside.kind != TerminalKind::InvalidCandidate ||
        outside.message.find("outside") == std::string::npos)
        return fail("typed set accepted a symbol outside its membership");

    auto structure = makeSymbol(
        "pkg::struct::Snapshot", "pkg::struct::Snapshot",
        Type::makeStruct("Snapshot", {}, "pkg::struct::Snapshot"));
    structure.kind = CatalogSymbolKind::Struct;
    SymbolCatalog kindCatalog({first, structure});
    SymbolQuery structureQuery;
    structureQuery.kind = CatalogSymbolKind::Struct;
    auto structureResult = kindCatalog.query(structureQuery).one();
    if (!structureResult.oneSucceeded() ||
        structureResult.selected->kind != CatalogSymbolKind::Struct ||
        structureResult.selected->symbolId != structure.symbolId)
        return fail("declaration-kind query did not isolate a non-function row");

    // Cardinality terminals and canonical `.all()` must be invariant under
    // registration order. Exhaust every source/catalog permutation.
    std::vector<CatalogSymbol> permutation{first, second, third};
    std::sort(permutation.begin(), permutation.end(),
              [](const auto& left, const auto& right) {
                  return left.symbolId.value < right.symbolId.value;
              });
    do {
        SymbolCatalog permuted(permutation);
        auto set = permuted.query(familyQuery);
        if (set.size() != 2 || set.one().kind != TerminalKind::Ambiguous ||
            !set.select({first.symbolId}).one().oneSucceeded())
            return fail("query terminal depends on registration order");
        const auto ordered = set.all().orderedSymbols();
        if (ordered.size() != 2 ||
            ordered[0]->symbolId.value > ordered[1]->symbolId.value)
            return fail("canonical .all() depends on registration order");
    } while (std::next_permutation(
        permutation.begin(), permutation.end(),
        [](const auto& left, const auto& right) {
            return left.symbolId.value < right.symbolId.value;
        }));

    SymbolCatalog duplicate({first, first});
    if (duplicate.valid() ||
        duplicate.query(familyQuery).one().kind !=
            TerminalKind::InvalidCatalog)
        return fail("duplicate SymbolId was accepted");

    auto forged = first;
    forged.symbolId = luna::identity::SymbolId{"symbol_forged"};
    if (SymbolCatalog({forged}).valid())
        return fail("inconsistent canonical SymbolId was accepted");

    auto missingContract = first;
    missingContract.contractId = {};
    if (SymbolCatalog({missingContract}).valid())
        return fail("catalog row without ContractId was accepted");

    auto forgedContract = first;
    forgedContract.contractId =
        luna::identity::ContractId{"contract_forged"};
    if (SymbolCatalog({forgedContract}).valid())
        return fail("inconsistent canonical ContractId was accepted");

    auto forgedFamily = first;
    forgedFamily.familyId = luna::identity::SymbolId{"symbol_forged_family"};
    if (SymbolCatalog({forgedFamily}).valid())
        return fail("inconsistent canonical family SymbolId was accepted");

    auto forgedType = first;
    forgedType.typeId = luna::identity::TypeId{"type_forged"};
    if (SymbolCatalog({forgedType}).valid())
        return fail("inconsistent TypeId was accepted");

    // A query set retains its immutable catalog snapshot; returning it from a
    // temporary catalog must not leave dangling symbol rows.
    auto retained = SymbolCatalog({first}).query(familyQuery);
    if (!retained.one().oneSucceeded() ||
        retained.one().selected->symbolId != first.symbolId)
        return fail("typed set did not retain its immutable catalog snapshot");

    luna::sysmeta::Facts facts;
    facts.capability.hostOnly = true;
    const luna::identity::TypeId callableId{"type_callable"};
    const auto canonical = luna::sysmeta::canonicalDeclarationContract(
        CatalogSymbolKind::Function, callableId, facts);
    const auto repeated = luna::sysmeta::canonicalDeclarationContract(
        CatalogSymbolKind::Function, callableId, facts);
    const auto differentKind = luna::sysmeta::canonicalDeclarationContract(
        CatalogSymbolKind::Fragment, callableId, facts);
    if (canonical != repeated || canonical == differentKind)
        return fail("shared declaration ContractId payload is not deterministic");

    return 0;
}
