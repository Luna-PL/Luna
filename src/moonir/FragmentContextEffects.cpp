#include "FragmentContextEffects.h"

#include <unordered_set>
#include <utility>
#include <vector>

namespace moon {
namespace {

struct FunctionEffectNode {
    const FunctionDecl* function = nullptr;
    bool reachesRuntimeSlot = false;
    std::unordered_set<std::string> directCallees;
};

void collectGraphEffects(
    const ControlFlowGraph& graph, FunctionEffectNode& node);

void addCallee(
    const DeclarationRef& reference, FunctionEffectNode& node) {
    if (reference.complete())
        node.directCallees.insert(fragmentContextEffectKey(reference));
}

void collectExpressionEffects(
    const Expr* expression, FunctionEffectNode& node) {
    if (!expression) return;
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(expression)) {
        collectExpressionEffects(binary->lhs.get(), node);
        collectExpressionEffects(binary->rhs.get(), node);
    } else if (const auto* unary =
                   dynamic_cast<const UnaryExpr*>(expression)) {
        collectExpressionEffects(unary->operand.get(), node);
    } else if (const auto* call = dynamic_cast<const CallExpr*>(expression)) {
        addCallee(call->calleeRef, node);
        if (call->calleeRef.empty()) {
            if (const auto* identifier =
                    dynamic_cast<const IdentifierExpr*>(call->callee.get()))
                addCallee(identifier->declaration, node);
        }
        addCallee(call->iteratorCollectBegin, node);
        addCallee(call->iteratorCollectPush, node);
        addCallee(call->iteratorCollectFinish, node);
        collectExpressionEffects(call->callee.get(), node);
        for (const auto& argument : call->args)
            collectExpressionEffects(argument.get(), node);
    } else if (const auto* launch =
                   dynamic_cast<const LaunchExpr*>(expression)) {
        collectExpressionEffects(launch->threads.get(), node);
        for (const auto& argument : launch->args)
            collectExpressionEffects(argument.get(), node);
    } else if (const auto* variant =
                   dynamic_cast<const VariantConstructExpr*>(expression)) {
        for (const auto& argument : variant->args)
            collectExpressionEffects(argument.get(), node);
    } else if (const auto* result =
                   dynamic_cast<const ResultConstructExpr*>(expression)) {
        collectExpressionEffects(result->payload.get(), node);
    } else if (const auto* field =
                   dynamic_cast<const FieldAccessExpr*>(expression)) {
        collectExpressionEffects(field->object.get(), node);
    } else if (const auto* index =
                   dynamic_cast<const IndexExpr*>(expression)) {
        collectExpressionEffects(index->object.get(), node);
        collectExpressionEffects(index->index.get(), node);
    } else if (const auto* length =
                   dynamic_cast<const SliceLengthExpr*>(expression)) {
        collectExpressionEffects(length->slice.get(), node);
    } else if (const auto* array =
                   dynamic_cast<const ArrayLiteralExpr*>(expression)) {
        for (const auto& element : array->elements)
            collectExpressionEffects(element.get(), node);
    } else if (const auto* record =
                   dynamic_cast<const RecordLiteralExpr*>(expression)) {
        for (const auto& field : record->fields)
            collectExpressionEffects(field.value.get(), node);
    } else if (const auto* allocation =
                   dynamic_cast<const HeapAllocExpr*>(expression)) {
        collectExpressionEffects(allocation->initializer.get(), node);
    } else if (const auto* allocation =
                   dynamic_cast<const InitAllocationExpr*>(expression)) {
        for (const auto& element : allocation->elements)
            collectExpressionEffects(element.value.get(), node);
    } else if (const auto* propagation =
                   dynamic_cast<const TryExpr*>(expression)) {
        addCallee(propagation->errorConversion, node);
        collectExpressionEffects(propagation->operand.get(), node);
    } else if (const auto* move =
                   dynamic_cast<const MoveExpr*>(expression)) {
        collectExpressionEffects(move->operand.get(), node);
    } else if (const auto* borrow =
                   dynamic_cast<const BorrowExpr*>(expression)) {
        collectExpressionEffects(borrow->operand.get(), node);
    } else if (const auto* dereference =
                   dynamic_cast<const DerefExpr*>(expression)) {
        collectExpressionEffects(dereference->operand.get(), node);
    } else if (const auto* address =
                   dynamic_cast<const AddrOfExpr*>(expression)) {
        collectExpressionEffects(address->operand.get(), node);
    } else if (const auto* block =
                   dynamic_cast<const BlockExpr*>(expression)) {
        // BlockExpr is construction-only and cannot survive sealing. It is
        // intentionally ignored here rather than becoming a second CFG walk.
        (void)block;
    } else if (const auto* condition =
                   dynamic_cast<const IfExpr*>(expression)) {
        collectExpressionEffects(condition->cond.get(), node);
        collectExpressionEffects(condition->thenExpr.get(), node);
        collectExpressionEffects(condition->elseExpr.get(), node);
    } else if (const auto* lambda =
                   dynamic_cast<const LambdaExpr*>(expression)) {
        // Until the context-aware indirect-call ABI is materialized, retain a
        // conservative dependency on any nested executable closure.
        if (lambda->controlFlow)
            collectGraphEffects(*lambda->controlFlow, node);
    } else if (const auto* closure =
                   dynamic_cast<const MakeClosureExpr*>(expression)) {
        collectExpressionEffects(closure->lambda.get(), node);
        for (const auto& value : closure->capturedValues)
            collectExpressionEffects(value.get(), node);
    } else if (const auto* assignment =
                   dynamic_cast<const AssignExpr*>(expression)) {
        collectExpressionEffects(assignment->lhs.get(), node);
        collectExpressionEffects(assignment->rhs.get(), node);
    }
}

void collectGraphEffects(
    const ControlFlowGraph& graph, FunctionEffectNode& node) {
    for (const auto& block : graph.blocks) {
        for (const auto& operation : block.operations) {
            if (const auto* let =
                    dynamic_cast<const LetStmt*>(operation.get())) {
                collectExpressionEffects(let->initializer.get(), node);
            } else if (const auto* statement =
                           dynamic_cast<const ExprStmt*>(operation.get())) {
                collectExpressionEffects(statement->expr.get(), node);
            } else if (const auto* release =
                           dynamic_cast<const FreeStmt*>(operation.get())) {
                collectExpressionEffects(release->operand.get(), node);
            } else if (const auto* await =
                           dynamic_cast<const AwaitStmt*>(operation.get())) {
                collectExpressionEffects(await->event.get(), node);
            }
        }
        if (block.terminator.kind == TerminatorKind::RuntimeSlot)
            node.reachesRuntimeSlot = true;
        collectExpressionEffects(block.terminator.operand.get(), node);
    }
}

void collectFunctions(
    const Module& module, std::vector<FunctionEffectNode>& nodes) {
    const auto add = [&](const FunctionDecl* function) {
        if (!function) return;
        FunctionEffectNode node;
        node.function = function;
        if (function->controlFlow)
            collectGraphEffects(*function->controlFlow, node);
        nodes.push_back(std::move(node));
    };
    for (const auto& declaration : module.declarations) {
        if (const auto* function =
                dynamic_cast<const FunctionDecl*>(declaration.get())) {
            add(function);
        } else if (const auto* implementation =
                       dynamic_cast<const ImplDecl*>(declaration.get())) {
            for (const auto& method : implementation->methods)
                add(method.get());
        }
    }
}

} // namespace

