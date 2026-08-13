#include "Verifier.h"
#include "../core/TypeLayout.h"

#include "../diagnostics/Diagnostic.h"
#include "../core/TypeRelations.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace moon {

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

namespace {

bool isGeneric(const FunctionDecl& function) {
    return !function.typeParams.empty() && !function.isTemplateInstance;
}

bool isIntegerMetadataType(TypeKind kind) {
    switch (kind) {
        case TypeKind::I8:
        case TypeKind::I16:
        case TypeKind::I32:
        case TypeKind::I64:
        case TypeKind::U8:
        case TypeKind::U16:
        case TypeKind::U32:
        case TypeKind::U64:
        case TypeKind::USize:
        case TypeKind::ISize:
            return true;
        default:
            return false;
    }
}

bool metadataConstantMatches(const ConstantValue& value, const TypeRecord* type) {
    if (!type) return false;
    if (std::holds_alternative<int64_t>(value))
        return isIntegerMetadataType(type->kind);
    if (std::holds_alternative<double>(value))
        return type->kind == TypeKind::F32 || type->kind == TypeKind::F64;
    if (std::holds_alternative<bool>(value))
        return type->kind == TypeKind::Bool;
    if (std::holds_alternative<std::string>(value))
        return type->kind == TypeKind::String || type->kind == TypeKind::CStr;
    return false;
}

luna::ownership::Usage frozenUsage(
    const Module& module, const TypeRef& reference) {
    const auto* type = module.findType(reference);
    return type ? type->sysmeta.resource.usage
                : luna::ownership::Usage::Copy;
}

bool validIdentifier(const std::string& value) {
    if (value.empty() || (!std::isalpha(static_cast<unsigned char>(value[0])) &&
                          value[0] != '_'))
        return false;
    return std::all_of(value.begin() + 1, value.end(), [](unsigned char character) {
        return std::isalnum(character) || character == '_';
    });
}

bool validSeparatedName(const std::string& value, const std::string& separator,
                        bool emptyAllowed = false) {
    if (value.empty()) return emptyAllowed;
    size_t begin = 0;
    for (;;) {
        const size_t end = value.find(separator, begin);
        const std::string component = value.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!validIdentifier(component)) return false;
        if (end == std::string::npos) return true;
        begin = end + separator.size();
    }
}

} // namespace

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

    std::vector<uint32_t> blockOwners(graph.blocks.size(), 0);
    for (const auto& region : graph.regions) {
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
                 !luna::ownership::satisfiesUsageRequirement(
                     local.usage, type->sysmeta.resource.usage))
            error({}, "local " + std::to_string(index) +
                      " weakens its frozen usage requirement");
        if (local.kind == LocalKind::Allocation &&
            (local.usage != luna::ownership::Usage::Affine ||
             local.relation != luna::ownership::Relation::Owned))
            error({}, "raw allocation local " + std::to_string(index) +
                      " is not an owned affine identity");
    }
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
            if (local->scope != cleanup.scope)
                error({}, "cleanup " + std::to_string(index) +
                          " targets a local owned by another scope");
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

    const auto verifyEdge = [this, &graph](
        const BasicBlock& source, const ControlEdge& edge,
        const std::string& context) {
        const auto* target = graph.findBlock(edge.target);
        if (!target) {
            error(source.location, context + " references a missing target block");
            return;
        }
        std::unordered_set<uint32_t> seen;
        for (const auto cleanup : edge.cleanups) {
            if (!graph.findCleanup(cleanup))
                error(source.location, context +
                      " references a missing cleanup row");
            else if (!seen.insert(cleanup.value).second)
                error(source.location, context +
                      " repeats a cleanup row");
        }
    };

    const auto localVisibleFrom = [&graph](
        ScopeId localScope, ScopeId useScope) {
        std::unordered_set<uint32_t> visited;
        for (const ScopeRecord* scope = graph.findScope(useScope); scope;
             scope = graph.findScope(scope->parent)) {
            if (!visited.insert(scope->id.value).second) break;
            if (scope->id == localScope) return true;
        }
        return false;
    };
    const auto scanGraphIdentifier =
        [this, &graph, &localVisibleFrom](const IdentifierExpr& identifier,
                                         const BasicBlock& block,
                                         bool allowSyntheticTransfer) {
            if (!identifier.local.empty()) {
                const auto* local = graph.findLocal(identifier.local);
                if (!local) {
                    error(identifier.location,
                          "identifier references a missing LocalId");
                    return;
                }
                if (!identifier.declaration.empty())
                    error(identifier.location,
                          "identifier carries both LocalId and DeclarationRef");
                if (identifier.type != local->type)
                    error(identifier.location,
                          "identifier type disagrees with its canonical local");
                if (!localVisibleFrom(local->scope, block.scope))
                    error(identifier.location,
                          "identifier references a local outside its lexical scope");
                if (local->kind == LocalKind::Synthetic &&
                    luna::ownership::isMoveOnly(local->usage) &&
                    !allowSyntheticTransfer)
                    error(identifier.location,
                          "synthetic affine local is read without transfer");
            } else if (identifier.declaration.empty()) {
                error(identifier.location,
                      "identifier has no canonical LocalId or DeclarationRef");
            }
        };
    std::function<void(const Expr*, const BasicBlock&)> scanGraphExpr;
    scanGraphExpr = [this, &graph, &module, &localVisibleFrom,
                     &scanGraphIdentifier, &scanGraphExpr](
        const Expr* expression, const BasicBlock& block) {
        if (!expression) return;
        if (const auto* identifier =
                dynamic_cast<const IdentifierExpr*>(expression)) {
            scanGraphIdentifier(*identifier, block, false);
            return;
        }
        if (const auto* binary = dynamic_cast<const BinaryExpr*>(expression)) {
            if (binary->op == Operator::LogicalAnd ||
                binary->op == Operator::LogicalOr)
                error(binary->location,
                      "sealed CFG contains a short-circuit expression");
            scanGraphExpr(binary->lhs.get(), block);
            scanGraphExpr(binary->rhs.get(), block);
        } else if (const auto* unary =
                       dynamic_cast<const UnaryExpr*>(expression)) {
            scanGraphExpr(unary->operand.get(), block);
        } else if (const auto* call =
                       dynamic_cast<const CallExpr*>(expression)) {
            if (call->iteratorOp != IteratorOp::None)
                error(call->location,
                      "sealed CFG contains an unexpanded iterator recipe");
            const TypeRecord* signature = nullptr;
            const auto* callee = dynamic_cast<const IdentifierExpr*>(
                call->callee.get());
            const bool hasFrozenCallee =
                (callee && !callee->local.empty()) ||
                !call->calleeRef.empty();
            if (callee && !callee->local.empty()) {
                signature = module.findType(callee->type);
            } else if (!call->calleeRef.empty()) {
                const auto* declaration = module.findDeclaration(
                    call->calleeRef);
                signature = declaration
                    ? module.findType(declaration->type) : nullptr;
            }
            if (hasFrozenCallee &&
                (!signature || signature->kind != TypeKind::Function)) {
                error(call->location,
                      "call target has no frozen function signature");
            } else if (signature) {
                if (signature->parameterTypeIds.size() !=
                    call->args.size())
                    error(call->location,
                          "call argument arity disagrees with its signature");
                const size_t comparable = std::min(
                    signature->parameterTypeIds.size(), call->args.size());
                for (size_t index = 0; index < comparable; ++index)
                    if (!call->args[index] ||
                        call->args[index]->type !=
                            signature->parameterTypeIds[index])
                        error(call->location,
                              "call argument type disagrees with its signature");
                    else if (const auto* borrow =
                                 dynamic_cast<const BorrowExpr*>(
                                     call->args[index].get());
                             borrow &&
                             (index >= signature->parameterContracts.size() ||
                              signature->parameterContracts[index].relation !=
                                  (borrow->isMutable
                                       ? luna::ownership::Relation::MutableBorrow
                                       : luna::ownership::Relation::SharedBorrow)))
                        error(call->location,
                              "call borrow argument disagrees with its signature contract");
                if (call->type != signature->returnTypeId)
                    error(call->location,
                          "call result type disagrees with its signature");
                if (call->returnUsage !=
                        signature->returnContract.usage ||
                    call->returnsLinear !=
                        (signature->returnContract.usage ==
                         luna::ownership::Usage::Linear))
                    error(call->location,
                          "call result contract disagrees with its signature");
            }
            scanGraphExpr(call->callee.get(), block);
            for (const auto& argument : call->args)
                scanGraphExpr(argument.get(), block);
        } else if (const auto* selection =
                       dynamic_cast<const DynamicSelectExpr*>(expression)) {
            for (const auto& argument : selection->filterArguments)
                scanGraphExpr(argument.get(), block);
        } else if (const auto* launch =
                       dynamic_cast<const LaunchExpr*>(expression)) {
            scanGraphExpr(launch->threads.get(), block);
            for (const auto& argument : launch->args)
                scanGraphExpr(argument.get(), block);
        } else if (const auto* variant =
                       dynamic_cast<const VariantConstructExpr*>(expression)) {
            for (const auto& argument : variant->args)
                scanGraphExpr(argument.get(), block);
        } else if (const auto* result =
                       dynamic_cast<const ResultConstructExpr*>(expression)) {
            scanGraphExpr(result->payload.get(), block);
        } else if (const auto* field =
                       dynamic_cast<const FieldAccessExpr*>(expression)) {
            scanGraphExpr(field->object.get(), block);
        } else if (const auto* index =
                       dynamic_cast<const IndexExpr*>(expression)) {
            scanGraphExpr(index->object.get(), block);
            scanGraphExpr(index->index.get(), block);
        } else if (const auto* length =
                       dynamic_cast<const SliceLengthExpr*>(expression)) {
            scanGraphExpr(length->slice.get(), block);
        } else if (const auto* array =
                       dynamic_cast<const ArrayLiteralExpr*>(expression)) {
            for (const auto& element : array->elements)
                scanGraphExpr(element.get(), block);
        } else if (const auto* record =
                       dynamic_cast<const RecordLiteralExpr*>(expression)) {
            const auto* recordType = module.findType(record->type);
            if (recordType && recordType->kind == TypeKind::Struct)
                error(record->location,
                      "sealed CFG retains implicit named-struct allocation");
            for (const auto& field : record->fields)
                scanGraphExpr(field.value.get(), block);
        } else if (const auto* initialized =
                       dynamic_cast<const InitAllocationExpr*>(expression)) {
            const auto* allocation = graph.findLocal(
                initialized->allocation);
            if (!allocation) {
                error(initialized->location,
                      "allocation initialization references a missing LocalId");
            } else if (allocation->kind != LocalKind::Allocation ||
                       allocation->type != initialized->allocatedType ||
                       !localVisibleFrom(allocation->scope, block.scope)) {
                error(initialized->location,
                      "allocation initialization disagrees with its raw allocation local");
            }
            for (const auto& element : initialized->elements)
                scanGraphExpr(element.value.get(), block);
        } else if (const auto* allocation =
                       dynamic_cast<const HeapAllocExpr*>(expression)) {
            error(allocation->location,
                  "sealed CFG retains implicit heap allocation");
        } else if (const auto* lambda =
                       dynamic_cast<const LambdaExpr*>(expression)) {
            if (lambda->body || !lambda->controlFlow)
                error(lambda->location,
                      "sealed CFG lambda does not exclusively own a canonical CFG body");
            return;
        } else if (dynamic_cast<const TryExpr*>(expression) ||
                   dynamic_cast<const BlockExpr*>(expression) ||
                   dynamic_cast<const IfExpr*>(expression)) {
            error(expression->location,
                  "sealed CFG contains a nested control-flow expression");
        } else if (const auto* move =
                       dynamic_cast<const MoveExpr*>(expression)) {
            if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(
                    move->operand.get()))
                scanGraphIdentifier(*identifier, block, true);
            else
                scanGraphExpr(move->operand.get(), block);
        } else if (const auto* borrow =
                       dynamic_cast<const BorrowExpr*>(expression)) {
            if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(
                    borrow->operand.get()))
                scanGraphIdentifier(*identifier, block, true);
            else
                scanGraphExpr(borrow->operand.get(), block);
        } else if (const auto* dereference =
                       dynamic_cast<const DerefExpr*>(expression)) {
            scanGraphExpr(dereference->operand.get(), block);
        } else if (const auto* address =
                       dynamic_cast<const AddrOfExpr*>(expression)) {
            scanGraphExpr(address->operand.get(), block);
        } else if (const auto* assignment =
                       dynamic_cast<const AssignExpr*>(expression)) {
            // A synthetic move-only local may be used as the destination of
            // a transfer assignment without reading its old value. The
            // ownership dataflow below still requires the RHS to consume the
            // active value before this destination can be reinitialized.
            if (const auto* destination =
                    dynamic_cast<const IdentifierExpr*>(
                        assignment->lhs.get());
                assignment->op == Operator::Assign && destination &&
                !destination->local.empty()) {
                const auto* local = graph.findLocal(destination->local);
                scanGraphIdentifier(
                    *destination, block,
                    local && local->kind == LocalKind::Synthetic &&
                        luna::ownership::isMoveOnly(local->usage));
            } else {
                scanGraphExpr(assignment->lhs.get(), block);
            }
            scanGraphExpr(assignment->rhs.get(), block);
        }
    };
    const auto verifyGraphExpr = [this, &module, &scanGraphExpr](
        const Expr* expression, const BasicBlock& block,
        const std::string& context) {
        verifyExpr(expression, module, context);
        scanGraphExpr(expression, block);
    };

    std::vector<uint32_t> localDefinitions(graph.locals.size(), 0);
    std::vector<std::vector<BlockId>> successors(graph.blocks.size());
    for (const auto& block : graph.blocks) {
        const auto* scope = graph.findScope(block.scope);
        if (!graph.findRegion(block.region) || !scope)
            error(block.location, "block references a missing region or scope");
        else if (scope->region != block.region)
            error(block.location, "block scope belongs to another region");
        for (const auto& operation : block.operations) {
            if (!operation) {
                error(block.location, "CFG block contains a null operation");
                continue;
            }
            if (const auto* declaration =
                    dynamic_cast<const LetStmt*>(operation.get())) {
                const auto* local = graph.findLocal(declaration->local);
                if (!local) {
                    error(declaration->location,
                          "let operation has no canonical LocalId");
                } else {
                    ++localDefinitions[local->id.value];
                    if ((local->kind != LocalKind::Binding &&
                         local->kind != LocalKind::Synthetic) ||
                        local->scope != block.scope ||
                        local->name != declaration->name ||
                        local->type != declaration->type ||
                        local->usage != declaration->usage)
                        error(declaration->location,
                              "let operation disagrees with its local-table row");
                    if (!declaration->initializer ||
                        declaration->initializer->type != declaration->type)
                        error(declaration->location,
                              "let initializer type disagrees with its canonical local");
                }
                if (declaration->materializesIteratorRecipe ||
                    declaration->materializedIteratorOwnsSource ||
                    !declaration->materializedIteratorSourceType.empty())
                    error(declaration->location,
                          "sealed CFG retains materialized iterator recipe metadata");
                verifyStmt(operation.get(), module, "CFG operation");
                scanGraphExpr(declaration->initializer.get(), block);
                if (dynamic_cast<const InitAllocationExpr*>(
                        declaration->initializer.get())) {
                    const auto* type = module.findType(declaration->type);
                    const CleanupKind expectedKind =
                        type && type->sysmeta.resource.cleanupRequired
                        ? CleanupKind::Value
                        : CleanupKind::Allocation;
                    size_t cleanupCount = 0;
                    for (const auto& cleanup : graph.cleanups)
                        if (cleanup.place.root == declaration->local &&
                            cleanup.kind == expectedKind)
                            ++cleanupCount;
                    if (cleanupCount != 1)
                        error(declaration->location,
                              "initialized allocation binding does not own exactly one final cleanup");
                }
            } else if (const auto* allocation =
                           dynamic_cast<const AllocateStmt*>(operation.get())) {
                const auto* local = graph.findLocal(allocation->local);
                if (!local) {
                    error(allocation->location,
                          "allocate operation has no canonical LocalId");
                } else {
                    ++localDefinitions[local->id.value];
                    if (local->kind != LocalKind::Allocation ||
                        local->scope != block.scope ||
                        local->type != allocation->allocatedType ||
                        local->usage != luna::ownership::Usage::Affine ||
                        local->relation !=
                            luna::ownership::Relation::Owned)
                        error(allocation->location,
                              "allocate operation disagrees with its local-table row");
                }
                verifyType(allocation->allocatedType,
                           allocation->location,
                           "raw allocation", module);
                if (allocation->storage != HeapStorageKind::Unique)
                    error(allocation->location,
                          "canonical allocation uses unsupported storage");
                size_t cleanupCount = 0;
                for (const auto& cleanup : graph.cleanups)
                    if (cleanup.place.root == allocation->local &&
                        cleanup.kind == CleanupKind::Allocation)
                        ++cleanupCount;
                if (cleanupCount != 1)
                    error(allocation->location,
                          "raw allocation does not own exactly one backing-storage cleanup");
            } else if (const auto* expression =
                           dynamic_cast<const ExprStmt*>(operation.get())) {
                verifyStmt(operation.get(), module, "CFG operation");
                scanGraphExpr(expression->expr.get(), block);
            } else if (const auto* release =
                           dynamic_cast<const FreeStmt*>(operation.get())) {
                if (release->isImplicit)
                    error(release->location,
                          "implicit lexical cleanup remains a CFG operation");
                verifyStmt(operation.get(), module, "CFG operation");
                scanGraphExpr(release->operand.get(), block);
            } else if (const auto* await =
                           dynamic_cast<const AwaitStmt*>(operation.get())) {
                verifyStmt(operation.get(), module, "CFG operation");
                scanGraphExpr(await->event.get(), block);
            } else {
                error(operation->location,
                      "sealed CFG contains a structured control operation");
            }
        }
        const auto appendSuccessor = [&](const ControlEdge& edge) {
            if (!edge.target.empty()) successors[block.id.value].push_back(edge.target);
        };
        const auto rejectOperand = [&]() {
            if (block.terminator.operand)
                error(block.terminator.location,
                      "terminator unexpectedly carries an operand");
        };
        const auto rejectSecondaryCasesAndExit = [&]() {
            if (!block.terminator.secondary.target.empty() ||
                !block.terminator.secondary.cleanups.empty() ||
                !block.terminator.cases.empty() ||
                !block.terminator.exitCleanups.empty())
                error(block.terminator.location,
                      "terminator carries fields outside its canonical shape");
        };
        switch (block.terminator.kind) {
            case TerminatorKind::Invalid:
                error(block.terminator.location, "block has no terminator");
                break;
            case TerminatorKind::Jump:
                rejectOperand();
                if (!block.terminator.switchType.empty())
                    error(block.terminator.location,
                          "jump terminator carries a switch type");
                verifyEdge(block, block.terminator.primary, "jump edge");
                appendSuccessor(block.terminator.primary);
                rejectSecondaryCasesAndExit();
                break;
            case TerminatorKind::Branch:
                if (!block.terminator.operand)
                    error(block.terminator.location,
                          "branch terminator has no condition");
                else
                    verifyGraphExpr(
                        block.terminator.operand.get(), block, "CFG branch");
                if (block.terminator.operand) {
                    const auto* conditionType = module.findType(
                        block.terminator.operand->type);
                    if (!conditionType || conditionType->kind != TypeKind::Bool)
                        error(block.terminator.location,
                              "branch condition is not bool");
                }
                if (!block.terminator.switchType.empty())
                    error(block.terminator.location,
                          "branch terminator carries a switch type");
                verifyEdge(block, block.terminator.primary, "true edge");
                verifyEdge(block, block.terminator.secondary, "false edge");
                appendSuccessor(block.terminator.primary);
                appendSuccessor(block.terminator.secondary);
                if (!block.terminator.cases.empty() ||
                    !block.terminator.exitCleanups.empty())
                    error(block.terminator.location,
                          "branch terminator carries non-branch fields");
                break;
            case TerminatorKind::Switch: {
                if (!block.terminator.operand)
                    error(block.terminator.location,
                          "switch terminator has no scrutinee");
                else
                    verifyGraphExpr(
                        block.terminator.operand.get(), block, "CFG switch");
                verifyType(block.terminator.switchType,
                           block.terminator.location,
                           "CFG switch type witness", module);
                const auto* switchType = module.findType(
                    block.terminator.switchType);
                if (!switchType ||
                    (switchType->kind != TypeKind::Enum &&
                     switchType->kind != TypeKind::Result))
                    error(block.terminator.location,
                          "CFG switch has no enum or Result type witness");
                if (block.terminator.operand &&
                    block.terminator.operand->type !=
                        block.terminator.switchType)
                    error(block.terminator.location,
                          "CFG switch operand disagrees with its type witness");
                if (block.terminator.cases.empty())
                    error(block.terminator.location,
                          "CFG switch has no cases");
                verifyEdge(block, block.terminator.primary, "switch default edge");
                appendSuccessor(block.terminator.primary);
                std::unordered_set<uint32_t> tags;
                for (const auto& item : block.terminator.cases) {
                    if (!tags.insert(item.tag).second)
                        error(block.terminator.location,
                              "switch terminator repeats a case tag");
                    verifyEdge(block, item.edge, "switch case edge");
                    appendSuccessor(item.edge);
                    const auto* target = graph.findBlock(item.edge.target);
                    std::vector<TypeRef> expectedBindings;
                    if (switchType && switchType->kind == TypeKind::Enum &&
                        item.tag < switchType->variants.size()) {
                        expectedBindings = switchType->variants[item.tag].fields;
                    } else if (switchType &&
                               switchType->kind == TypeKind::Result &&
                               switchType->typeArgumentIds.size() == 2 &&
                               item.tag < 2) {
                        expectedBindings.push_back(
                            switchType->typeArgumentIds[item.tag == 1 ? 0 : 1]);
                    } else if (switchType) {
                        error(block.terminator.location,
                              "switch case tag is outside its frozen type");
                    }
                    if (item.bindings.size() != expectedBindings.size())
                        error(block.terminator.location,
                              "switch case binding arity disagrees with its frozen variant");
                    const size_t comparable = std::min(
                        item.bindings.size(), expectedBindings.size());
                    for (size_t index = 0; index < comparable; ++index) {
                        const auto* local = graph.findLocal(item.bindings[index]);
                        if (!local) {
                            error(block.terminator.location,
                                  "switch case references a missing pattern LocalId");
                            continue;
                        }
                        ++localDefinitions[local->id.value];
                        if (!target || local->kind != LocalKind::Pattern ||
                            local->scope != target->scope ||
                            local->type != expectedBindings[index])
                            error(block.terminator.location,
                                  "switch case pattern local disagrees with its target scope or type");
                    }
                }
                if (!block.terminator.secondary.target.empty() ||
                    !block.terminator.secondary.cleanups.empty() ||
                    !block.terminator.exitCleanups.empty())
                    error(block.terminator.location,
                          "switch terminator carries non-switch fields");
                break;
            }
            case TerminatorKind::Return:
                if (block.terminator.operand)
                    verifyGraphExpr(
                        block.terminator.operand.get(), block, "CFG return");
                if (!block.terminator.primary.target.empty() ||
                    !block.terminator.primary.cleanups.empty() ||
                    !block.terminator.secondary.target.empty() ||
                    !block.terminator.secondary.cleanups.empty() ||
                    !block.terminator.cases.empty())
                    error(block.terminator.location,
                          "return terminator carries successor fields");
                if (!block.terminator.switchType.empty())
                    error(block.terminator.location,
                          "return terminator carries a switch type");
                break;
            case TerminatorKind::Resume:
            case TerminatorKind::Abort:
                rejectOperand();
                if (!block.terminator.switchType.empty())
                    error(block.terminator.location,
                          "resume/abort terminator carries a switch type");
                verifyEdge(
                    block, block.terminator.primary,
                    block.terminator.kind == TerminatorKind::Resume
                        ? "resume edge" : "abort edge");
                appendSuccessor(block.terminator.primary);
                rejectSecondaryCasesAndExit();
                break;
            case TerminatorKind::Unreachable:
                rejectOperand();
                if (!block.terminator.switchType.empty())
                    error(block.terminator.location,
                          "unreachable terminator carries a switch type");
                if (!block.terminator.primary.target.empty() ||
                    !block.terminator.primary.cleanups.empty())
                    error(block.terminator.location,
                          "unreachable terminator carries a successor");
                rejectSecondaryCasesAndExit();
                break;
        }
    }

    for (size_t index = 0; index < graph.locals.size(); ++index) {
        const auto kind = graph.locals[index].kind;
        const uint32_t expected =
            (kind == LocalKind::Binding || kind == LocalKind::Pattern ||
             kind == LocalKind::Synthetic ||
             kind == LocalKind::Allocation)
                ? 1u : 0u;
        if (localDefinitions[index] != expected)
            error({}, "local " + std::to_string(index) + " has " +
                      std::to_string(localDefinitions[index]) +
                      " definitions; expected " + std::to_string(expected));
    }

    // Canonical ownership state is path-sensitive. Scope tables enumerate
    // possible cleanup obligations; synthetic move-only locals append
    // compile-time-only marker bits to the same dataflow, without adding a
    // runtime token. The state reconstructs which places remain active after
    // initialization, move, explicit free, and return transfer.
    using CleanupState = std::vector<uint8_t>;
    std::vector<std::vector<CleanupId>> cleanupsByLocal(graph.locals.size());
    for (const auto& cleanup : graph.cleanups) {
        if (!cleanup.place.root.empty() &&
            cleanup.place.root.value < cleanupsByLocal.size())
            cleanupsByLocal[cleanup.place.root.value].push_back(cleanup.id);
    }
    const size_t noMarker = graph.cleanups.size() + graph.locals.size();
    std::vector<size_t> markerStateByLocal(graph.locals.size(), noMarker);
    size_t stateSize = graph.cleanups.size();
    for (const auto& local : graph.locals)
        if (local.kind == LocalKind::Synthetic &&
            luna::ownership::isMoveOnly(local.usage))
            markerStateByLocal[local.id.value] = stateSize++;
    const auto activateLocal = [this, &graph, &cleanupsByLocal,
                                &markerStateByLocal, noMarker](
        LocalId local, CleanupState& state, const SourceLocation& location,
        const std::string& context) {
        if (local.empty() || local.value >= cleanupsByLocal.size()) return;
        for (const auto cleanup : cleanupsByLocal[local.value]) {
            if (cleanup.value >= state.size()) continue;
            if (state[cleanup.value])
                error(location, context + " reinitializes active local '" +
                      graph.locals[local.value].name + "'");
            state[cleanup.value] = 1;
        }
        const size_t marker = markerStateByLocal[local.value];
        if (marker != noMarker && marker < state.size()) {
            if (state[marker])
                error(location, context + " reinitializes active local '" +
                      graph.locals[local.value].name + "'");
            state[marker] = 1;
        }
    };
    std::function<std::optional<PlaceRef>(const Expr*)> placeOf;
    placeOf = [&module, &placeOf](
        const Expr* expression) -> std::optional<PlaceRef> {
        if (!expression) return std::nullopt;
        if (const auto* identifier =
                dynamic_cast<const IdentifierExpr*>(expression)) {
            if (identifier->local.empty()) return std::nullopt;
            return PlaceRef{identifier->local, {}};
        }
        if (const auto* field =
                dynamic_cast<const FieldAccessExpr*>(expression)) {
            auto place = placeOf(field->object.get());
            const auto* objectType = field->object
                ? module.findType(field->object->type) : nullptr;
            if (!place || !objectType) return std::nullopt;
            for (size_t index = 0; index < objectType->fields.size(); ++index) {
                if (objectType->fields[index].name == field->field) {
                    place->projections.push_back({
                        ProjectionKind::Field,
                        static_cast<uint64_t>(index), {}});
                    return place;
                }
            }
            return std::nullopt;
        }
        if (const auto* index = dynamic_cast<const IndexExpr*>(expression)) {
            auto place = placeOf(index->object.get());
            if (!place) return std::nullopt;
            if (const auto* constant =
                    dynamic_cast<const IntLiteralExpr*>(index->index.get());
                constant && constant->value >= 0) {
                place->projections.push_back({
                    ProjectionKind::ConstantIndex,
                    static_cast<uint64_t>(constant->value), {}});
                return place;
            }
            if (const auto* dynamic =
                    dynamic_cast<const IdentifierExpr*>(index->index.get());
                dynamic && !dynamic->local.empty()) {
                place->projections.push_back({
                    ProjectionKind::DynamicIndex, 0, dynamic->local});
                return place;
            }
            return std::nullopt;
        }
        if (const auto* dereference =
                dynamic_cast<const DerefExpr*>(expression)) {
            auto place = placeOf(dereference->operand.get());
            if (place)
                place->projections.push_back({
                    ProjectionKind::Dereference, 0, {}});
            return place;
        }
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(expression);
            unary && unary->op == Operator::Dereference) {
            auto place = placeOf(unary->operand.get());
            if (place)
                place->projections.push_back({
                    ProjectionKind::Dereference, 0, {}});
            return place;
        }
        return std::nullopt;
    };
    const auto projectionPrefix = [](const PlaceRef& prefix,
                                     const PlaceRef& value) {
        return prefix.root == value.root &&
            prefix.projections.size() <= value.projections.size() &&
            std::equal(prefix.projections.begin(), prefix.projections.end(),
                       value.projections.begin());
    };
    const auto consumePlace = [this, &graph, &cleanupsByLocal,
                               &markerStateByLocal, noMarker,
                               &projectionPrefix](
        const PlaceRef& place, CleanupState& state,
        const SourceLocation& location, const std::string& context) {
        if (place.root.empty() || place.root.value >= cleanupsByLocal.size())
            return;
        bool consumed = false;
        const size_t marker = markerStateByLocal[place.root.value];
        if (marker != noMarker && place.projections.empty()) {
            if (marker >= state.size() || !state[marker])
                error(location, context +
                      " consumes inactive synthetic affine local '" +
                      graph.locals[place.root.value].name + "'");
            else {
                state[marker] = 0;
                consumed = true;
            }
        }
        for (const auto cleanupId : cleanupsByLocal[place.root.value]) {
            if (cleanupId.value >= state.size() || !state[cleanupId.value])
                continue;
            const auto& cleanup = graph.cleanups[cleanupId.value];
            if (place.projections.empty() ||
                projectionPrefix(place, cleanup.place)) {
                state[cleanupId.value] = 0;
                consumed = true;
            } else if (cleanup.place.projections.empty()) {
                error(location, context + " partially consumes local '" +
                      graph.locals[place.root.value].name +
                      "' without projected cleanup rows");
            }
        }
        if (!consumed && !cleanupsByLocal[place.root.value].empty())
            error(location, context + " consumes an inactive place rooted at '" +
                  graph.locals[place.root.value].name + "'");
    };
    std::function<void(const Expr*, CleanupState&)> transferExpr;
    transferExpr = [&transferExpr, &placeOf, &consumePlace,
                    &activateLocal, &graph](
        const Expr* expression, CleanupState& state) {
        if (!expression) return;
        if (const auto* move = dynamic_cast<const MoveExpr*>(expression)) {
            transferExpr(move->operand.get(), state);
            if (auto place = placeOf(move->operand.get()))
                consumePlace(*place, state, move->location, "move");
            return;
        }
        if (const auto* binary = dynamic_cast<const BinaryExpr*>(expression)) {
            transferExpr(binary->lhs.get(), state);
            transferExpr(binary->rhs.get(), state);
        } else if (const auto* unary =
                       dynamic_cast<const UnaryExpr*>(expression)) {
            transferExpr(unary->operand.get(), state);
        } else if (const auto* call =
                       dynamic_cast<const CallExpr*>(expression)) {
            transferExpr(call->callee.get(), state);
            for (const auto& argument : call->args)
                transferExpr(argument.get(), state);
        } else if (const auto* selection =
                       dynamic_cast<const DynamicSelectExpr*>(expression)) {
            for (const auto& argument : selection->filterArguments)
                transferExpr(argument.get(), state);
        } else if (const auto* launch =
                       dynamic_cast<const LaunchExpr*>(expression)) {
            transferExpr(launch->threads.get(), state);
            for (const auto& argument : launch->args)
                transferExpr(argument.get(), state);
        } else if (const auto* variant =
                       dynamic_cast<const VariantConstructExpr*>(expression)) {
            for (const auto& argument : variant->args)
                transferExpr(argument.get(), state);
        } else if (const auto* result =
                       dynamic_cast<const ResultConstructExpr*>(expression)) {
            transferExpr(result->payload.get(), state);
        } else if (const auto* field =
                       dynamic_cast<const FieldAccessExpr*>(expression)) {
            transferExpr(field->object.get(), state);
        } else if (const auto* index =
                       dynamic_cast<const IndexExpr*>(expression)) {
            transferExpr(index->object.get(), state);
            transferExpr(index->index.get(), state);
        } else if (const auto* length =
                       dynamic_cast<const SliceLengthExpr*>(expression)) {
            transferExpr(length->slice.get(), state);
        } else if (const auto* array =
                       dynamic_cast<const ArrayLiteralExpr*>(expression)) {
            for (const auto& element : array->elements)
                transferExpr(element.get(), state);
        } else if (const auto* record =
                       dynamic_cast<const RecordLiteralExpr*>(expression)) {
            for (const auto& field : record->fields)
                transferExpr(field.value.get(), state);
        } else if (const auto* initialized =
                       dynamic_cast<const InitAllocationExpr*>(expression)) {
            for (const auto& element : initialized->elements)
                transferExpr(element.value.get(), state);
            consumePlace(
                PlaceRef{initialized->allocation, {}}, state,
                initialized->location, "allocation initialization");
        } else if (const auto* allocation =
                       dynamic_cast<const HeapAllocExpr*>(expression)) {
            transferExpr(allocation->initializer.get(), state);
        } else if (const auto* borrow =
                       dynamic_cast<const BorrowExpr*>(expression)) {
            transferExpr(borrow->operand.get(), state);
        } else if (const auto* dereference =
                       dynamic_cast<const DerefExpr*>(expression)) {
            transferExpr(dereference->operand.get(), state);
        } else if (const auto* address =
                       dynamic_cast<const AddrOfExpr*>(expression)) {
            transferExpr(address->operand.get(), state);
        } else if (const auto* assignment =
                       dynamic_cast<const AssignExpr*>(expression)) {
            if (assignment->op == Operator::Assign) {
                auto destination = placeOf(assignment->lhs.get());
                const auto* local = destination &&
                        destination->projections.empty()
                    ? graph.findLocal(destination->root) : nullptr;
                if (local && local->kind == LocalKind::Synthetic &&
                    luna::ownership::isMoveOnly(local->usage)) {
                    // The RHS is evaluated first by Luna assignment. It must
                    // consume the current value, otherwise activation reports
                    // an overwrite of active affine state. Successful return
                    // then reinitializes the same compile-time-tracked slot.
                    transferExpr(assignment->rhs.get(), state);
                    activateLocal(
                        destination->root, state, assignment->location,
                        "transfer assignment");
                    return;
                }
            }
            transferExpr(assignment->lhs.get(), state);
            transferExpr(assignment->rhs.get(), state);
        }
    };
    const auto expectedActiveCleanups = [&graph](
        const CleanupState& state, ScopeId source,
        std::optional<ScopeId> target) {
        std::unordered_set<uint32_t> targetAncestors;
        if (target) {
            for (const ScopeRecord* scope = graph.findScope(*target); scope;
                 scope = graph.findScope(scope->parent))
                targetAncestors.insert(scope->id.value);
        }
        std::vector<CleanupId> result;
        std::unordered_set<uint32_t> visited;
        for (const ScopeRecord* scope = graph.findScope(source); scope;
             scope = graph.findScope(scope->parent)) {
            if (!visited.insert(scope->id.value).second ||
                targetAncestors.count(scope->id.value))
                break;
            for (auto cleanup = scope->cleanups.rbegin();
                 cleanup != scope->cleanups.rend(); ++cleanup)
                if (cleanup->value < state.size() && state[cleanup->value])
                    result.push_back(*cleanup);
        }
        return result;
    };
    const auto applyCleanupEdge = [this,&expectedActiveCleanups](
        const BasicBlock& source, const std::vector<CleanupId>& actual,
        std::optional<ScopeId> target, CleanupState& state,
        const std::string& context) {
        const auto expected = expectedActiveCleanups(state, source.scope, target);
        if (actual != expected) {
            error(source.terminator.location,
                  context + " does not match active place cleanup state");
            return;
        }
        for (const auto cleanup : actual)
            if (cleanup.value < state.size()) state[cleanup.value] = 0;
    };

    if (entry) {
        std::vector<std::optional<CleanupState>> incoming(graph.blocks.size());
        CleanupState entryState(stateSize, 0);
        for (const auto& local : graph.locals) {
            if (local.kind == LocalKind::Parameter) {
                if (local.scope != graph.rootScope)
                    error({}, "parameter local is outside the CFG root scope");
                activateLocal(local.id, entryState, {}, "parameter entry");
            }
        }
        incoming[graph.entry.value] = entryState;
        std::vector<BlockId> worklist{graph.entry};
        const auto propagate = [this, &graph, &markerStateByLocal, noMarker,
                                &localVisibleFrom, &incoming, &worklist](
            BlockId target, CleanupState state,
            const SourceLocation& location) {
            const auto* targetBlock = graph.findBlock(target);
            if (!targetBlock || target.value >= incoming.size()) return;
            for (const auto& local : graph.locals) {
                const size_t marker = markerStateByLocal[local.id.value];
                if (marker != noMarker && marker < state.size() &&
                    !localVisibleFrom(local.scope, targetBlock->scope))
                    state[marker] = 0;
            }
            if (!incoming[target.value]) {
                incoming[target.value] = std::move(state);
                worklist.push_back(target);
            } else if (*incoming[target.value] != state) {
                error(location,
                      "CFG predecessors disagree on active ownership state");
            }
        };
        while (!worklist.empty()) {
            const BlockId blockId = worklist.back();
            worklist.pop_back();
            const auto& block = graph.blocks[blockId.value];
            CleanupState state = *incoming[blockId.value];
            for (const auto& operation : block.operations) {
                if (const auto* declaration =
                        dynamic_cast<const LetStmt*>(operation.get())) {
                    transferExpr(declaration->initializer.get(), state);
                    activateLocal(declaration->local, state,
                                  declaration->location, "let operation");
                } else if (const auto* allocation =
                               dynamic_cast<const AllocateStmt*>(
                                   operation.get())) {
                    activateLocal(allocation->local, state,
                                  allocation->location,
                                  "allocate operation");
                } else if (const auto* expression =
                               dynamic_cast<const ExprStmt*>(operation.get())) {
                    transferExpr(expression->expr.get(), state);
                } else if (const auto* release =
                               dynamic_cast<const FreeStmt*>(operation.get())) {
                    transferExpr(release->operand.get(), state);
                    if (auto place = placeOf(release->operand.get()))
                        consumePlace(*place, state, release->location,
                                     "explicit free");
                } else if (const auto* await =
                               dynamic_cast<const AwaitStmt*>(operation.get())) {
                    transferExpr(await->event.get(), state);
                    if (auto place = placeOf(await->event.get()))
                        consumePlace(*place, state, await->location, "await");
                }
            }

            const auto propagateEdge = [&](const ControlEdge& edge,
                                           CleanupState edgeState,
                                           const std::string& context) {
                const auto* target = graph.findBlock(edge.target);
                if (!target) return;
                applyCleanupEdge(block, edge.cleanups, target->scope,
                                 edgeState, context);
                propagate(edge.target, edgeState, block.terminator.location);
            };
            switch (block.terminator.kind) {
                case TerminatorKind::Jump:
                    propagateEdge(block.terminator.primary, state,
                                  "jump edge cleanup");
                    break;
                case TerminatorKind::Branch:
                    transferExpr(block.terminator.operand.get(), state);
                    propagateEdge(block.terminator.primary, state,
                                  "true edge cleanup");
                    propagateEdge(block.terminator.secondary, state,
                                  "false edge cleanup");
                    break;
                case TerminatorKind::Switch:
                    transferExpr(block.terminator.operand.get(), state);
                    propagateEdge(block.terminator.primary, state,
                                  "switch default cleanup");
                    for (const auto& item : block.terminator.cases) {
                        CleanupState caseState = state;
                        const auto* target = graph.findBlock(item.edge.target);
                        if (!target) continue;
                        applyCleanupEdge(block, item.edge.cleanups,
                                         target->scope, caseState,
                                         "switch case cleanup");
                        for (const auto local : item.bindings)
                            activateLocal(local, caseState,
                                          block.terminator.location,
                                          "match pattern");
                        propagate(item.edge.target, caseState,
                                  block.terminator.location);
                    }
                    break;
                case TerminatorKind::Return: {
                    transferExpr(block.terminator.operand.get(), state);
                    if (block.terminator.operand &&
                        !dynamic_cast<const MoveExpr*>(
                            block.terminator.operand.get())) {
                        if (auto place = placeOf(
                                block.terminator.operand.get())) {
                            const auto* local = graph.findLocal(place->root);
                            const auto* type = local
                                ? module.findType(local->type) : nullptr;
                            if (local &&
                                (luna::ownership::isMoveOnly(local->usage) ||
                                 (type && type->sysmeta.resource.cleanupRequired)))
                                consumePlace(*place, state,
                                             block.terminator.location,
                                             "return transfer");
                        }
                    }
                    applyCleanupEdge(block, block.terminator.exitCleanups,
                                     std::nullopt, state,
                                     "return cleanup");
                    break;
                }
                case TerminatorKind::Resume:
                    propagateEdge(block.terminator.primary, state,
                                  "resume edge cleanup");
                    break;
                case TerminatorKind::Abort:
                    propagateEdge(block.terminator.primary, state,
                                  "abort edge cleanup");
                    break;
                case TerminatorKind::Invalid:
                case TerminatorKind::Unreachable:
                    break;
            }
        }
    }

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
            if (!reachable[index])
                error(graph.blocks[index].location,
                      "sealed CFG contains unreachable block " +
                          std::to_string(index));
    }

    return mErrors.empty();
}

