#include "ControlFlowBuilder.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <type_traits>
#include <unordered_set>

namespace moon {
namespace {

bool isTraversalLeaf(const Expr* expression) {
    return dynamic_cast<const IntLiteralExpr*>(expression) ||
        dynamic_cast<const FloatLiteralExpr*>(expression) ||
        dynamic_cast<const StringLiteralExpr*>(expression) ||
        dynamic_cast<const BoolLiteralExpr*>(expression) ||
        dynamic_cast<const UnitExpr*>(expression) ||
        dynamic_cast<const IdentifierExpr*>(expression) ||
        dynamic_cast<const EnvLoadExpr*>(expression);
}

} // namespace

bool ControlFlowBuilder::containsIteratorTerminal(
    const Expr* expression) const {
    if (!expression) return false;
    if (isTraversalLeaf(expression)) return false;
    if (const auto* call = dynamic_cast<const CallExpr*>(expression)) {
        if (call->iteratorOp == IteratorOp::Fold ||
            call->iteratorOp == IteratorOp::ForEach ||
            call->iteratorOp == IteratorOp::Count ||
            call->iteratorOp == IteratorOp::Collect)
            return true;
        if (containsIteratorTerminal(call->callee.get())) return true;
        for (const auto& argument : call->args)
            if (containsIteratorTerminal(argument.get())) return true;
        return false;
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(expression))
        return containsIteratorTerminal(binary->lhs.get()) ||
            containsIteratorTerminal(binary->rhs.get());
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(expression))
        return containsIteratorTerminal(unary->operand.get());
    if (const auto* launch = dynamic_cast<const LaunchExpr*>(expression)) {
        if (containsIteratorTerminal(launch->threads.get())) return true;
        for (const auto& argument : launch->args)
            if (containsIteratorTerminal(argument.get())) return true;
        return false;
    }
    if (const auto* variant =
            dynamic_cast<const VariantConstructExpr*>(expression)) {
        for (const auto& argument : variant->args)
            if (containsIteratorTerminal(argument.get())) return true;
        return false;
    }
    if (const auto* result =
            dynamic_cast<const ResultConstructExpr*>(expression))
        return containsIteratorTerminal(result->payload.get());
    if (const auto* field =
            dynamic_cast<const FieldAccessExpr*>(expression))
        return containsIteratorTerminal(field->object.get());
    if (const auto* index = dynamic_cast<const IndexExpr*>(expression))
        return containsIteratorTerminal(index->object.get()) ||
            containsIteratorTerminal(index->index.get());
    if (const auto* length =
            dynamic_cast<const SliceLengthExpr*>(expression))
        return containsIteratorTerminal(length->slice.get());
    if (const auto* array =
            dynamic_cast<const ArrayLiteralExpr*>(expression)) {
        for (const auto& element : array->elements)
            if (containsIteratorTerminal(element.get())) return true;
        return false;
    }
    if (const auto* record =
            dynamic_cast<const RecordLiteralExpr*>(expression)) {
        for (const auto& field : record->fields)
            if (containsIteratorTerminal(field.value.get())) return true;
        return false;
    }
    if (const auto* allocation =
            dynamic_cast<const HeapAllocExpr*>(expression))
        return containsIteratorTerminal(allocation->initializer.get());
    if (const auto* move = dynamic_cast<const MoveExpr*>(expression))
        return containsIteratorTerminal(move->operand.get());
    if (const auto* borrow = dynamic_cast<const BorrowExpr*>(expression))
        return containsIteratorTerminal(borrow->operand.get());
    if (const auto* dereference =
            dynamic_cast<const DerefExpr*>(expression))
        return containsIteratorTerminal(dereference->operand.get());
    if (const auto* address = dynamic_cast<const AddrOfExpr*>(expression))
        return containsIteratorTerminal(address->operand.get());
    if (const auto* assignment =
            dynamic_cast<const AssignExpr*>(expression))
        return containsIteratorTerminal(assignment->lhs.get()) ||
            containsIteratorTerminal(assignment->rhs.get());
    return false;
}

