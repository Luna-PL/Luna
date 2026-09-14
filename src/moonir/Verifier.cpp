#include "Verifier.h"
#include "VerifierInternal.h"
#include "../core/TypeLayout.h"

#include "../diagnostics/Diagnostic.h"
#include "../core/TypeRelations.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace moon {

using namespace verifier_detail;

void Verifier::verifyCleanupAction(
    luna::ownership::CleanupAction action,
    const luna::types::TypeId& typeId,
    const SourceLocation& location,
    const std::string& context,
    const Module& module) {
    const auto* type = module.findType(typeId);
    if (!type) return;
    if (!type->sysmeta.resource.cleanupRequired &&
        action != luna::ownership::CleanupAction::Deallocate)
        error(location, context + " targets a type with no cleanup obligation");
    if (type->sysmeta.resource.cleanupRequired &&
        action != type->sysmeta.resource.cleanup)
        error(location, context + " uses cleanup action '" +
              std::string(luna::ownership::cleanupActionName(action)) +
              "' but its frozen ResourceContract requires '" +
              std::string(luna::ownership::cleanupActionName(
                  type->sysmeta.resource.cleanup)) + "'");
}

const DeclarationRecord* Verifier::verifyDeclarationRef(
    const DeclarationRef& reference,
    const SourceLocation& location,
    const std::string& context,
    const Module& module,
    DeclarationKind expectedKind) {
    if (!reference.complete()) {
        error(location, context +
              " has no complete SymbolId/ContractId table reference");
        return nullptr;
    }
    const DeclarationRecord* declaration = nullptr;
    for (const auto& candidate : module.declarationTable) {
        if (candidate.symbolId == reference.symbol) {
            declaration = &candidate;
            break;
        }
    }
    if (!declaration) {
        error(location, context + " references missing SymbolId '" +
              reference.symbol.value + "'");
        return nullptr;
    }
    if (declaration->contractId != reference.contract) {
        error(location, context + " expects ContractId '" +
              reference.contract.value + "' but declaration '" +
              declaration->id + "' provides '" +
              declaration->contractId.value + "'");
        return nullptr;
    }
    if (declaration->kind != expectedKind) {
        error(location, context + " references declaration '" +
              declaration->id + "' of the wrong kind");
        return nullptr;
    }
    return declaration;
}

void Verifier::verifyCanonicalTables(const ControlFlowGraph& graph) {
    const auto checkIndexTable = [this](
        const auto& table, const std::string& name) {
        for (size_t index = 0; index < table.size(); ++index) {
            if (table[index].id.empty() || table[index].id.value != index)
                error({}, name + " table row " + std::to_string(index) +
                          " does not carry its canonical index");
        }
    };
    checkIndexTable(graph.blocks, "block");
    checkIndexTable(graph.regions, "region");
    checkIndexTable(graph.scopes, "scope");
    checkIndexTable(graph.locals, "local");
    checkIndexTable(graph.cleanups, "cleanup");

    const auto* entry = graph.findBlock(graph.entry);
    const auto* rootRegion = graph.findRegion(graph.rootRegion);
    const auto* rootScope = graph.findScope(graph.rootScope);
    if (!entry) error({}, "CFG entry does not reference a canonical block");
    if (!rootRegion) error({}, "CFG root does not reference a canonical region");
    if (!rootScope) error({}, "CFG root does not reference a canonical scope");
    if (rootRegion && !rootRegion->parent.empty())
        error(rootRegion->location, "CFG root region has a parent");
    if (rootScope && !rootScope->parent.empty())
        error(rootScope->location, "CFG root scope has a parent");
    if (entry && (entry->region != graph.rootRegion ||
                  entry->scope != graph.rootScope))
        error(entry->location,
              "CFG entry is outside the root region or root scope");

    const auto verifyParentChains = [this](
        size_t size, const auto& find,
        const auto& parentOf, const std::string& name) {
        for (uint32_t index = 0; index < size; ++index) {
            std::unordered_set<uint32_t> seen;
            auto current = find(index);
            while (current) {
                const auto parent = parentOf(*current);
                if (parent.empty()) break;
                if (!seen.insert(parent.value).second) {
                    error({}, name + " parent chain contains a cycle at row " +
                              std::to_string(index));
                    break;
                }
                current = find(parent.value);
                if (!current) {
                    error({}, name + " parent chain references a missing row from " +
                              std::to_string(index));
                    break;
                }
            }
        }
    };
    verifyParentChains(
        graph.regions.size(),
        [&graph](uint32_t index) -> const RegionRecord* {
            return graph.findRegion(RegionId{index});
        },
        [](const RegionRecord& region) { return region.parent; }, "region");
    verifyParentChains(
        graph.scopes.size(),
        [&graph](uint32_t index) -> const ScopeRecord* {
            return graph.findScope(ScopeId{index});
        },
        [](const ScopeRecord& scope) { return scope.parent; }, "scope");
}