bool Verifier::verify(const Module& module) {
    mErrors.clear();
    mVerifiedTypeIds.clear();
    mActiveTypeIds.clear();
    if (module.formatMajor != FormatMajor) {
        error({}, "unsupported MoonIR major version " +
                  std::to_string(module.formatMajor));
    }
    if (!module.typeTableSealed)
        error({}, "MoonIR type table must be sealed before verification");
    if (!validSeparatedName(module.name, "."))
        error({}, "module has an invalid canonical Package ID '" + module.name + "'");
    std::unordered_set<std::string> sourceModules;
    for (const auto& sourceModule : module.sourceModules) {
        if (!validSeparatedName(sourceModule, "::", true))
            error({}, "invalid source module path '" + sourceModule + "'");
        if (!sourceModules.insert(sourceModule).second)
            error({}, "duplicate source module path '" + sourceModule + "'");
    }
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
        packageAliases;
    for (const auto& use : module.packageUses) {
        if (!validSeparatedName(use.ownerPackageId, ".") ||
            !validSeparatedName(use.packageId, ".") || !validIdentifier(use.alias)) {
            error({}, "invalid package using entry '" + use.packageId + "' as '" +
                      use.alias + "'");
            continue;
        }
        if (use.packageId == use.ownerPackageId)
            error({}, "package cannot use itself as '" + use.alias + "'");
        auto [found, inserted] = packageAliases[use.ownerPackageId].emplace(
            use.alias, use.packageId);
        if (!inserted && found->second != use.packageId)
            error({}, "package alias '" + use.alias + "' identifies multiple packages");
    }

    std::unordered_map<std::string, std::string> shapePayloads;
    std::unordered_map<std::string, std::string> abiLayoutPayloads;
    for (const auto& type : module.typeTable) {
        if (type.id.empty()) {
            error({}, "type table entry has no stable TypeId");
            continue;
        }
        if (!mVerifiedTypeIds.insert(type.id.value).second)
            error({}, "duplicate or colliding TypeId '" + type.id.value + "'");
        if (luna::types::typeIdFromCanonical(type.canonicalType) != type.id)
            error({}, "TypeId payload mismatch for '" + type.id.value + "'");
        if (luna::types::shapeIdFromCanonical(type.canonicalShape) != type.shapeId)
            error({}, "ShapeId payload mismatch for type '" + type.id.value + "'");
        auto [shape, insertedShape] = shapePayloads.emplace(
            type.shapeId.value, type.canonicalShape);
        if (!insertedShape && shape->second != type.canonicalShape)
            error({}, "colliding ShapeId '" + type.shapeId.value + "'");
        if (canonicalAbiLayout(type) != type.canonicalAbiLayout)
            error({}, "ABI layout payload mismatch for type '" + type.id.value + "'");
        if (luna::identity::abiLayoutIdFromCanonical(
                type.canonicalAbiLayout) != type.abiLayoutId)
            error({}, "AbiLayoutId payload mismatch for type '" +
                      type.id.value + "'");
        auto [layout, insertedLayout] = abiLayoutPayloads.emplace(
            type.abiLayoutId.value, type.canonicalAbiLayout);
        if (!insertedLayout && layout->second != type.canonicalAbiLayout)
            error({}, "colliding AbiLayoutId '" +
                      type.abiLayoutId.value + "'");
        if (type.sysmeta.identity.type != type.id ||
            type.sysmeta.identity.shape != type.shapeId ||
            type.sysmeta.identity.abiLayout != type.abiLayoutId)
            error({}, "type '" + type.id.value +
                      "' has inconsistent identity sysmeta");
        if (type.domain == luna::types::TypeDomain::Inference ||
            type.domain == luna::types::TypeDomain::Error ||
            type.identityMode == luna::types::IdentityMode::Inference ||
            type.identityMode == luna::types::IdentityMode::Error)
            error({}, "unresolved Sema type entered MoonIR type table as '" +
                      type.id.value + "' (" + type.displayName + ")");
        if ((type.identityMode == luna::types::IdentityMode::Nominal ||
             type.identityMode == luna::types::IdentityMode::MetaSchema) &&
            type.nominalDeclarationId.empty())
            error({}, "identity-bearing type '" + type.id.value +
                      "' has no declaration identity");
        if (type.identityMode == luna::types::IdentityMode::Structural &&
            !type.nominalDeclarationId.empty())
            error({}, "structural type '" + type.id.value +
                      "' unexpectedly carries a nominal identity");
        if (type.valueAlignment == 0 ||
            (type.valueAlignment & (type.valueAlignment - 1)) != 0)
            error({}, "type '" + type.id.value +
                      "' has an invalid ABI alignment");
        if (type.kind == TypeKind::Enum ||
            type.kind == TypeKind::Result) {
            if (type.layoutAbiVersion !=
                luna::layout::InlineAdtAbiVersion)
                error({}, "inline ADT '" + type.id.value +
                          "' uses an unsupported layout ABI");
            const std::string prefix =
                "luna.inline-adt.v" +
                std::to_string(luna::layout::InlineAdtAbiVersion) +
                ";tag_storage=" +
                std::to_string(luna::layout::InlineTagStorageSize) +
                ";payload_align=" +
                std::to_string(luna::layout::InlinePayloadAlignment) +
                ";size=" + std::to_string(type.valueSize);
            if (type.abiLayout.rfind(prefix, 0) != 0)
                error({}, "inline ADT '" + type.id.value +
                          "' has an inconsistent frozen ABI signature");
        } else if (type.layoutAbiVersion != 0 ||
                   !type.abiLayout.empty()) {
            error({}, "non-ADT type '" + type.id.value +
                      "' unexpectedly carries inline ADT ABI metadata");
        }
        if (type.kind == TypeKind::Metadata &&
            (type.domain != luna::types::TypeDomain::Meta ||
             type.identityMode != luna::types::IdentityMode::MetaSchema))
            error({}, "metadata type '" + type.id.value +
                      "' is outside the Meta type domain");
        if ((type.kind == TypeKind::MetadataView ||
             type.kind == TypeKind::DeclarationView ||
             type.kind == TypeKind::DeclarationRef) &&
            type.domain != luna::types::TypeDomain::Compiler)
            error({}, "compile-time view/reference type '" + type.id.value +
                      "' is outside the Compiler type domain");
        if (type.kind == TypeKind::Trait &&
            type.domain != luna::types::TypeDomain::Compiler)
            error({}, "trait type '" + type.id.value +
                      "' is outside the Compiler type domain");
        if (type.sysmeta.schemaMajor != luna::sysmeta::SchemaMajor)
            error({}, "type '" + type.id.value +
                      "' uses an unsupported sysmeta schema");
        if (type.kind == TypeKind::DeviceBuffer &&
            type.sysmeta.resource.releaseDomain !=
                luna::sysmeta::ReleaseDomain::Device)
            error({}, "device_buffer type '" + type.id.value +
                      "' has inconsistent release-domain sysmeta");
        if (type.kind == TypeKind::Result) {
            if (type.referencedTypeIds.size() != 2) {
                error({}, "Result type '" + type.id.value +
                          "' must reference exactly value and error payload types");
            }
        }
        if (!type.sysmeta.abi.dropGlueSymbol.empty())
            error({}, "type '" + type.id.value +
                      "' retains a linkage-string Drop reference");
        if (type.sysmeta.resource.needsDrop != type.dropGlue.complete())
            error({}, "type '" + type.id.value +
                      "' has inconsistent Drop sysmeta");
        if (!type.dropGlue.empty())
            verifyDeclarationRef(
                type.dropGlue, {}, "Drop glue for type '" +
                type.id.value + "'", module, DeclarationKind::Function);
        if (type.sysmeta.resource.cleanupRequired !=
            (type.sysmeta.resource.cleanup !=
             luna::ownership::CleanupAction::None))
            error({}, "type '" + type.id.value +
                      "' has inconsistent Resource cleanup sysmeta");
        if (type.sysmeta.resource.cleanupRequired &&
            type.sysmeta.resource.usage ==
                luna::ownership::Usage::Copy)
            error({}, "type '" + type.id.value +
                      "' has cleanup obligations but Copy usage");
        if (type.sysmeta.resource.recursiveCleanup &&
            !type.sysmeta.resource.cleanupRequired)
            error({}, "type '" + type.id.value +
                      "' has recursive cleanup without a cleanup obligation");
        if (type.kind == TypeKind::Reference &&
            (type.sysmeta.resource.lifetime !=
                 luna::sysmeta::ResourceLifetime::Borrowed ||
             type.sysmeta.resource.relation ==
                 luna::ownership::Relation::Owned))
            error({}, "reference type '" + type.id.value +
                      "' has inconsistent borrowed Resource lifetime");
        if (type.sysmeta.resource.lifetime ==
                luna::sysmeta::ResourceLifetime::Borrowed &&
            type.kind != TypeKind::Reference)
            error({}, "non-reference type '" + type.id.value +
                      "' claims a borrowed Resource lifetime");
        if (type.sysmeta.resource.cleanupRequired &&
            type.sysmeta.resource.lifetime ==
                luna::sysmeta::ResourceLifetime::Value)
            error({}, "type '" + type.id.value +
                      "' has cleanup obligations but value lifetime");
    }
    for (const auto& type : module.typeTable) {
        std::vector<TypeRef> structuralReferences;
        const auto appendReference = [&](const TypeRef& reference) {
            if (!reference.empty()) structuralReferences.push_back(reference);
        };
        appendReference(type.innerTypeId);
        appendReference(type.returnTypeId);
        for (const auto& reference : type.typeArgumentIds)
            appendReference(reference);
        for (const auto& reference : type.parameterTypeIds)
            appendReference(reference);
        for (const auto& field : type.fields)
            appendReference(field.type);
        for (const auto& variant : type.variants)
            for (const auto& field : variant.fields)
                appendReference(field);
        if (structuralReferences != type.referencedTypeIds)
            error({}, "frozen type '" + type.id.value +
                      "' has a reference index inconsistent with its payload");
        for (const auto& referenced : type.referencedTypeIds) {
            if (!module.findType(referenced))
                error({}, "frozen type '" + type.id.value +
                      "' references missing type '" + referenced.value + "'");
        }
    }

    // Reconstruct from frozen records only, then recompute every identity.
    // This proves the verifier does not rely on the frontend Type objects that
    // happened to exist while the module was built.
    TypeMaterializer materializer(module);
    for (const auto& type : module.typeTable) {
        const TypePtr restored = materializer.materialize(type.id);
        if (!restored) {
            error({}, "cannot materialize frozen type '" + type.id.value + "'");
            continue;
        }
        if (luna::types::canonicalType(restored) != type.canonicalType ||
            luna::types::typeId(restored) != type.id)
            error({}, "frozen payload does not reproduce TypeId '" +
                      type.id.value + "'");
        const auto restoredShape = luna::types::canonicalShape(restored);
        if (restoredShape != type.canonicalShape ||
            luna::types::shapeId(restored) != type.shapeId)
            error({}, "frozen payload does not reproduce ShapeId for type '" +
                      type.id.value + "' (" + type.displayName +
                      ")");
        if (luna::layout::valueSize(restored) != type.valueSize ||
            luna::layout::valueAlignment(restored) != type.valueAlignment)
            error({}, "frozen payload does not reproduce layout for type '" +
                      type.id.value + "'");
        if ((type.kind == TypeKind::Enum || type.kind == TypeKind::Result) &&
            luna::layout::inlineAdtLayoutSignature(restored) != type.abiLayout)
            error({}, "frozen payload does not reproduce inline ADT layout for type '" +
                      type.id.value + "'");
        const auto resource = resourceContractForType(restored);
        if (resource.usage != type.sysmeta.resource.usage ||
            resource.cleanup != type.sysmeta.resource.cleanup ||
            resource.cleanupRequired !=
                type.sysmeta.resource.cleanupRequired ||
            resource.recursiveCleanup !=
                type.sysmeta.resource.recursiveCleanup ||
            resource.lifetime != type.sysmeta.resource.lifetime ||
            resource.relation != type.sysmeta.resource.relation)
            error({}, "frozen payload does not reproduce Resource contract for type '" +
                      type.id.value + "'");
    }

    std::unordered_set<std::string> schemaIds;
    std::unordered_map<std::string, const MetadataSchema*> schemasById;
    for (const auto& schema : module.metadataSchemas) {
        if (schema.id.empty()) error(schema.location, "metadata schema has no stable id");
        else if (!schemaIds.insert(schema.id).second) {
            error(schema.location, "duplicate metadata schema id '" + schema.id + "'");
        } else {
            schemasById.emplace(schema.id, &schema);
        }
        if (schema.fields.empty())
            error(schema.location, "metadata schema '" + schema.name + "' has no fields");
        std::unordered_set<std::string> fieldNames;
        for (const auto& field : schema.fields) {
            if (field.name.empty())
                error(schema.location, "metadata schema '" + schema.name +
                                       "' contains an unnamed field");
            else if (!fieldNames.insert(field.name).second)
                error(schema.location, "metadata schema '" + schema.name +
                                       "' contains duplicate field '" + field.name + "'");
            verifyType(field.type, schema.location,
                       "metadata field '" + schema.name + "." + field.name + "'",
                       module);
        }
    }

    std::unordered_set<std::string> declarationIds;
    std::unordered_set<std::string> linkageNames;
    std::unordered_set<std::string> symbolIds;
    std::unordered_map<std::string, std::string> contractPayloads;
    std::unordered_map<std::string, const DeclarationRecord*> recordsById;
    for (const auto& record : module.declarationTable) {
        if (record.id.empty()) error(record.location, "declaration table entry has no id");
        else if (!declarationIds.insert(record.id).second) {
            error(record.location, "duplicate declaration id '" + record.id + "'");
        } else {
            recordsById.emplace(record.id, &record);
        }
        if (luna::identity::symbolIdFromCanonical(record.id) !=
            record.symbolId)
            error(record.location, "SymbolId payload mismatch for declaration '" +
                                   record.id + "'");
        else if (!symbolIds.insert(record.symbolId.value).second)
            error(record.location, "duplicate or colliding SymbolId '" +
                                   record.symbolId.value + "'");
        if (canonicalContract(record) != record.canonicalContract)
            error(record.location, "contract payload mismatch for declaration '" +
                                   record.id + "'");
        if (luna::identity::contractIdFromCanonical(
                record.canonicalContract) != record.contractId)
            error(record.location, "ContractId payload mismatch for declaration '" +
                                   record.id + "'");
        auto [contract, insertedContract] = contractPayloads.emplace(
            record.contractId.value, record.canonicalContract);
        if (!insertedContract &&
            contract->second != record.canonicalContract)
            error(record.location, "colliding ContractId '" +
                                   record.contractId.value + "'");
        if (record.sysmeta.identity.symbol != record.symbolId ||
            record.sysmeta.identity.contract != record.contractId)
            error(record.location, "declaration '" + record.id +
                                   "' has inconsistent identity sysmeta");
        if (!record.linkageName.empty() &&
            !linkageNames.insert(record.linkageName).second)
            error(record.location, "duplicate linkage identity '" +
                                   record.linkageName + "'");
        if (record.retention != Retention::CompileTime && !module.features.runtime)
            error(record.location, "runtime-retained declaration '" + record.id +
                                   "' is present without the runtime feature");
        if (record.sysmeta.schemaMajor != luna::sysmeta::SchemaMajor)
            error(record.location, "declaration '" + record.id +
                                   "' uses an unsupported sysmeta schema");
        if (!record.sysmeta.abi.dropGlueSymbol.empty())
            error(record.location, "declaration '" + record.id +
                                   "' retains a linkage-string Drop reference");
        if (!record.dropGlue.empty())
            verifyDeclarationRef(
                record.dropGlue, record.location,
                "Drop glue for declaration '" + record.id + "'",
                module, DeclarationKind::Function);
        if (record.sysmeta.capability.dynamicDispatch &&
            !module.features.dynamicApply)
            error(record.location, "declaration '" + record.id +
                                   "' requires dynamic dispatch without the module capability");
        if (record.sysmeta.capability.runtimeRetained !=
            (record.retention != Retention::CompileTime))
            error(record.location, "declaration '" + record.id +
                                   "' has inconsistent runtime-retention sysmeta");
        if (!record.type.empty()) {
            const auto* recordType = module.findType(record.type);
            if (!recordType)
                error(record.location, "declaration '" + record.id +
                                       "' references a type absent from the type table");
            else if ((recordType->kind == TypeKind::Function ||
                      recordType->kind == TypeKind::Slot ||
                      recordType->kind == TypeKind::Fragment) &&
                record.sysmeta.resource.parameters.size() !=
                    recordType->parameterTypeIds.size())
                error(record.location, "declaration '" + record.id +
                                       "' sysmeta parameter contract count does not match its type");
            if (recordType && record.dropGlue != recordType->dropGlue)
                error(record.location, "declaration '" + record.id +
                                       "' Drop reference differs from its frozen type");
            const size_t contractCount = std::min(
                record.sysmeta.resource.parameters.size(),
                recordType ? recordType->parameterContracts.size() : size_t{0});
            for (size_t index = 0; index < contractCount; ++index) {
                if (record.sysmeta.resource.parameters[index] !=
                    recordType->parameterContracts[index])
                    error(record.location, "declaration '" + record.id +
                                           "' sysmeta ownership contract differs from its type");
            }
            if (recordType &&
                record.sysmeta.resource.result != recordType->returnContract)
                error(record.location, "declaration '" + record.id +
                                       "' sysmeta result contract differs from its type");
        }
        if (record.kind == DeclarationKind::Fragment) {
            const auto form = record.sysmeta.control.form;
            if (form != luna::sysmeta::ControlForm::Interceptor &&
                form != luna::sysmeta::ControlForm::Context)
                error(record.location, "fragment '" + record.id +
                                       "' has no fragment control sysmeta");
            if (record.sysmeta.control.storage !=
                luna::sysmeta::ContinuationStorage::ScopedStack)
                error(record.location, "fragment '" + record.id +
                                       "' must use a scoped continuation in the current ABI");
            if (!record.sysmeta.capability.hostOnly)
                error(record.location, "fragment '" + record.id +
                                       "' must be host-only in the current ABI");
        }
        for (const auto& metadata : record.metadata) {
            auto schema = schemasById.find(metadata.schemaId);
            if (schema == schemasById.end()) {
                error(metadata.location, "metadata instance references unknown schema '" +
                                         metadata.schemaId + "'");
            } else {
                const auto& fields = schema->second->fields;
                if (metadata.values.size() != fields.size()) {
                    error(metadata.location, "metadata instance for schema '" +
                                             metadata.schemaId + "' has " +
                                             std::to_string(metadata.values.size()) +
                                             " values; expected " +
                                             std::to_string(fields.size()));
                }
                const size_t comparable = std::min(metadata.values.size(), fields.size());
                for (size_t index = 0; index < comparable; ++index) {
                    if (!metadataConstantMatches(
                            metadata.values[index],
                            module.findType(fields[index].type)))
                        error(metadata.location, "metadata value for field '" +
                                                 fields[index].name +
                                                 "' does not match its schema type");
                }
            }
            if (metadata.retention != Retention::CompileTime && !module.features.runtime)
                error(metadata.location, "runtime metadata is present without the runtime feature");
            if (static_cast<uint8_t>(metadata.retention) >
                static_cast<uint8_t>(record.retention))
                error(metadata.location, "metadata retention exceeds declaration retention for '" +
                                         record.id + "'");
        }
    }

    std::unordered_set<std::string> executableIds;
    for (const auto& declaration : module.declarations) {
        if (!declaration) {
            error({}, "module contains a null declaration");
            continue;
        }
        if (declaration->declarationId.empty())
            error(declaration->location, "executable declaration has no stable id");
        else if (!executableIds.insert(declaration->declarationId).second)
            error(declaration->location, "duplicate executable declaration id '" +
                                         declaration->declarationId + "'");
        auto record = recordsById.find(declaration->declarationId);
        if (record == recordsById.end()) {
            error(declaration->location, "executable declaration '" +
                                         declaration->declarationId +
                                         "' is absent from the declaration table");
        } else {
            const auto& linkage = declaration->generatedSymbolName;
            if (declaration->symbolId != record->second->symbolId)
                error(declaration->location, "declaration table SymbolId mismatch for '" +
                                             declaration->declarationId + "'");
            if (declaration->contractId != record->second->contractId)
                error(declaration->location, "declaration table ContractId mismatch for '" +
                                             declaration->declarationId + "'");
            if (record->second->linkageName != linkage)
                error(declaration->location, "declaration table linkage mismatch for '" +
                                             declaration->declarationId + "'");
            if (record->second->familyId != declaration->familyId)
                error(declaration->location, "declaration table family mismatch for '" +
                                             declaration->declarationId + "'");
            if (record->second->sysmeta.schemaMajor !=
                declaration->sysmeta.schemaMajor)
                error(declaration->location, "declaration sysmeta schema mismatch for '" +
                                             declaration->declarationId + "'");
        }
        verifyDeclaration(*declaration, module);
    }

    if (module.features.dynamicApply && !module.features.runtime)
        error({}, "dynamic apply requires the runtime feature");
    if (module.features.dynamicSelect && !module.features.runtime)
        error({}, "dynamic select requires the runtime feature");
    if (module.features.dynamicReflection && !module.features.runtime)
        error({}, "dynamic reflection requires the runtime feature");
    if (module.features.kernelRuntimeReserved && !module.features.kernel)
        error({}, "reserved kernel runtime requires the kernel feature");

    return mErrors.empty();
}

