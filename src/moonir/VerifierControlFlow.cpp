#include "Verifier.h"
#include "VerifierInternal.h"
#include "../core/TypeLayout.h"
#include "../core/TypeRelations.h"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace moon {

using namespace verifier_detail;

void Verifier::verifyControlFlowBlocks(
    const ControlFlowGraph& graph, const Module& module,
    const std::unordered_set<uint32_t>& guardedCursorIds,
    std::vector<std::vector<BlockId>>& successors) {
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

    const auto enclosingRegion = [&graph](
        RegionId start, RegionKind kind) -> const RegionRecord* {
        std::unordered_set<uint32_t> visited;
        for (const RegionRecord* region = graph.findRegion(start); region;
             region = graph.findRegion(region->parent)) {
            if (!visited.insert(region->id.value).second) break;
            if (region->kind == kind) return region;
        }
        return nullptr;
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
    const auto scopeWithin = [&graph](ScopeId scope, ScopeId ancestor) {
        std::unordered_set<uint32_t> visited;
        for (const ScopeRecord* current = graph.findScope(scope); current;
             current = graph.findScope(current->parent)) {
            if (!visited.insert(current->id.value).second) break;
            if (current->id == ancestor) return true;
        }
        return false;
    };
    const auto scanGraphIdentifier =
        [this, &graph, &enclosingRegion, &localVisibleFrom, &scopeWithin](
            const IdentifierExpr& identifier, const BasicBlock& block,
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
                if (const auto* continuation = enclosingRegion(
                        block.region, RegionKind::Continuation)) {
                    const auto* fragment = enclosingRegion(
                        continuation->parent, RegionKind::Fragment);
                    if (fragment &&
                        scopeWithin(local->scope, fragment->scope) &&
                        !scopeWithin(local->scope, continuation->scope))
                        error(identifier.location,
                              "context continuation references fragment-local state");
                }
                if (local->kind == LocalKind::Synthetic &&
                    luna::ownership::isMoveOnly(local->usage) &&
                    !allowSyntheticTransfer)
                    error(identifier.location,
                          "synthetic affine local is read without transfer");
            } else if (identifier.declaration.empty()) {
                // Compiler intrinsics (print, panic, slice, etc.) have no
                // declaration table row. Their callee identifiers appear in
                // sealed CFG CallExprs without a LocalId or DeclarationRef.
                if (!isCompilerIntrinsicName(identifier.name))
                    error(identifier.location,
                          "identifier has no canonical LocalId or DeclarationRef");
            }
        };
    std::function<void(const Expr*, const BasicBlock&)> scanGraphExpr;
    scanGraphExpr = [this, &graph, &module, &localVisibleFrom,
                     &guardedCursorIds, &scanGraphIdentifier,
                     &scanGraphExpr](
        const Expr* expression, const BasicBlock& block) {
        if (!expression) return;
        if (dynamic_cast<const IntLiteralExpr*>(expression) ||
            dynamic_cast<const FloatLiteralExpr*>(expression) ||
            dynamic_cast<const StringLiteralExpr*>(expression) ||
            dynamic_cast<const BoolLiteralExpr*>(expression) ||
            dynamic_cast<const UnitExpr*>(expression))
            return;
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
                (!signature ||
                 (signature->kind != TypeKind::Function &&
                  signature->kind != TypeKind::Closure))) {
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
                            signature->parameterTypeIds[index]) {
                        // Integer literal arguments may carry an i32 type from
                        // Sema inference even when the parameter expects a
                        // different integer width (usize, i64, etc.). The
                        // structured backend relies on LLVM's implicit
                        // integer widening/narrowing; the canonical verifier
                        // must tolerate the same for integer-to-integer.
                        const auto* argType = call->args[index]
                            ? module.findType(call->args[index]->type) : nullptr;
                        const auto* sigType = module.findType(
                            signature->parameterTypeIds[index]);
                        const bool bothInteger = argType && sigType &&
                            (argType->kind == TypeKind::I8 ||
                             argType->kind == TypeKind::I16 ||
                             argType->kind == TypeKind::I32 ||
                             argType->kind == TypeKind::I64 ||
                             argType->kind == TypeKind::U8 ||
                             argType->kind == TypeKind::U16 ||
                             argType->kind == TypeKind::U32 ||
                             argType->kind == TypeKind::U64 ||
                             argType->kind == TypeKind::USize ||
                             argType->kind == TypeKind::ISize) &&
                            (sigType->kind == TypeKind::I8 ||
                             sigType->kind == TypeKind::I16 ||
                             sigType->kind == TypeKind::I32 ||
                             sigType->kind == TypeKind::I64 ||
                             sigType->kind == TypeKind::U8 ||
                             sigType->kind == TypeKind::U16 ||
                             sigType->kind == TypeKind::U32 ||
                             sigType->kind == TypeKind::U64 ||
                             sigType->kind == TypeKind::USize ||
                             sigType->kind == TypeKind::ISize);
                        // The structured backend tolerates String/CStr
                        // coercion (both are pointer types in LLVM). The
                        // canonical verifier must accept the same.
                        const bool stringCstrCoercion = argType && sigType &&
                            ((argType->kind == TypeKind::String &&
                              sigType->kind == TypeKind::CStr) ||
                             (argType->kind == TypeKind::CStr &&
                              sigType->kind == TypeKind::String));
                        if (!bothInteger && !stringCstrCoercion)
                            error(call->location,
                                  "call argument type disagrees with its signature");
                    }
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
        } else if (const auto* closure =
                       dynamic_cast<const MakeClosureExpr*>(expression)) {
            const auto* closureType = module.findType(closure->type);
            if (!closureType || closureType->kind != TypeKind::Closure)
                error(closure->location,
                      "sealed CFG closure construction has no Closure type");
            for (const auto& captured : closure->capturedValues)
                scanGraphExpr(captured.get(), block);
        } else if (const auto* envLoad =
                       dynamic_cast<const EnvLoadExpr*>(expression)) {
            const auto* local = graph.findLocal(envLoad->envLocal);
            if (!local) {
                error(envLoad->location,
                      "environment load references a missing local");
            } else {
                const auto* closureType = module.findType(local->type);
                if (local->kind != LocalKind::Parameter ||
                    !closureType || closureType->kind != TypeKind::Closure)
                    error(envLoad->location,
                          "environment load does not reference a Closure parameter");
                else if (envLoad->fieldIndex >=
                         closureType->capturedFields.size())
                    error(envLoad->location,
                          "environment load field is outside its closure environment");
                else if (envLoad->type !=
                         closureType->capturedFields[envLoad->fieldIndex].type)
                    error(envLoad->location,
                          "environment load type disagrees with its environment field");
                if (!localVisibleFrom(local->scope, block.scope))
                    error(envLoad->location,
                          "environment load references a local outside its lexical scope");
            }
        } else if (dynamic_cast<const TryExpr*>(expression) ||
                   dynamic_cast<const BlockExpr*>(expression) ||
                   dynamic_cast<const IfExpr*>(expression)) {
            error(expression->location,
                  "sealed CFG contains a nested control-flow expression");
        } else if (const auto* move =
                       dynamic_cast<const MoveExpr*>(expression)) {
            if (!move->nextUnread.empty()) {
                const auto* indexed = dynamic_cast<const IndexExpr*>(
                    move->operand.get());
                const auto* source = indexed
                    ? dynamic_cast<const IdentifierExpr*>(
                          indexed->object.get()) : nullptr;
                const auto* index = indexed
                    ? dynamic_cast<const IdentifierExpr*>(
                          indexed->index.get()) : nullptr;
                const auto* sourceLocal = source
                    ? graph.findLocal(source->local) : nullptr;
                const auto* indexLocal = index
                    ? graph.findLocal(index->local) : nullptr;
                const auto* cursor = graph.findLocal(move->nextUnread);
                const auto* sourceType = sourceLocal
                    ? module.findType(sourceLocal->type) : nullptr;
                size_t guardedElements = 0;
                bool matchingGuards = sourceLocal && sourceType && cursor &&
                    indexLocal && sourceType->kind == TypeKind::Array &&
                    sourceType->innerTypeId == move->type &&
                    indexLocal->id == move->nextUnread &&
                    cursor->type == indexLocal->type &&
                    (cursor->scope == sourceLocal->scope ||
                     localVisibleFrom(sourceLocal->scope, cursor->scope)) &&
                    localVisibleFrom(cursor->scope, block.scope);
                if (sourceLocal) {
                    for (const auto& cleanup : graph.cleanups) {
                        if (cleanup.place.root != sourceLocal->id) continue;
                        if (!cleanup.guard ||
                            cleanup.guard->nextUnread != cursor->id)
                            matchingGuards = false;
                        else
                            ++guardedElements;
                    }
                }
                if (!matchingGuards || !sourceType ||
                    guardedElements != sourceType->arrayLength)
                    error(move->location,
                          "array element transfer has no matching guarded tail state");
            }
            if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(
                    move->operand.get()))
                scanGraphIdentifier(*identifier, block, true);
            else
                scanGraphExpr(move->operand.get(), block);
        } else if (const auto* borrow =
                       dynamic_cast<const BorrowExpr*>(expression)) {
            if (borrow->isMutable) {
                const auto* identifier =
                    dynamic_cast<const IdentifierExpr*>(
                        borrow->operand.get());
                if (identifier && !identifier->local.empty() &&
                    guardedCursorIds.count(identifier->local.value))
                    error(borrow->location,
                          "guarded array cursor is mutably borrowed");
            }
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
            if (address->isMutable) {
                const auto* identifier =
                    dynamic_cast<const IdentifierExpr*>(
                        address->operand.get());
                if (identifier && !identifier->local.empty() &&
                    guardedCursorIds.count(identifier->local.value))
                    error(address->location,
                          "guarded array cursor address is mutable");
            }
            scanGraphExpr(address->operand.get(), block);
        } else if (const auto* assignment =
                       dynamic_cast<const AssignExpr*>(expression)) {
            const auto* assignedIdentifier =
                dynamic_cast<const IdentifierExpr*>(
                    assignment->lhs.get());
            if (assignedIdentifier &&
                !assignedIdentifier->local.empty() &&
                guardedCursorIds.count(
                    assignedIdentifier->local.value))
                error(assignment->location,
                      "guarded array cursor is assigned outside its element transfer");
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
    successors.assign(graph.blocks.size(), {});
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
                        local->usage != declaration->usage ||
                        !declaration->relation ||
                        local->relation != *declaration->relation)
                        error(declaration->location,
                              "let operation disagrees with its local-table row");
                    if (!declaration->initializer ||
                        declaration->initializer->type != declaration->type) {
                        // String literals are typed as String by Sema, but a
                        // cstr-annotated binding accepts them (both are pointer
                        // types in the LLVM backend). Allow this coercion.
                        const auto* initType = declaration->initializer
                            ? module.findType(declaration->initializer->type) : nullptr;
                        const auto* declType = module.findType(declaration->type);
                        const bool stringCstrCoercion = initType && declType &&
                            ((initType->kind == TypeKind::String &&
                              declType->kind == TypeKind::CStr) ||
                             (initType->kind == TypeKind::CStr &&
                              declType->kind == TypeKind::String));
                        if (!stringCstrCoercion)
                            error(declaration->location,
                                  "let initializer type disagrees with its canonical local");
                    }
                    if (guardedCursorIds.count(local->id.value)) {
                        const auto* zero =
                            dynamic_cast<const IntLiteralExpr*>(
                                declaration->initializer.get());
                        if (!zero || zero->value != 0)
                            error(declaration->location,
                                  "guarded array cursor is not initialized to zero");
                    }
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
                if (const auto* target = graph.findBlock(
                        block.terminator.primary.target)) {
                    const auto* sourceContinuation = enclosingRegion(
                        block.region, RegionKind::Continuation);
                    const auto* targetContinuation = enclosingRegion(
                        target->region, RegionKind::Continuation);
                    if (sourceContinuation &&
                        (!targetContinuation ||
                         targetContinuation->id != sourceContinuation->id)) {
                        if (sourceContinuation->exit.empty() ||
                            target->id != sourceContinuation->exit)
                            error(block.terminator.location,
                                  "jump escapes a continuation through a non-exit edge");
                    } else if (!sourceContinuation) {
                        const auto* sourceFragment = enclosingRegion(
                            block.region, RegionKind::Fragment);
                        const auto* targetFragment = enclosingRegion(
                            target->region, RegionKind::Fragment);
                        if (sourceFragment && targetContinuation) {
                            const auto* declaration = module.findDeclaration(
                                sourceFragment->fragment);
                            const auto* contract = declaration
                                ? module.findType(declaration->type) : nullptr;
                            const auto* fragmentApply = enclosingRegion(
                                sourceFragment->id, RegionKind::Apply);
                            const auto* continuationApply = enclosingRegion(
                                targetContinuation->id, RegionKind::Apply);
                            if (!contract ||
                                contract->continuationKind !=
                                    ContinuationKind::Interceptor ||
                                !fragmentApply || !continuationApply ||
                                fragmentApply->id != continuationApply->id)
                                error(block.terminator.location,
                                      "ordinary jump enters a continuation outside interceptor forwarding");
                        } else if (sourceFragment &&
                                   (!targetFragment ||
                                    targetFragment->id != sourceFragment->id) &&
                                   (sourceFragment->exit.empty() ||
                                    target->id != sourceFragment->exit)) {
                            error(block.terminator.location,
                                  "jump escapes a fragment through a non-exit edge");
                        }
                    }
                }
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
                if (const auto* fragment = enclosingRegion(
                        block.region, RegionKind::Fragment)) {
                    const auto* declaration = module.findDeclaration(
                        fragment->fragment);
                    const auto* fragmentType = declaration
                        ? module.findType(declaration->type) : nullptr;
                    const auto* target = graph.findBlock(
                        block.terminator.primary.target);
                    if (block.terminator.kind == TerminatorKind::Abort) {
                        if (fragment->exit.empty() ||
                            block.terminator.primary.target != fragment->exit)
                            error(block.terminator.location,
                                  "abort edge does not target its enclosing fragment exit");
                    } else if (target) {
                        if (!fragmentType ||
                            fragmentType->continuationKind !=
                                ContinuationKind::Context)
                            error(block.terminator.location,
                                  "resume terminator is not owned by a context fragment");
                        const auto* continuation = graph.findRegion(
                            target->region);
                        const auto* fragmentApply = enclosingRegion(
                            fragment->id, RegionKind::Apply);
                        const auto* continuationApply = continuation
                            ? enclosingRegion(
                                  continuation->id, RegionKind::Apply)
                            : nullptr;
                        if (!continuation ||
                            continuation->kind != RegionKind::Continuation ||
                            continuation->entry != target->id ||
                            !fragmentApply || !continuationApply ||
                            fragmentApply->id != continuationApply->id)
                            error(block.terminator.location,
                                  "resume edge does not enter the sibling continuation of its apply region");
                    }
                } else {
                    error(block.terminator.location,
                          block.terminator.kind == TerminatorKind::Resume
                              ? "resume terminator is outside a fragment region"
                              : "abort terminator is outside a fragment region");
                }
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
}

} // namespace moon