void Verifier::verifyRegions(const ControlFlowGraph& graph, const Module& module) {
    std::vector<uint32_t> blockOwners(graph.blocks.size(), 0);
    for (const auto& region : graph.regions) {
        if (region.kind == RegionKind::Fragment) {
            verifyDeclarationRef(
                region.fragment, region.location,
                "canonical fragment region", module,
                DeclarationKind::Fragment);
            const auto* declaration = module.findDeclaration(
                region.fragment);
            const auto* fragmentType = declaration
                ? module.findType(declaration->type) : nullptr;
            if (!fragmentType || fragmentType->kind != TypeKind::Fragment) {
                error(region.location,
                      "canonical fragment region has no frozen fragment contract");
            } else {
                if (region.parameters.size() !=
                        fragmentType->parameterTypeIds.size() ||
                    region.parameters.size() !=
                        fragmentType->parameterContracts.size())
                    error(region.location,
                          "canonical fragment region parameter arity disagrees with its contract");
                const size_t comparable = std::min(
                    region.parameters.size(),
                    std::min(fragmentType->parameterTypeIds.size(),
                             fragmentType->parameterContracts.size()));
                for (size_t index = 0; index < comparable; ++index) {
                    const auto* local = graph.findLocal(
                        region.parameters[index]);
                    const auto& contract =
                        fragmentType->parameterContracts[index];
                    if (!local || local->scope != region.scope ||
                        local->kind != LocalKind::Binding ||
                        local->type !=
                            fragmentType->parameterTypeIds[index] ||
                        local->relation != contract.relation ||
                        local->usage != contract.usage)
                        error(region.location,
                              "canonical fragment parameter binding disagrees with its frozen contract");
                }
            }
        } else if (!region.fragment.empty()) {
            error(region.location,
                  "non-fragment region carries a fragment declaration reference");
        } else if (!region.parameters.empty()) {
            error(region.location,
                  "non-fragment region carries fragment parameter bindings");
        }
        if (!graph.findScope(region.scope))
            error(region.location, "region references a missing lexical scope");
        if (!graph.findBlock(region.entry))
            error(region.location, "region references a missing entry block");
        else if (std::find(region.blocks.begin(), region.blocks.end(),
                           region.entry) == region.blocks.end())
            error(region.location,
                  "region entry is not a direct member of the region");
        if (!region.exit.empty() && !graph.findBlock(region.exit))
            error(region.location, "region references a missing exit block");
        std::unordered_set<uint32_t> members;
        for (const auto blockId : region.blocks) {
            const auto* block = graph.findBlock(blockId);
            if (!block) {
                error(region.location, "region contains a missing block");
                continue;
            }
            if (!members.insert(blockId.value).second)
                error(region.location, "region repeats block " +
                                       std::to_string(blockId.value));
            if (block->region != region.id)
                error(block->location, "block region reference disagrees with its owner row");
            ++blockOwners[blockId.value];
        }
    }
    for (size_t index = 0; index < blockOwners.size(); ++index)
        if (blockOwners[index] != 1)
            error(graph.blocks[index].location,
                  "block " + std::to_string(index) +
                  " must belong to exactly one direct region");
}