void Verifier::verifyDeclaration(const Decl& declaration, const Module& module) {
    if (!validSeparatedName(declaration.packageId, "."))
        error(declaration.location, "declaration has invalid Package ID '" +
                                    declaration.packageId + "'");
    if (!validSeparatedName(declaration.modulePath, "::", true))
        error(declaration.location, "declaration has invalid module path '" +
                                    declaration.modulePath + "'");
    if (!declaration.sysmeta.abi.dropGlueSymbol.empty())
        error(declaration.location,
              "executable declaration retains a linkage-string Drop reference");
    if (auto* function = dynamic_cast<const FunctionDecl*>(&declaration)) {
        verifyFunction(*function, module);
        return;
    }
    if (auto* fragment = dynamic_cast<const FragmentDecl*>(&declaration)) {
        if (!fragment->body)
            error(fragment->location, "fragment '" + fragment->name + "' has no body");
        verifyType(fragment->structuralType, fragment->location,
                   "fragment '" + fragment->name + "' structural type", module);
        for (const auto& parameter : fragment->params)
            verifyType(parameter.type, fragment->location,
                       "fragment parameter '" + parameter.name + "'", module);
        verifyBlock(fragment->body.get(), module, fragment->name);
        return;
    }
    if (auto* structure = dynamic_cast<const StructDecl*>(&declaration)) {
        verifyType(structure->type, structure->location,
                   "struct '" + structure->name + "'", module,
                   !structure->typeParams.empty());
        for (const auto& field : structure->fields)
            verifyType(field.type, structure->location,
                       "field '" + structure->name + "." + field.name + "'",
                       module,
                       !structure->typeParams.empty());
        return;
    }
    if (auto* enumeration = dynamic_cast<const EnumDecl*>(&declaration)) {
        verifyType(enumeration->type, enumeration->location,
                   "enum '" + enumeration->name + "'", module,
                   !enumeration->typeParams.empty());
        return;
    }
    if (auto* trait = dynamic_cast<const TraitDecl*>(&declaration)) {
        verifyType(trait->type, trait->location,
                   "trait '" + trait->name + "'", module, true);
        for (const auto& method : trait->methods) {
            for (const auto& parameter : method.params)
                verifyType(parameter.type, trait->location,
                           "trait method parameter '" + method.name + "." +
                               parameter.name + "'", module, true);
            verifyType(method.returnType, trait->location,
                       "trait method return '" + method.name + "'", module, true);
        }
        return;
    }
    if (auto* implementation = dynamic_cast<const ImplDecl*>(&declaration)) {
        verifyDeclarationRef(
            implementation->traitRef, implementation->location,
            "implementation trait", module, DeclarationKind::Trait);
        verifyType(implementation->targetType, implementation->location,
                   "implementation target", module,
                   !implementation->typeParams.empty());
        for (const auto& method : implementation->methods) {
            if (!method) error(implementation->location, "implementation contains a null method");
            else verifyFunction(
                *method, module,
                !implementation->typeParams.empty());
        }
    }
}

