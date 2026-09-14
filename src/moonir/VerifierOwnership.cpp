#include "Verifier.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <unordered_set>
#include <vector>

namespace moon {

void Verifier::verifyOwnershipDataflow(
    const ControlFlowGraph& graph, const Module& module, bool hasEntry) {
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
        const SourceLocation& location, const std::string& context,
        LocalId guardedCursor = {}) {
        if (place.root.empty() || place.root.value >= cleanupsByLocal.size())
            return;
        if (!guardedCursor.empty()) {
            bool matched = !cleanupsByLocal[place.root.value].empty();
            for (const auto cleanupId : cleanupsByLocal[place.root.value]) {
                if (cleanupId.value >= state.size()) {
                    matched = false;
                    continue;
                }
                const auto& cleanup = graph.cleanups[cleanupId.value];
                if (!cleanup.guard ||
                    cleanup.guard->nextUnread != guardedCursor) {
                    matched = false;
                } else if (!state[cleanupId.value]) {
                    error(location, context +
                          " consumes an inactive guarded array tail");
                }
            }
            if (!matched)
                error(location, context +
                      " has no active guarded array tail");
            return;
        }
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
                    &activateLocal, &graph, &module](
        const Expr* expression, CleanupState& state) {
        if (!expression) return;
        if (dynamic_cast<const IntLiteralExpr*>(expression) ||
            dynamic_cast<const FloatLiteralExpr*>(expression) ||
            dynamic_cast<const StringLiteralExpr*>(expression) ||
            dynamic_cast<const BoolLiteralExpr*>(expression) ||
            dynamic_cast<const UnitExpr*>(expression))
            return;
        if (const auto* move = dynamic_cast<const MoveExpr*>(expression)) {
            transferExpr(move->operand.get(), state);
            const auto* movedType = move->operand
                ? module.findType(move->operand->type) : nullptr;
            // `move` of a Copy projection reads the field value; it does not
            // partially consume or deactivate the owning aggregate cleanup.
            // This matters for affine nominal products containing Copy fields.
            if (auto place = placeOf(move->operand.get())) {
                if (place->projections.empty() || !movedType ||
                    movedType->sysmeta.resource.usage !=
                        luna::ownership::Usage::Copy)
                    consumePlace(*place, state, move->location, "move",
                                 move->nextUnread);
            }
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
        } else if (const auto* closure =
                       dynamic_cast<const MakeClosureExpr*>(expression)) {
            // Each captured value is moved into the closure environment and
            // its cleanup obligation transfers to the closure value (C016 CL010).
            for (const auto& value : closure->capturedValues) {
                transferExpr(value.get(), state);
                if (auto place = placeOf(value.get()))
                    consumePlace(*place, state, value->location,
                                 "closure capture");
            }
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
        const auto normalizeGuardedArrayTail = [&graph, &result](size_t begin) {
            size_t groupBegin = begin;
            while (groupBegin < result.size()) {
                const auto* first = graph.findCleanup(result[groupBegin]);
                const bool isGuardedElement = first && first->guard &&
                    first->place.projections.size() == 1 &&
                    first->place.projections.front().kind ==
                        ProjectionKind::ConstantIndex &&
                    first->place.projections.front().index ==
                        first->guard->elementIndex;
                if (!isGuardedElement) {
                    ++groupBegin;
                    continue;
                }
                size_t groupEnd = groupBegin + 1;
                uint64_t previousIndex = first->guard->elementIndex;
                while (groupEnd < result.size()) {
                    const auto* next = graph.findCleanup(result[groupEnd]);
                    if (!next || !next->guard ||
                        next->place.root != first->place.root ||
                        next->guard->nextUnread != first->guard->nextUnread ||
                        next->place.projections.size() != 1 ||
                        next->place.projections.front().kind !=
                            ProjectionKind::ConstantIndex ||
                        next->place.projections.front().index !=
                            next->guard->elementIndex ||
                        next->guard->elementIndex + 1 != previousIndex)
                        break;
                    previousIndex = next->guard->elementIndex;
                    ++groupEnd;
                }
                std::reverse(
                    result.begin() + static_cast<std::ptrdiff_t>(groupBegin),
                    result.begin() + static_cast<std::ptrdiff_t>(groupEnd));
                groupBegin = groupEnd;
            }
        };
        std::unordered_set<uint32_t> visited;
        for (const ScopeRecord* scope = graph.findScope(source); scope;
             scope = graph.findScope(scope->parent)) {
            if (!visited.insert(scope->id.value).second ||
                targetAncestors.count(scope->id.value))
                break;
            const size_t scopeBegin = result.size();
            for (auto cleanup = scope->cleanups.rbegin();
                 cleanup != scope->cleanups.rend(); ++cleanup)
                if (cleanup->value < state.size() && state[cleanup->value])
                    result.push_back(*cleanup);
            normalizeGuardedArrayTail(scopeBegin);
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

    if (hasEntry) {
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
                            const auto* returnedType = module.findType(
                                block.terminator.operand->type);
                            if (returnedType &&
                                (luna::ownership::isMoveOnly(
                                     returnedType->sysmeta.resource.usage) ||
                                 returnedType->sysmeta.resource.cleanupRequired))
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
}

} // namespace moon