bool Verifier::verify(const ControlFlowGraph& graph, const Module& module) {
    mErrors.clear();
    mVerifiedTypeIds.clear();
    mActiveTypeIds.clear();
    if (!module.typeTableSealed)
        error({}, "MoonIR type table must be sealed before CFG verification");
    if (!graph.sealed)
        error({}, "MoonIR CFG must be sealed before verification");
    if (graph.blocks.empty() || graph.regions.empty() || graph.scopes.empty())
        error({}, "MoonIR CFG is missing a required canonical table");

    verifyCanonicalTables(graph);

    const auto* entry = graph.findBlock(graph.entry);

    verifyRegions(graph, module);


    std::vector<uint32_t> localOwners(graph.locals.size(), 0);
    std::vector<uint32_t> cleanupOwners(graph.cleanups.size(), 0);
    for (const auto& scope : graph.scopes) {
        if (!graph.findRegion(scope.region))
            error(scope.location, "scope references a missing region");
        std::unordered_set<uint32_t> members;
        for (const auto localId : scope.locals) {
            const auto* local = graph.findLocal(localId);
            if (!local) {
                error(scope.location, "scope contains a missing local");
                continue;
            }
            if (!members.insert(localId.value).second)
                error(scope.location, "scope repeats local " +
                                      std::to_string(localId.value));
            if (local->scope != scope.id)
                error(scope.location, "local scope reference disagrees with its owner row");
            ++localOwners[localId.value];
        }
        members.clear();
        for (const auto cleanupId : scope.cleanups) {
            const auto* cleanup = graph.findCleanup(cleanupId);
            if (!cleanup) {
                error(scope.location, "scope contains a missing cleanup");
                continue;
            }
            if (!members.insert(cleanupId.value).second)
                error(scope.location, "scope repeats cleanup " +
                                      std::to_string(cleanupId.value));
            if (cleanup->scope != scope.id)
                error(scope.location, "cleanup scope reference disagrees with its owner row");
            ++cleanupOwners[cleanupId.value];
        }
    }
    for (const auto& region : graph.regions) {
        if (region.id != graph.rootRegion && region.parent.empty())
            error(region.location, "non-root region has no parent");
    }
    for (const auto& scope : graph.scopes) {
        if (scope.id != graph.rootScope && scope.parent.empty())
            error(scope.location, "non-root scope has no parent");
    }
    for (size_t index = 0; index < graph.locals.size(); ++index) {
        const auto& local = graph.locals[index];
        if (localOwners[index] != 1)
            error({}, "local " + std::to_string(index) +
                      " must belong to exactly one scope");
        const auto* type = module.findType(local.type);
        if (!type)
            error({}, "local " + std::to_string(index) +
                      " references a missing frozen type");
        else if (type->kind == TypeKind::Iterator)
            error({}, "sealed CFG retains compiler iterator recipe local " +
                      std::to_string(index));
        else if (local.kind != LocalKind::Allocation &&
                 local.relation == luna::ownership::Relation::Owned &&
                 !luna::ownership::satisfiesUsageRequirement(
                     local.usage, type->sysmeta.resource.usage))
            error({}, "local " + std::to_string(index) +
                      " weakens its frozen usage requirement");
        else if (local.relation != luna::ownership::Relation::Owned &&
                 local.usage != luna::ownership::Usage::Copy)
            error({}, "borrowed local " + std::to_string(index) +
                      " does not use copy cardinality");
        if (local.kind == LocalKind::Allocation &&
            (local.usage != luna::ownership::Usage::Affine ||
             local.relation != luna::ownership::Relation::Owned))
            error({}, "raw allocation local " + std::to_string(index) +
                      " is not an owned affine identity");
    }
    struct GuardedArrayState {
        LocalId nextUnread;
        uint64_t length = 0;
        std::unordered_set<uint64_t> elements;
    };
    std::unordered_map<uint32_t, GuardedArrayState> guardedArrays;
    std::unordered_set<uint32_t> unguardedCleanupRoots;
    std::unordered_set<uint32_t> guardedCursorIds;
    for (size_t index = 0; index < graph.cleanups.size(); ++index) {
        const auto& cleanup = graph.cleanups[index];
        if (cleanupOwners[index] != 1)
            error({}, "cleanup " + std::to_string(index) +
                      " must belong to exactly one scope");
        const auto* local = graph.findLocal(cleanup.place.root);
        if (!local)
            error({}, "cleanup " + std::to_string(index) +
                      " references a missing local");
        else {
            if (local->scope != cleanup.scope) {
                // A guarded array tail cleanup for a materialized consuming
                // source lives in the for-loop's (child) scope while the source
                // local lives in the recipe's outer scope. The tail is cleaned
                // when the child scope exits, before the outer scope, so a
                // guarded cleanup in a descendant scope of the source local is
                // sound. Unguarded cleanups still require the same scope.
                bool guardedDescendantScope = false;
                if (cleanup.guard) {
                    for (const auto* scope = graph.findScope(cleanup.scope);
                         scope; scope = graph.findScope(scope->parent))
                        if (scope->id == local->scope) {
                            guardedDescendantScope = true;
                            break;
                        }
                }
                if (!guardedDescendantScope)
                    error({}, "cleanup " + std::to_string(index) +
                              " targets a local owned by another scope");
            }
            if (local->relation != luna::ownership::Relation::Owned)
                error({}, "cleanup " + std::to_string(index) +
                          " targets a borrowed local");
            TypeRef projectedType = local->type;
            bool validProjection = true;
            for (const auto& projection : cleanup.place.projections) {
                const auto* projected = module.findType(projectedType);
                if (!projected) {
                    validProjection = false;
                    break;
                }
                switch (projection.kind) {
                    case ProjectionKind::Field:
                        if (projection.index >= projected->fields.size()) {
                            validProjection = false;
                        } else {
                            projectedType = projected->fields[
                                static_cast<size_t>(projection.index)].type;
                        }
                        break;
                    case ProjectionKind::ConstantIndex:
                        if ((projected->kind != TypeKind::Array &&
                             projected->kind != TypeKind::Slice) ||
                            (projected->kind == TypeKind::Array &&
                             projection.index >= projected->arrayLength)) {
                            validProjection = false;
                        } else {
                            projectedType = projected->innerTypeId;
                        }
                        break;
                    case ProjectionKind::DynamicIndex: {
                        const auto* indexLocal = graph.findLocal(
                            projection.dynamicIndex);
                        if ((projected->kind != TypeKind::Array &&
                             projected->kind != TypeKind::Slice) ||
                            !indexLocal) {
                            validProjection = false;
                        } else {
                            projectedType = projected->innerTypeId;
                        }
                        break;
                    }
                    case ProjectionKind::Dereference:
                        if (projected->kind != TypeKind::Reference &&
                            projected->kind != TypeKind::RawPointer) {
                            validProjection = false;
                        } else {
                            projectedType = projected->innerTypeId;
                        }
                        break;
                }
                if (!validProjection) break;
            }
            if (!validProjection)
                error({}, "cleanup " + std::to_string(index) +
                          " has an invalid canonical place projection");
            else if (projectedType != cleanup.type)
                error({}, "cleanup " + std::to_string(index) +
                          " type disagrees with its projected place");
        }
        const auto* cleanupType = module.findType(cleanup.type);
        if (cleanup.guard) {
            const auto* cursor = graph.findLocal(
                cleanup.guard->nextUnread);
            const auto* cursorType = cursor
                ? module.findType(cursor->type) : nullptr;
            const auto* rootType = local
                ? module.findType(local->type) : nullptr;
            const bool constantElement =
                cleanup.place.projections.size() == 1 &&
                cleanup.place.projections.front().kind ==
                    ProjectionKind::ConstantIndex;
            if (!local || !rootType || rootType->kind != TypeKind::Array ||
                !constantElement ||
                cleanup.place.projections.front().index !=
                    cleanup.guard->elementIndex ||
                cleanup.guard->elementIndex >= rootType->arrayLength)
                error({}, "guarded cleanup " + std::to_string(index) +
                          " is not one constant array element");
            if (!cursor || cursor->scope != cleanup.scope ||
                cursor->kind != LocalKind::Synthetic ||
                cursor->relation != luna::ownership::Relation::Owned ||
                cursor->usage != luna::ownership::Usage::Copy ||
                !cursorType ||
                (cursorType->kind != TypeKind::I32 &&
                 cursorType->kind != TypeKind::USize))
                error({}, "guarded cleanup " + std::to_string(index) +
                          " has no canonical next-unread cursor");
            if (cursor) guardedCursorIds.insert(cursor->id.value);
            if (cleanup.kind != CleanupKind::Value)
                error({}, "guarded cleanup " + std::to_string(index) +
                          " is not a value cleanup");
            if (local && rootType && rootType->kind == TypeKind::Array) {
                auto& state = guardedArrays[local->id.value];
                if (state.nextUnread.empty()) {
                    state.nextUnread = cleanup.guard->nextUnread;
                    state.length = rootType->arrayLength;
                } else if (state.nextUnread !=
                           cleanup.guard->nextUnread) {
                    error({}, "guarded array cleanup rows use different cursors");
                }
                if (!state.elements.insert(
                        cleanup.guard->elementIndex).second)
                    error({}, "guarded array cleanup repeats element " +
                              std::to_string(
                                  cleanup.guard->elementIndex));
            }
        } else if (local) {
            unguardedCleanupRoots.insert(local->id.value);
        }
        if (cleanup.kind == CleanupKind::Allocation) {
            if (!cleanup.place.projections.empty())
                error({}, "allocation cleanup " + std::to_string(index) +
                          " targets a projected place");
            if (cleanup.action !=
                luna::ownership::CleanupAction::Deallocate)
                error({}, "allocation cleanup " + std::to_string(index) +
                          " does not deallocate backing storage");
            if (local && local->kind == LocalKind::Allocation &&
                cleanup.place.root != local->id)
                error({}, "raw allocation cleanup " +
                          std::to_string(index) +
                          " disagrees with its allocation identity");
        } else {
            if (local && local->kind == LocalKind::Allocation)
                error({}, "raw allocation local " +
                          std::to_string(local->id.value) +
                          " carries a value cleanup");
            if (cleanupType &&
                !cleanupType->sysmeta.resource.cleanupRequired)
                error({}, "value cleanup " + std::to_string(index) +
                          " targets a type with no cleanup obligation");
            verifyCleanupAction(
                cleanup.action, cleanup.type, {},
                "value cleanup " + std::to_string(index), module);
        }
    }
    for (auto& [root, state] : guardedArrays) {
        if (unguardedCleanupRoots.count(root))
            error({}, "guarded array state also carries an unguarded cleanup");
        if (state.elements.size() != state.length)
            error({}, "guarded array cleanup does not cover every element");
        for (uint64_t element = 0; element < state.length; ++element)
            if (!state.elements.count(element)) {
                error({}, "guarded array cleanup omits element " +
                          std::to_string(element));
                break;
            }
    }

    std::vector<std::vector<BlockId>> successors;
    verifyControlFlowBlocks(
        graph, module, guardedCursorIds, successors);


    verifyOwnershipDataflow(graph, module, entry != nullptr);


    if (entry) {
        std::vector<bool> reachable(graph.blocks.size(), false);
        std::vector<BlockId> worklist{graph.entry};
        while (!worklist.empty()) {
            const BlockId current = worklist.back();
            worklist.pop_back();
            if (current.empty() || current.value >= reachable.size() ||
                reachable[current.value])
                continue;
            reachable[current.value] = true;
            for (const auto successor : successors[current.value])
                worklist.push_back(successor);
        }
        for (size_t index = 0; index < reachable.size(); ++index)
            if (!reachable[index]) {
                // Unreachable blocks can arise from context/fragment control
                // flow where a continuation's return terminates the path
                // before post-resume or invocation-exit blocks are
                // connected. These are dead code, not verification errors.
                // The project owner confirmed: avoid creating them at
                // construction time (the long-term goal), but do not reject
                // the module for having them.
            }
    }

    return mErrors.empty();
}

} // namespace moon