void Verifier::verifyFunction(
    const FunctionDecl& function, const Module& module,
    bool inheritsTypeParameters) {
    const bool generic = isGeneric(function) || inheritsTypeParameters;
    const bool previousAllowance = mAllowTypeParameters;
    mAllowTypeParameters = generic;
    if (function.name.empty()) error(function.location, "function has no source name");
    if (function.generatedSymbolName.empty() && function.linkName.empty())
        error(function.location, "function '" + function.name + "' has no linkage identity");
    for (const auto& parameter : function.params) {
        verifyType(parameter.type, function.location,
                   "parameter '" + function.name + "." + parameter.name + "'",
                   module, generic);
        if (parameter.isLinear !=
            (parameter.usage == luna::ownership::Usage::Linear))
            error(function.location, "parameter '" + function.name + "." +
                  parameter.name + "' has inconsistent linear compatibility flag");
        if (parameter.relation != luna::ownership::Relation::Owned &&
            parameter.usage != luna::ownership::Usage::Copy)
            error(function.location, "borrowed parameter '" + function.name + "." +
                  parameter.name + "' must use copy cardinality");
    }
    verifyType(function.returnType, function.location,
               "return type of '" + function.name + "'", module, generic);
    if (!function.isExtern && !function.body)
        error(function.location, "function '" + function.name + "' has no body");
    if (function.returnsLinear !=
        (function.returnUsage == luna::ownership::Usage::Linear))
        error(function.location, "function '" + function.name +
              "' has inconsistent linear return compatibility flag");
    if (function.isKernel && function.isCodegenReachable && !module.features.kernel)
        error(function.location, "kernel '" + function.name +
                                 "' is present without the kernel feature");
    const auto* returnType = module.findType(function.returnType);
    if (function.isKernel && returnType && returnType->kind != TypeKind::Unit)
        error(function.location, "kernel '" + function.name + "' must return unit");
    if (function.body) verifyBlock(function.body.get(), module, function.name);
    mAllowTypeParameters = previousAllowance;
}