std::string fragmentContextEffectKey(const DeclarationRef& reference) {
    return reference.symbol.value + "\n" + reference.contract.value;
}

FragmentContextEffectMap computeFragmentContextEffects(const Module& module) {
    std::vector<FunctionEffectNode> nodes;
    collectFunctions(module, nodes);
    FragmentContextEffectMap effects;
    for (const auto& node : nodes) {
        const DeclarationRef reference{
            node.function->symbolId, node.function->contractId};
        effects[fragmentContextEffectKey(reference)] =
            node.reachesRuntimeSlot;
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& node : nodes) {
            const DeclarationRef reference{
                node.function->symbolId, node.function->contractId};
            auto& required = effects[fragmentContextEffectKey(reference)];
            if (required) continue;
            for (const auto& callee : node.directCallees) {
                const auto found = effects.find(callee);
                if (found != effects.end() && found->second) {
                    required = true;
                    changed = true;
                    break;
                }
            }
        }
    }
    return effects;
}

void inferFragmentContextEffects(Module& module) {
    const auto effects = computeFragmentContextEffects(module);
    const auto apply = [&](FunctionDecl* function) {
        if (!function) return;
        const DeclarationRef reference{
            function->symbolId, function->contractId};
        const auto found = effects.find(fragmentContextEffectKey(reference));
        function->requiresFragmentContext =
            found != effects.end() && found->second;
    };
    for (auto& declaration : module.declarations) {
        if (auto* function = dynamic_cast<FunctionDecl*>(declaration.get())) {
            apply(function);
        } else if (auto* implementation =
                       dynamic_cast<ImplDecl*>(declaration.get())) {
            for (auto& method : implementation->methods)
                apply(method.get());
        }
    }
}

} // namespace moon