bool ControlFlowBuilder::containsPendingControlFlow(
    const Expr* expression) const {
    if (!expression) return false;
    if (isTraversalLeaf(expression)) return false;
    if (dynamic_cast<const TryExpr*>(expression) ||
        dynamic_cast<const BlockExpr*>(expression) ||
        dynamic_cast<const IfExpr*>(expression))
        return true;
    if (const auto* call = dynamic_cast<const CallExpr*>(expression)) {
        if (call->iteratorOp == IteratorOp::Fold ||
            call->iteratorOp == IteratorOp::ForEach ||
            call->iteratorOp == IteratorOp::Count ||
            call->iteratorOp == IteratorOp::Collect)
            return true;
        if (containsPendingControlFlow(call->callee.get())) return true;
        for (const auto& argument : call->args)
            if (containsPendingControlFlow(argument.get())) return true;
        return false;
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(expression)) {
        if (binary->op == Operator::LogicalAnd ||
            binary->op == Operator::LogicalOr)
            return true;
        return containsPendingControlFlow(binary->lhs.get()) ||
            containsPendingControlFlow(binary->rhs.get());
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(expression))
        return containsPendingControlFlow(unary->operand.get());
    if (const auto* launch = dynamic_cast<const LaunchExpr*>(expression)) {
        if (containsPendingControlFlow(launch->threads.get())) return true;
        for (const auto& argument : launch->args)
            if (containsPendingControlFlow(argument.get())) return true;
        return false;
    }
    if (const auto* variant =
            dynamic_cast<const VariantConstructExpr*>(expression)) {
        for (const auto& argument : variant->args)
            if (containsPendingControlFlow(argument.get())) return true;
        return false;
    }
    if (const auto* result =
            dynamic_cast<const ResultConstructExpr*>(expression))
        return containsPendingControlFlow(result->payload.get());
    if (const auto* field = dynamic_cast<const FieldAccessExpr*>(expression))
        return containsPendingControlFlow(field->object.get());
    if (const auto* index = dynamic_cast<const IndexExpr*>(expression))
        return containsPendingControlFlow(index->object.get()) ||
            containsPendingControlFlow(index->index.get());
    if (const auto* length = dynamic_cast<const SliceLengthExpr*>(expression))
        return containsPendingControlFlow(length->slice.get());
    if (const auto* array = dynamic_cast<const ArrayLiteralExpr*>(expression)) {
        for (const auto& element : array->elements)
            if (containsPendingControlFlow(element.get())) return true;
        return false;
    }
    if (const auto* record = dynamic_cast<const RecordLiteralExpr*>(expression)) {
        const auto* type = mModule ? mModule->findType(record->type) : nullptr;
        if (type && type->kind == TypeKind::Struct) return true;
        for (const auto& field : record->fields)
            if (containsPendingControlFlow(field.value.get())) return true;
        return false;
    }
    if (dynamic_cast<const HeapAllocExpr*>(expression)) return true;
    if (const auto* move = dynamic_cast<const MoveExpr*>(expression))
        return containsPendingControlFlow(move->operand.get());
    if (const auto* borrow = dynamic_cast<const BorrowExpr*>(expression))
        return containsPendingControlFlow(borrow->operand.get());
    if (const auto* dereference = dynamic_cast<const DerefExpr*>(expression))
        return containsPendingControlFlow(dereference->operand.get());
    if (const auto* address = dynamic_cast<const AddrOfExpr*>(expression))
        return containsPendingControlFlow(address->operand.get());
    if (const auto* assignment = dynamic_cast<const AssignExpr*>(expression))
        return containsPendingControlFlow(assignment->lhs.get()) ||
            containsPendingControlFlow(assignment->rhs.get());
    return false;
}