void Verifier::verifyBlock(const BlockStmt* block, const Module& module,
                           const std::string& owner) {
    if (!block) {
        error({}, "null block in '" + owner + "'");
        return;
    }
    for (const auto& statement : block->stmts)
        verifyStmt(statement.get(), module, owner);
}

void Verifier::verifyStmt(const Stmt* stmt, const Module& module,
                          const std::string& owner) {
    if (!stmt) {
        error({}, "null statement in '" + owner + "'");
        return;
    }
    if (auto* block = dynamic_cast<const BlockStmt*>(stmt)) {
        verifyBlock(block, module, owner);
    } else if (auto* let = dynamic_cast<const LetStmt*>(stmt)) {
        if (let->name.empty()) error(let->location, "binding has no name in '" + owner + "'");
        verifyType(let->type, let->location, "binding '" + let->name + "'", module);
        verifyExpr(let->initializer.get(), module, owner);
        const auto* bindingType = module.findType(let->type);
        auto requiredUsage = bindingType
            ? bindingType->sysmeta.resource.usage
            : luna::ownership::Usage::Copy;
        if (auto* call = dynamic_cast<const CallExpr*>(
                let->initializer.get())) {
            const auto callUsage = call->returnsLinear
                ? luna::ownership::Usage::Linear
                : call->returnUsage;
            requiredUsage = luna::ownership::strongerUsage(
                requiredUsage, callUsage);
        }
        if (!luna::ownership::satisfiesUsageRequirement(
                let->usage, requiredUsage))
            error(let->location, "binding '" + let->name +
                  "' weakens its required usage contract in '" + owner + "'");
        if (let->isLinear != (let->usage == luna::ownership::Usage::Linear))
            error(let->location, "binding '" + let->name +
                  "' has inconsistent linear compatibility flag");
        if (let->materializesIteratorRecipe) {
            if (!bindingType || bindingType->kind != TypeKind::Iterator)
                error(let->location,
                      "materialized iterator binding '" +
                      let->name +
                      "' has no iterator type");
            auto* call = dynamic_cast<const CallExpr*>(
                let->initializer.get());
            if (!call ||
                call->iteratorOp == IteratorOp::None ||
                call->iteratorOp == IteratorOp::Fold ||
                call->iteratorOp == IteratorOp::ForEach ||
                call->iteratorOp == IteratorOp::Count ||
                call->iteratorOp == IteratorOp::Collect)
                error(let->location,
                      "materialized iterator binding '" +
                      let->name +
                      "' has no adapter recipe");
            if (let->materializedIteratorOwnsSource) {
                verifyType(
                    let->materializedIteratorSourceType,
                    let->location,
                    "materialized iterator source", module);
                const auto* source = module.findType(
                    let->materializedIteratorSourceType);
                const auto* sourceElement = source
                    ? module.findType(source->innerTypeId) : nullptr;
                if (!source ||
                    source->kind != TypeKind::Array ||
                    !sourceElement ||
                    sourceElement->sysmeta.resource.usage ==
                        luna::ownership::Usage::Copy ||
                    luna::ownership::mustConsume(
                        source->sysmeta.resource.usage))
                    error(let->location,
                          "owning materialized iterator '" +
                          let->name +
                          "' has no affine move-only array source");
            } else if (!let->materializedIteratorSourceType.empty()) {
                error(let->location,
                      "non-owning materialized iterator '" +
                      let->name +
                      "' carries an owning source witness");
            }
        } else if (let->materializedIteratorOwnsSource ||
                   !let->materializedIteratorSourceType.empty()) {
            error(let->location,
                  "ordinary binding '" + let->name +
                  "' carries materialized iterator source state");
        }
    } else if (auto* ret = dynamic_cast<const ReturnStmt*>(stmt)) {
        if (ret->value) verifyExpr(ret->value.get(), module, owner);
        std::unordered_set<std::string> cleanupPlaces;
        for (const auto& cleanup : ret->cleanups) {
            if (cleanup.place.empty())
                error(ret->location, "return cleanup in '" + owner + "' has no place");
            else if (!cleanupPlaces.insert(cleanup.place).second)
                error(ret->location, "duplicate return cleanup for place '" +
                      cleanup.place + "' in '" + owner + "'");
            if (cleanup.typeId.empty() || !module.findType(cleanup.typeId))
                error(ret->location, "return cleanup for place '" + cleanup.place +
                      "' references no frozen type in '" + owner + "'");
            else
                verifyCleanupAction(
                    cleanup.action, cleanup.typeId, ret->location,
                    "return cleanup for '" + cleanup.place + "'", module);
        }
    } else if (auto* expression = dynamic_cast<const ExprStmt*>(stmt)) {
        verifyExpr(expression->expr.get(), module, owner);
    } else if (auto* conditional = dynamic_cast<const IfStmt*>(stmt)) {
        verifyExpr(conditional->cond.get(), module, owner);
        verifyBlock(conditional->thenBlock.get(), module, owner);
        if (conditional->elseBranch)
            verifyStmt(conditional->elseBranch.get(), module, owner);
    } else if (auto* match = dynamic_cast<const MatchStmt*>(stmt)) {
        verifyExpr(match->scrutinee.get(), module, owner);
        verifyType(match->matchedType, match->location, "match type", module);
        if (match->arms.empty())
            error(match->location, "match in '" + owner + "' has no arms");
        const auto* matchedType = module.findType(match->matchedType);
        const size_t expectedVariantCount =
            matchedType && matchedType->kind == TypeKind::Result
                ? 2
                : (matchedType ? matchedType->variants.size() : 0);
        if (match->arms.size() != expectedVariantCount)
            error(match->location, "match in '" + owner +
                  "' is not exhaustive in frozen MoonIR");
        std::unordered_set<uint32_t> variants;
        for (const auto& arm : match->arms) {
            if (!variants.insert(arm.variantIndex).second)
                error(arm.location, "duplicate match variant index in '" +
                      owner + "'");
            if (arm.variantIndex >= expectedVariantCount)
                error(arm.location, "match arm has an out-of-range variant "
                      "index in '" + owner + "'");
            if (arm.bindings.size() != arm.bindingTypes.size())
                error(arm.location, "match arm binding/type arity mismatch in '" +
                      owner + "'");
            if (arm.bindings.size() != arm.bindingUsages.size())
                error(arm.location, "match arm binding/usage arity mismatch in '" +
                      owner + "'");
            TypeRefVec expectedFields;
            if (matchedType && matchedType->kind == TypeKind::Enum &&
                arm.variantIndex <
                    matchedType->variants.size()) {
                expectedFields =
                    matchedType->variants[
                        arm.variantIndex].fields;
            } else if (matchedType &&
                       matchedType->kind ==
                           TypeKind::Result &&
                       matchedType->typeArgumentIds.size() == 2 &&
                       arm.variantIndex < 2) {
                expectedFields.push_back(
                    matchedType->typeArgumentIds[
                        arm.variantIndex == 1 ? 0 : 1]);
            }
            if (arm.bindingTypes.size() != expectedFields.size())
                error(arm.location, "match arm payload arity disagrees with "
                      "its frozen variant in '" + owner + "'");
            const size_t comparable = std::min(
                arm.bindingTypes.size(), expectedFields.size());
            for (size_t index = 0; index < comparable; ++index) {
                if (arm.bindingTypes[index] != expectedFields[index])
                    error(arm.location, "match binding type disagrees with "
                          "its frozen variant payload in '" + owner + "'");
                if (index < arm.bindingUsages.size() &&
                    !luna::ownership::satisfiesUsageRequirement(
                        arm.bindingUsages[index],
                        frozenUsage(module, arm.bindingTypes[index])))
                    error(arm.location, "match binding weakens its required "
                          "usage contract in '" + owner + "'");
            }
            for (const auto& type : arm.bindingTypes)
                verifyType(type, arm.location, "match binding type", module);
            verifyBlock(arm.body.get(), module, owner);
        }
    } else if (auto* loop = dynamic_cast<const WhileStmt*>(stmt)) {
        verifyExpr(loop->cond.get(), module, owner);
        verifyBlock(loop->body.get(), module, owner);
    } else if (auto* loop = dynamic_cast<const ForStmt*>(stmt)) {
        verifyExpr(loop->iterable.get(), module, owner);
        verifyType(loop->elementType, loop->location,
                   "for-loop element type", module);
        if (!luna::ownership::satisfiesUsageRequirement(
                loop->bindingUsage,
                frozenUsage(module, loop->elementType)))
            error(loop->location, "for-loop binding weakens its required "
                  "usage contract in '" + owner + "'");
        if (!loop->protocolNext.empty()) {
            verifyDeclarationRef(
                loop->protocolNext, loop->location,
                "Iterator::next protocol witness", module,
                DeclarationKind::Function);
            verifyType(loop->protocolIteratorType, loop->location,
                       "iterator protocol state type", module);
            verifyType(loop->protocolOptionType, loop->location,
                       "iterator protocol option type", module);
            const auto* optionType = module.findType(loop->protocolOptionType);
            if (!optionType || optionType->kind != TypeKind::Enum) {
                error(loop->location,
                      "iterator protocol for-loop requires an enum Option type");
            } else {
                const auto variantCount =
                    optionType->variants.size();
                if (loop->protocolNoneVariant >= variantCount ||
                    loop->protocolSomeVariant >= variantCount ||
                    loop->protocolNoneVariant ==
                        loop->protocolSomeVariant)
                    error(loop->location,
                          "iterator protocol for-loop has invalid Option variants");
            }
            if (!loop->protocolInto.empty()) {
                verifyDeclarationRef(
                    loop->protocolInto, loop->location,
                    "IntoIterator protocol witness", module,
                    DeclarationKind::Function);
                verifyType(loop->protocolInputType,
                           loop->location,
                           "IntoIterator protocol input type", module);
                if (loop->protocolStateName.empty())
                    error(loop->location,
                          "IntoIterator protocol for-loop has no hidden "
                          "state identity");
            } else if (!loop->protocolStateName.empty()) {
                error(loop->location,
                      "direct Iterator for-loop unexpectedly owns a "
                      "hidden state identity");
            }
        }
        if (!loop->recipeStateName.empty()) {
            verifyType(loop->recipeSourceType,
                       loop->location,
                       "consuming recipe source type", module);
            const auto* recipeSource = module.findType(loop->recipeSourceType);
            const auto* recipeElement = recipeSource
                ? module.findType(recipeSource->innerTypeId) : nullptr;
            if (!recipeSource ||
                recipeSource->kind !=
                    TypeKind::Array ||
                !recipeElement ||
                recipeElement->sysmeta.resource.usage ==
                    luna::ownership::Usage::Copy)
                error(loop->location,
                      "consuming recipe state must own a move-only array");
        }
        verifyBlock(loop->body.get(), module, owner);
    } else if (auto* release = dynamic_cast<const FreeStmt*>(stmt)) {
        verifyExpr(release->operand.get(), module, owner);
        if (release->operand && !release->operand->type.empty())
            verifyCleanupAction(
                release->action,
                release->operand->type,
                release->location, "free operation", module);
    } else if (auto* slot = dynamic_cast<const SlotDeclStmt*>(stmt)) {
        verifyType(slot->structuralType, slot->location,
                   "slot '" + slot->name + "' structural contract", module);
        if (!slot->defaultFragment.empty() ||
            !slot->defaultFragmentRef.empty())
            verifyDeclarationRef(
                slot->defaultFragmentRef, slot->location,
                "default fragment for slot '" + slot->name + "'", module,
                DeclarationKind::Fragment);
    } else if (auto* slot = dynamic_cast<const SlotInvokeStmt*>(stmt)) {
        for (const auto& argument : slot->args)
            verifyExpr(argument.get(), module, owner);
        verifyType(slot->structuralType, slot->location,
                   "slot invocation '" + slot->name + "' contract", module);
        if (!slot->defaultFragment.empty() ||
            !slot->defaultFragmentRef.empty())
            verifyDeclarationRef(
                slot->defaultFragmentRef, slot->location,
                "default fragment for slot invocation '" +
                slot->name + "'", module, DeclarationKind::Fragment);
        for (const auto& candidate : slot->dynamicFragmentRefs)
            verifyDeclarationRef(
                candidate, slot->location,
                "dynamic fragment candidate for slot '" +
                slot->name + "'", module, DeclarationKind::Fragment);
        verifyBlock(slot->continuation.get(), module, owner);
        if (slot->usesDynamicDispatch && !module.features.dynamicApply)
            error(slot->location, "dynamic slot dispatch is present without dynamic apply capability");
    } else if (auto* apply = dynamic_cast<const ApplyStmt*>(stmt)) {
        verifyDeclarationRef(
            apply->fragmentRef, apply->location,
            "fragment bound by apply for slot '" + apply->slotName + "'",
            module, DeclarationKind::Fragment);
        std::unordered_set<std::string> fragmentSymbols{
            apply->fragmentRef.symbol.value};
        for (const auto& candidate : apply->alternativeFragmentRefs) {
            verifyDeclarationRef(
                candidate, apply->location,
                "dynamic apply fragment candidate for slot '" +
                apply->slotName + "'", module, DeclarationKind::Fragment);
            if (!fragmentSymbols.insert(candidate.symbol.value).second)
                error(apply->location,
                      "dynamic apply repeats fragment SymbolId '" +
                      candidate.symbol.value + "'");
        }
        if (apply->isDynamic && !module.features.dynamicApply)
            error(apply->location, "dynamic apply is present without dynamic apply capability");
        if (apply->body) verifyBlock(apply->body.get(), module, owner);
    } else if (auto* abort = dynamic_cast<const AbortStmt*>(stmt)) {
        std::unordered_set<std::string> cleanupPlaces;
        for (const auto& cleanup : abort->cleanups) {
            if (cleanup.place.empty())
                error(abort->location, "abort cleanup in '" + owner + "' has no place");
            else if (!cleanupPlaces.insert(cleanup.place).second)
                error(abort->location, "duplicate abort cleanup for place '" +
                      cleanup.place + "' in '" + owner + "'");
            if (cleanup.typeId.empty() || !module.findType(cleanup.typeId))
                error(abort->location, "abort cleanup for place '" + cleanup.place +
                      "' references no frozen type in '" + owner + "'");
            else
                verifyCleanupAction(
                    cleanup.action, cleanup.typeId, abort->location,
                    "abort cleanup for '" + cleanup.place + "'", module);
        }
    } else if (auto* await = dynamic_cast<const AwaitStmt*>(stmt)) {
        verifyExpr(await->event.get(), module, owner);
    }
}

void Verifier::verifyExpr(const Expr* expr, const Module& module,
                          const std::string& owner) {
    if (!expr) {
        error({}, "null expression in '" + owner + "'");
        return;
    }
    if (auto* unit = dynamic_cast<const UnitExpr*>(expr)) {
        verifyType(unit->type, unit->location, "unit expression", module);
        const auto* type = module.findType(unit->type);
        if (!type || type->kind != TypeKind::Unit)
            error(unit->location,
                  "canonical unit expression does not have unit type");
    } else if (auto* selection = dynamic_cast<const DynamicSelectExpr*>(expr)) {
        if (!module.features.dynamicSelect || !module.features.runtime)
            error(selection->location,
                  "dynamic select expression is present without runtime dynamic-select capability");
        verifyType(selection->type, selection->location,
                   "dynamic select callable type", module);
        const auto* selectionType = module.findType(selection->type);
        if (!selectionType || selectionType->kind != TypeKind::Function)
            error(selection->location, "dynamic select must produce a callable type");
        if (selection->familyId.empty() || selection->selector.empty() ||
            selection->metadataSchemaId.empty())
            error(selection->location, "dynamic select has an incomplete binding identity");
        else
            verifyDeclarationRef(
                selection->selector, selection->location,
                "dynamic selector", module, DeclarationKind::Function);
        if (selection->filterArguments.empty() || selection->candidates.empty())
            error(selection->location, "dynamic select has an empty filter or candidate set");
        for (const auto& argument : selection->filterArguments)
            verifyExpr(argument.get(), module, owner);
        std::unordered_set<std::string> candidateIds;
        for (const auto& candidate : selection->candidates) {
            if (!candidate.declaration.complete())
                error(selection->location,
                      "dynamic select candidate has no stable declaration reference");
            else if (!candidateIds.insert(
                         candidate.declaration.symbol.value).second)
                error(selection->location, "dynamic select contains duplicate candidate '" +
                                           candidate.declaration.symbol.value + "'");
            if (candidate.metadataValues.size() != selection->filterArguments.size())
                error(selection->location, "dynamic select candidate metadata shape mismatch");
            const DeclarationRecord* record = verifyDeclarationRef(
                candidate.declaration, selection->location,
                "dynamic select candidate", module,
                DeclarationKind::Function);
            if (!record || record->retention == Retention::CompileTime) {
                error(selection->location, "dynamic select candidate '" +
                      candidate.declaration.symbol.value +
                      "' has no runtime descriptor");
                continue;
            }
            if (record->familyId != selection->familyId)
                error(selection->location, "dynamic select candidate '" +
                                           candidate.declaration.symbol.value +
                                           "' does not match its declared linkage/family");
            bool retainedMetadata = false;
            for (const auto& metadata : record->metadata) {
                if (metadata.schemaId == selection->metadataSchemaId &&
                    metadata.retention != Retention::CompileTime &&
                    metadata.values == candidate.metadataValues) {
                    retainedMetadata = true;
                    break;
                }
            }
            if (!retainedMetadata)
                error(selection->location, "dynamic select candidate '" +
                      candidate.declaration.symbol.value +
                      "' does not retain the inspected metadata at runtime");
        }
    } else if (auto* identifier = dynamic_cast<const IdentifierExpr*>(expr)) {
        if (!identifier->declaration.empty())
            verifyDeclarationRef(
                identifier->declaration, identifier->location,
                "declaration-valued identifier '" + identifier->name + "'",
                module, DeclarationKind::Function);
    } else if (auto* binary = dynamic_cast<const BinaryExpr*>(expr)) {
        verifyExpr(binary->lhs.get(), module, owner);
        verifyExpr(binary->rhs.get(), module, owner);
    } else if (auto* unary = dynamic_cast<const UnaryExpr*>(expr)) {
        verifyExpr(unary->operand.get(), module, owner);
    } else if (auto* call = dynamic_cast<const CallExpr*>(expr)) {
        verifyExpr(call->callee.get(), module, owner);
        for (const auto& argument : call->args)
            verifyExpr(argument.get(), module, owner);
        if (!call->calleeRef.empty())
            verifyDeclarationRef(
                call->calleeRef, call->location,
                "direct call target", module, DeclarationKind::Function);
        if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(
                call->callee.get());
            identifier &&
            (!call->calleeRef.empty() || !identifier->declaration.empty())) {
            if (call->calleeRef != identifier->declaration)
                error(call->location,
                      "direct call and callee identifier disagree on their DeclarationRef");
        }
        if (!call->intrinsicType.empty())
            verifyType(call->intrinsicType, call->location,
                       "intrinsic call type witness", module);
        if (call->iteratorOp != IteratorOp::None) {
            verifyType(call->iteratorInputType, call->location,
                       "iterator operation input", module);
            verifyType(call->iteratorOutputType, call->location,
                       "iterator operation output", module);
            const auto* callType = module.findType(call->type);
            if (!callType)
                error(call->location,
                      "iterator operation has no result type");
            const bool terminal =
                call->iteratorOp == IteratorOp::Fold ||
                call->iteratorOp == IteratorOp::ForEach ||
                call->iteratorOp == IteratorOp::Count ||
                call->iteratorOp == IteratorOp::Collect;
            if (!terminal &&
                (!callType || callType->kind != TypeKind::Iterator))
                error(call->location,
                      "iterator adapter does not produce an iterator recipe");
            if (call->iteratorOp == IteratorOp::Collect) {
                verifyType(
                    call->iteratorCollectTargetType,
                    call->location,
                    "iterator collect target", module);
                verifyType(
                    call->iteratorCollectBuilderType,
                    call->location,
                    "iterator collect builder", module);
                if (!call->iteratorCollectBegin.complete() ||
                    !call->iteratorCollectPush.complete() ||
                    !call->iteratorCollectFinish.complete())
                    error(call->location,
                          "iterator collect has an incomplete "
                          "FromIterator protocol witness");
                else {
                    verifyDeclarationRef(
                        call->iteratorCollectBegin, call->location,
                        "FromIterator begin witness", module,
                        DeclarationKind::Function);
                    verifyDeclarationRef(
                        call->iteratorCollectPush, call->location,
                        "FromIterator push witness", module,
                        DeclarationKind::Function);
                    verifyDeclarationRef(
                        call->iteratorCollectFinish, call->location,
                        "FromIterator finish witness", module,
                        DeclarationKind::Function);
                }
            }
            if (!call->iteratorRecipeStateName.empty()) {
                if (!terminal)
                    error(call->location,
                          "non-terminal iterator adapter owns terminal recipe state");
                verifyType(
                    call->iteratorRecipeSourceType,
                    call->location,
                    "iterator terminal recipe source", module);
                const auto* recipeSource = module.findType(
                    call->iteratorRecipeSourceType);
                const auto* recipeElement = recipeSource
                    ? module.findType(recipeSource->innerTypeId) : nullptr;
                if (!recipeSource || recipeSource->kind !=
                        TypeKind::Array ||
                    !recipeElement ||
                    recipeElement->sysmeta.resource.usage ==
                        luna::ownership::Usage::Copy)
                    error(call->location,
                          "iterator terminal recipe state does not own a move-only array");
            }
        }
        if (call->returnsLinear !=
            (call->returnUsage == luna::ownership::Usage::Linear))
            error(call->location, "call in '" + owner +
                  "' has inconsistent linear result compatibility flag");
    } else if (auto* launch = dynamic_cast<const LaunchExpr*>(expr)) {
        if (!module.features.kernel)
            error(launch->location, "kernel launch is present without kernel capability");
        const auto* kernel = verifyDeclarationRef(
            launch->kernelRef, launch->location,
            "kernel launch target", module, DeclarationKind::Function);
        if (!kernel)
            error(launch->location, "kernel launch has no resolved declaration identity");
        else if (!kernel->sysmeta.capability.gpu)
            error(launch->location,
                  "kernel launch target is not GPU-capable");
        verifyExpr(launch->threads.get(), module, owner);
        for (const auto& argument : launch->args)
            verifyExpr(argument.get(), module, owner);
    } else if (auto* variant = dynamic_cast<const VariantConstructExpr*>(expr)) {
        verifyType(variant->constructedType, variant->location,
                   "constructed enum '" + variant->typeName + "'", module);
        for (const auto& argument : variant->args)
            verifyExpr(argument.get(), module, owner);
    } else if (auto* result =
                   dynamic_cast<const ResultConstructExpr*>(expr)) {
        verifyType(result->type, result->location,
                   "constructed Result", module);
        verifyExpr(result->payload.get(), module, owner);
        const auto* resultType = module.findType(result->type);
        if (!resultType || resultType->kind != TypeKind::Result ||
            resultType->typeArgumentIds.size() != 2) {
            error(result->location,
                  "Result construction has no frozen Result<T, E> type");
        } else if (!result->payload ||
                   result->payload->type != resultType->typeArgumentIds[
                       result->isOk ? 0 : 1]) {
            error(result->location,
                  "Result construction payload disagrees with its tag");
        }
    } else if (auto* field = dynamic_cast<const FieldAccessExpr*>(expr)) {
        verifyExpr(field->object.get(), module, owner);
    } else if (auto* index = dynamic_cast<const IndexExpr*>(expr)) {
        verifyExpr(index->object.get(), module, owner);
        verifyExpr(index->index.get(), module, owner);
    } else if (auto* length = dynamic_cast<const SliceLengthExpr*>(expr)) {
        verifyExpr(length->slice.get(), module, owner);
        verifyType(length->type, length->location,
                   "slice length result", module);
        const auto* resultType = module.findType(length->type);
        const auto* sliceType = length->slice
            ? module.findType(length->slice->type) : nullptr;
        if (!resultType || resultType->kind != TypeKind::USize)
            error(length->location,
                  "slice length projection does not produce canonical usize");
        if (!sliceType || sliceType->kind != TypeKind::Slice)
            error(length->location,
                  "slice length projection operand is not a frozen slice");
    } else if (auto* array = dynamic_cast<const ArrayLiteralExpr*>(expr)) {
        verifyType(array->elementType, array->location, "array element", module);
        for (const auto& element : array->elements)
            verifyExpr(element.get(), module, owner);
    } else if (auto* record = dynamic_cast<const RecordLiteralExpr*>(expr)) {
        verifyType(record->type, record->location, "record literal", module);
        const auto* recordType = module.findType(record->type);
        if (!recordType ||
            (recordType->kind != TypeKind::Record &&
             recordType->kind != TypeKind::Struct))
            error(record->location,
                  "record literal has neither a structural record nor named struct type");
        std::unordered_set<std::string> names;
        for (const auto& field : record->fields) {
            if (field.name.empty() || !names.insert(field.name).second)
                error(record->location,
                      "record literal contains an empty or duplicate field");
            verifyExpr(field.value.get(), module, owner);
        }
    } else if (auto* initialized =
                   dynamic_cast<const InitAllocationExpr*>(expr)) {
        verifyType(initialized->type, initialized->location,
                   "initialized allocation result", module);
        verifyType(initialized->allocatedType, initialized->location,
                   "initialized allocation storage", module);
        const auto* allocated = module.findType(
            initialized->allocatedType);
        if (!allocated || initialized->type != initialized->allocatedType)
            error(initialized->location,
                  "initialized allocation result disagrees with its storage type");
        if (initialized->storage != HeapStorageKind::Unique)
            error(initialized->location,
                  "initialized allocation uses unsupported storage");
        std::unordered_set<uint32_t> indices;
        for (const auto& element : initialized->elements) {
            if (!indices.insert(element.index).second)
                error(initialized->location,
                      "initialized allocation repeats an element index");
            verifyExpr(element.value.get(), module, owner);
            if (!allocated || !element.value) continue;
            TypeRef expected;
            if (allocated->kind == TypeKind::Struct ||
                allocated->kind == TypeKind::Record) {
                if (element.index >= allocated->fields.size()) {
                    error(initialized->location,
                          "initialized allocation element is outside its product type");
                    continue;
                }
                expected = allocated->fields[element.index].type;
            } else if (element.index == 0) {
                expected = initialized->allocatedType;
            } else {
                error(initialized->location,
                      "scalar allocation has a nonzero initializer index");
                continue;
            }
            if (element.value->type != expected)
                error(initialized->location,
                      "initialized allocation element type disagrees with frozen layout");
        }
        const size_t expectedCount = allocated &&
                (allocated->kind == TypeKind::Struct ||
                 allocated->kind == TypeKind::Record)
            ? allocated->fields.size() : 1;
        if (initialized->elements.size() != expectedCount)
            error(initialized->location,
                  "initialized allocation does not cover its frozen layout");
    } else if (auto* allocation = dynamic_cast<const HeapAllocExpr*>(expr)) {
        verifyType(allocation->allocatedType, allocation->location,
                   "heap allocation", module);
        verifyExpr(allocation->initializer.get(), module, owner);
    } else if (auto* propagation = dynamic_cast<const TryExpr*>(expr)) {
        verifyExpr(propagation->operand.get(), module, owner);
        verifyType(propagation->resultType, propagation->location,
                   "error propagation Result", module);
        verifyType(propagation->propagatedResultType, propagation->location,
                   "propagated Result", module);
        verifyType(propagation->valueType, propagation->location,
                   "error propagation value", module);
        verifyType(propagation->errorType, propagation->location,
                   "error propagation error", module);
        verifyType(propagation->propagatedErrorType, propagation->location,
                   "propagated error", module);
        const auto* resultType = module.findType(propagation->resultType);
        const auto* propagatedResultType = module.findType(
            propagation->propagatedResultType);
        if (!resultType || resultType->kind != TypeKind::Result ||
            resultType->typeArgumentIds.size() != 2)
            error(propagation->location,
                  "error propagation has no validated Result<T, E> type");
        if (!propagatedResultType ||
            propagatedResultType->kind != TypeKind::Result ||
            propagatedResultType->typeArgumentIds.size() != 2)
            error(propagation->location,
                  "error propagation has no validated enclosing Result<T, E> type");
        if (propagation->errorType != propagation->propagatedErrorType) {
            if (propagation->errorConversion.empty())
                error(propagation->location,
                      "error propagation changes error type without a static From conversion");
            else
                verifyDeclarationRef(
                    propagation->errorConversion,
                    propagation->location,
                    "error propagation From conversion", module,
                    DeclarationKind::Function);
        } else if (!propagation->errorConversion.empty()) {
            error(propagation->location,
                  "error propagation carries an unnecessary From conversion");
        }
        std::unordered_set<std::string> cleanupPlaces;
        for (const auto& cleanup : propagation->cleanups) {
            if (cleanup.place.empty())
                error(propagation->location,
                      "error propagation cleanup has no place in '" + owner + "'");
            else if (!cleanupPlaces.insert(cleanup.place).second)
                error(propagation->location,
                      "duplicate error propagation cleanup for '" +
                      cleanup.place + "' in '" + owner + "'");
            if (cleanup.typeId.empty() || !module.findType(cleanup.typeId))
                error(propagation->location,
                      "error propagation cleanup for '" + cleanup.place +
                      "' references no frozen type in '" + owner + "'");
            else
                verifyCleanupAction(
                    cleanup.action, cleanup.typeId,
                    propagation->location,
                    "error propagation cleanup for '" + cleanup.place + "'",
                    module);
        }
    } else if (auto* move = dynamic_cast<const MoveExpr*>(expr)) {
        verifyExpr(move->operand.get(), module, owner);
    } else if (auto* borrow = dynamic_cast<const BorrowExpr*>(expr)) {
        verifyExpr(borrow->operand.get(), module, owner);
    } else if (auto* dereference = dynamic_cast<const DerefExpr*>(expr)) {
        verifyExpr(dereference->operand.get(), module, owner);
    } else if (auto* address = dynamic_cast<const AddrOfExpr*>(expr)) {
        verifyExpr(address->operand.get(), module, owner);
    } else if (auto* block = dynamic_cast<const BlockExpr*>(expr)) {
        verifyBlock(block->block.get(), module, owner);
    } else if (auto* conditional = dynamic_cast<const IfExpr*>(expr)) {
        verifyExpr(conditional->cond.get(), module, owner);
        verifyExpr(conditional->thenExpr.get(), module, owner);
        verifyExpr(conditional->elseExpr.get(), module, owner);
    } else if (auto* lambda = dynamic_cast<const LambdaExpr*>(expr)) {
        verifyType(lambda->returnType, lambda->location,
                   "lambda return type", module);
        verifyType(lambda->closureType, lambda->location,
                   "lambda closure type", module);
        if (lambda->type != lambda->closureType)
            error(lambda->location,
                  "lambda expression type disagrees with its closure type");
        const auto* closure = module.findType(lambda->closureType);
        if (!closure || closure->kind != TypeKind::Function) {
            error(lambda->location,
                  "lambda closure type is not a frozen function type");
        } else {
            if (closure->parameterTypeIds.size() != lambda->params.size() ||
                closure->parameterContracts.size() != lambda->params.size())
                error(lambda->location,
                      "lambda parameter list disagrees with its closure type");
            const size_t comparable = std::min(
                lambda->params.size(), closure->parameterTypeIds.size());
            for (size_t index = 0; index < comparable; ++index) {
                if (lambda->params[index].type !=
                    closure->parameterTypeIds[index])
                    error(lambda->location,
                          "lambda parameter type disagrees with its closure type");
                if (index < closure->parameterContracts.size() &&
                    (lambda->params[index].relation !=
                         closure->parameterContracts[index].relation ||
                     lambda->params[index].usage !=
                         closure->parameterContracts[index].usage))
                    error(lambda->location,
                          "lambda parameter contract disagrees with its closure type");
            }
            if (closure->returnTypeId != lambda->returnType)
                error(lambda->location,
                      "lambda return type disagrees with its closure type");
        }
        std::unordered_set<std::string> parameterNames;
        for (const auto& parameter : lambda->params) {
            if (parameter.name.empty() ||
                !parameterNames.insert(parameter.name).second)
                error(lambda->location,
                      "lambda contains an empty or duplicate parameter name");
            verifyType(parameter.type, lambda->location,
                       "lambda parameter '" + parameter.name + "'", module);
            if (parameter.isLinear !=
                (parameter.usage == luna::ownership::Usage::Linear))
                error(lambda->location,
                      "lambda parameter has an inconsistent linear compatibility flag");
        }
        if (!lambda->captures.empty())
            error(lambda->location,
                  "lambda capture has no canonical closure environment layout");
        if (static_cast<bool>(lambda->body) ==
            static_cast<bool>(lambda->controlFlow)) {
            error(lambda->location,
                  "lambda must own exactly one structured or canonical body");
        } else if (lambda->body) {
            verifyBlock(lambda->body.get(), module, owner);
        } else {
            const auto* root = lambda->controlFlow->findRegion(
                lambda->controlFlow->rootRegion);
            if (!root || root->kind != RegionKind::Lambda)
                error(lambda->location,
                      "lambda canonical body has no lambda root region");

            std::vector<const LocalRecord*> parameters;
            for (const auto& local : lambda->controlFlow->locals)
                if (local.kind == LocalKind::Parameter)
                    parameters.push_back(&local);
            if (parameters.size() != lambda->params.size()) {
                error(lambda->location,
                      "lambda canonical body parameter table has the wrong arity");
            } else {
                for (size_t index = 0; index < parameters.size(); ++index) {
                    const auto& expected = lambda->params[index];
                    const auto& actual = *parameters[index];
                    if (actual.scope != lambda->controlFlow->rootScope ||
                        actual.name != expected.name ||
                        actual.type != expected.type ||
                        actual.usage != expected.usage ||
                        actual.relation != expected.relation)
                        error(lambda->location,
                              "lambda canonical parameter disagrees with its signature");
                }
            }

            Verifier nestedVerifier;
            if (!nestedVerifier.verify(*lambda->controlFlow, module))
                mErrors.insert(mErrors.end(), nestedVerifier.errors().begin(),
                               nestedVerifier.errors().end());
        }
    } else if (auto* assignment = dynamic_cast<const AssignExpr*>(expr)) {
        verifyExpr(assignment->lhs.get(), module, owner);
        verifyExpr(assignment->rhs.get(), module, owner);
    }
}