bool ControlFlowBuilder::containsPotentialEarlyExit(
    const Expr* expression) const {
    if (!expression) return false;
    if (isTraversalLeaf(expression)) return false;
    if (dynamic_cast<const TryExpr*>(expression) ||
        dynamic_cast<const BlockExpr*>(expression) ||
        dynamic_cast<const IfExpr*>(expression))
        return true;
    if (const auto* call = dynamic_cast<const CallExpr*>(expression)) {
        if (containsPotentialEarlyExit(call->callee.get())) return true;
        for (const auto& argument : call->args)
            if (containsPotentialEarlyExit(argument.get())) return true;
        return false;
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(expression))
        return containsPotentialEarlyExit(binary->lhs.get()) ||
            containsPotentialEarlyExit(binary->rhs.get());
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(expression))
        return containsPotentialEarlyExit(unary->operand.get());
    if (const auto* launch = dynamic_cast<const LaunchExpr*>(expression)) {
        if (containsPotentialEarlyExit(launch->threads.get())) return true;
        for (const auto& argument : launch->args)
            if (containsPotentialEarlyExit(argument.get())) return true;
        return false;
    }
    if (const auto* variant =
            dynamic_cast<const VariantConstructExpr*>(expression)) {
        for (const auto& argument : variant->args)
            if (containsPotentialEarlyExit(argument.get())) return true;
        return false;
    }
    if (const auto* result =
            dynamic_cast<const ResultConstructExpr*>(expression))
        return containsPotentialEarlyExit(result->payload.get());
    if (const auto* field = dynamic_cast<const FieldAccessExpr*>(expression))
        return containsPotentialEarlyExit(field->object.get());
    if (const auto* index = dynamic_cast<const IndexExpr*>(expression))
        return containsPotentialEarlyExit(index->object.get()) ||
            containsPotentialEarlyExit(index->index.get());
    if (const auto* length = dynamic_cast<const SliceLengthExpr*>(expression))
        return containsPotentialEarlyExit(length->slice.get());
    if (const auto* array = dynamic_cast<const ArrayLiteralExpr*>(expression)) {
        for (const auto& element : array->elements)
            if (containsPotentialEarlyExit(element.get())) return true;
        return false;
    }
    if (const auto* record = dynamic_cast<const RecordLiteralExpr*>(expression)) {
        for (const auto& field : record->fields)
            if (containsPotentialEarlyExit(field.value.get())) return true;
        return false;
    }
    if (const auto* allocation = dynamic_cast<const HeapAllocExpr*>(expression))
        return containsPotentialEarlyExit(allocation->initializer.get());
    if (const auto* move = dynamic_cast<const MoveExpr*>(expression))
        return containsPotentialEarlyExit(move->operand.get());
    if (const auto* borrow = dynamic_cast<const BorrowExpr*>(expression))
        return containsPotentialEarlyExit(borrow->operand.get());
    if (const auto* dereference = dynamic_cast<const DerefExpr*>(expression))
        return containsPotentialEarlyExit(dereference->operand.get());
    if (const auto* address = dynamic_cast<const AddrOfExpr*>(expression))
        return containsPotentialEarlyExit(address->operand.get());
    if (const auto* assignment = dynamic_cast<const AssignExpr*>(expression))
        return containsPotentialEarlyExit(assignment->lhs.get()) ||
            containsPotentialEarlyExit(assignment->rhs.get());
    return false;
}

bool ControlFlowBuilder::hoistOrderedOperand(
    std::unique_ptr<Expr>& expression, OpenBlock& current,
    ScopeId scope, bool allowLinear) {
    if (!expression) return true;
    if (dynamic_cast<IntLiteralExpr*>(expression.get()) ||
        dynamic_cast<FloatLiteralExpr*>(expression.get()) ||
        dynamic_cast<StringLiteralExpr*>(expression.get()) ||
        dynamic_cast<BoolLiteralExpr*>(expression.get()) ||
        dynamic_cast<UnitExpr*>(expression.get()))
        return true;

    // Compiler intrinsic identifiers (print, panic, slice, etc.) have no
    // declaration table row. Skip hoisting for bare intrinsic identifiers
    // so they are not rejected as unresolved by bindExpr.
    if (const auto* id = dynamic_cast<const IdentifierExpr*>(expression.get())) {
        if (id->declaration.empty() && isCompilerIntrinsicName(id->name))
            return true;
    }

    if (!bindExpr(expression.get())) return false;
    if (const auto* identifier =
            dynamic_cast<const IdentifierExpr*>(expression.get())) {
        if (!identifier->declaration.empty()) return true;
        const auto* local = mGraph->findLocal(identifier->local);
        if (local && local->kind == LocalKind::Synthetic) return true;
    }
    if (const auto* transfer =
            dynamic_cast<const MoveExpr*>(expression.get())) {
        const auto* identifier = dynamic_cast<const IdentifierExpr*>(
            transfer->operand.get());
        const auto* local = identifier
            ? mGraph->findLocal(identifier->local) : nullptr;
        if (local && local->kind == LocalKind::Synthetic) return true;
    }

    const auto* type = mModule->findType(expression->type);
    if (!type) {
        error(expression->location,
              "expression sibling has no frozen type for CFG hoisting");
        return false;
    }
    auto usage = type->sysmeta.resource.usage;
    bool explicitFreshTransfer = false;
    if (const auto* call = dynamic_cast<const CallExpr*>(expression.get())) {
        usage = luna::ownership::strongerUsage(
            usage, call->returnUsage);
        explicitFreshTransfer = luna::ownership::isMoveOnly(
            call->returnUsage);
    } else if (dynamic_cast<const InitAllocationExpr*>(expression.get())) {
        usage = luna::ownership::strongerUsage(
            usage, luna::ownership::Usage::Affine);
        explicitFreshTransfer = true;
    } else if (const auto* transfer =
                   dynamic_cast<const MoveExpr*>(expression.get())) {
        explicitFreshTransfer = true;
        if (const auto* identifier =
                dynamic_cast<const IdentifierExpr*>(
                    transfer->operand.get())) {
            if (const auto* local = mGraph->findLocal(identifier->local))
                usage = luna::ownership::strongerUsage(
                    usage, local->usage);
        }
    }
    if (type->kind == TypeKind::Unit) {
        const auto location = expression->location;
        auto statement = std::make_unique<ExprStmt>();
        statement->location = location;
        statement->expr = std::move(expression);
        mGraph->blocks[current.block.value].operations.push_back(
            std::move(statement));

        auto unit = std::make_unique<UnitExpr>();
        unit->location = location;
        unit->type = type->id;
        expression = std::move(unit);
        return true;
    }
    if (usage == luna::ownership::Usage::Linear && !allowLinear) {
        error(expression->location,
              "linear expression sibling may cross an early-exit CFG path");
        return false;
    }
    if (luna::ownership::isMoveOnly(usage) &&
        !explicitFreshTransfer) {
        error(expression->location,
              "move-only expression sibling hoisting requires an explicit transfer");
        return false;
    }

    const std::string name =
        "$expression.hoist." + std::to_string(mExpressionCounter++);
    const LocalId local = addLocal(
        scope, LocalKind::Synthetic, name, expression->type,
        usage);
    if (local.empty()) return false;
    if (dynamic_cast<InitAllocationExpr*>(expression.get()) &&
        !type->sysmeta.resource.cleanupRequired)
        addCleanup(
            local, expression->type,
            luna::ownership::CleanupAction::Deallocate,
            CleanupKind::Allocation);

    auto declaration = std::make_unique<LetStmt>();
    declaration->location = expression->location;
    declaration->name = name;
    declaration->local = local;
    declaration->isLinear =
        usage == luna::ownership::Usage::Linear;
    declaration->usage = usage;
    declaration->relation = mGraph->locals[local.value].relation;
    declaration->type = expression->type;
    const auto expressionLocation = expression->location;
    declaration->initializer = std::move(expression);
    mGraph->blocks[current.block.value].operations.push_back(
        std::move(declaration));
    if (type->sysmeta.resource.cleanupRequired) {
        const auto cleanup = mCleanupByLocal.find(local.value);
        if (cleanup == mCleanupByLocal.end()) {
            error(expressionLocation,
                  "cleanup-bearing expression sibling has no canonical cleanup row");
            return false;
        }
        mActiveExpressionCleanups.push_back(cleanup->second);
    }

    auto identifier = std::make_unique<IdentifierExpr>();
    identifier->location =
        mGraph->blocks[current.block.value].location;
    identifier->name = name;
    identifier->local = local;
    identifier->type = type->id;
    if (luna::ownership::isMoveOnly(usage)) {
        auto transfer = std::make_unique<MoveExpr>();
        transfer->location = identifier->location;
        transfer->type = type->id;
        transfer->operand = std::move(identifier);
        expression = std::move(transfer);
    } else {
        expression = std::move(identifier);
    }
    return true;
}

} // namespace moon