void Verifier::verifyType(const TypeRef& reference,
                          const SourceLocation& location,
                          const std::string& context,
                          const Module& module,
                          bool allowTypeParameter) {
    if (reference.empty()) {
        error(location, context + " has no resolved type");
        return;
    }
    const auto* type = module.findType(reference);
    if (!type) {
        error(location, context + " references type '" + reference.value +
                        "' absent from the MoonIR type table");
        return;
    }
    if (!mActiveTypeIds.insert(reference.value).second) return;
    if (type->domain == luna::types::TypeDomain::Inference ||
        type->domain == luna::types::TypeDomain::Error)
        error(location, context + " contains a non-materialized Sema type");
    if (type->kind == TypeKind::Unknown || type->kind == TypeKind::InferenceVar)
        error(location, context + " contains an unresolved type");
    if (type->kind == TypeKind::TypeParam &&
        !allowTypeParameter && !mAllowTypeParameters)
        error(location, context + " contains a type parameter outside a generic recipe");
    if (!type->innerTypeId.empty())
        verifyType(type->innerTypeId, location, context, module,
                   allowTypeParameter);
    for (const auto& argument : type->typeArgumentIds)
        verifyType(argument, location, context, module, allowTypeParameter);
    for (const auto& parameter : type->parameterTypeIds)
        verifyType(parameter, location, context, module, allowTypeParameter);
    if (!type->returnTypeId.empty())
        verifyType(type->returnTypeId, location, context, module,
                   allowTypeParameter);
    for (const auto& field : type->fields)
        verifyType(field.type, location, context + "." + field.name, module,
                   allowTypeParameter);
    for (const auto& variant : type->variants)
        for (const auto& field : variant.fields)
            verifyType(field, location, context + "::" + variant.name, module,
                       allowTypeParameter);
    mActiveTypeIds.erase(reference.value);
}

void Verifier::error(const SourceLocation& location, const std::string& message) {
    mErrors.push_back(diagnostic::format(
        "moon-verify", message, location.path, location.line, location.column,
        "MoonIR must be valid before LLVM lowering or Moon container emission"));
}

} // namespace moon
