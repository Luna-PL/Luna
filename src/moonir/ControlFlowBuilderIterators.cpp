#include "ControlFlowBuilder.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <type_traits>
#include <unordered_set>

namespace moon {

bool ControlFlowBuilder::parseIteratorRecipe(
    std::unique_ptr<Expr> expression, IteratorRecipePlan& plan,
    const SourceLocation& location) {
    if (!expression) {
        error(location, "iterator recipe has no canonical expression");
        return false;
    }
    auto* call = dynamic_cast<CallExpr*>(expression.get());
    if (!call) {
        if (const auto* identifier =
                dynamic_cast<const IdentifierExpr*>(expression.get())) {
            if (const auto* materialized =
                    lookupMaterializedIterator(identifier->name)) {
                plan.mode = materialized->mode;
                plan.sourceType = materialized->sourceType;
                plan.itemType = materialized->itemType;
                plan.materialized = true;
                plan.materializedName = identifier->name;
                plan.materializedOwnsSource = materialized->ownsSource;
                plan.materializedSource = materialized->source;
                plan.materializedIndex = materialized->index;
                plan.materializedLimit = materialized->limit;
                for (const auto& step : materialized->steps) {
                    IteratorRecipeStep copied;
                    copied.op = step.op;
                    copied.argumentLocal = step.argument;
                    copied.inputType = step.inputType;
                    copied.outputType = step.outputType;
                    plan.steps.push_back(std::move(copied));
                }
                return true;
            }
        }
        // Resolve the source type from the canonical local table when the
        // MoonIR identifier lacks an inline type (lowering does not always
        // set it). This lets a direct `for v in array_binding` enter the
        // canonical recipe path without hitting the materialized-recipe gate.
        if (const auto* identifier =
                dynamic_cast<const IdentifierExpr*>(expression.get())) {
            if (expression->type.empty() && !identifier->name.empty()) {
                const LocalId local = lookupLocal(identifier->name);
                if (!local.empty() && local.value < mGraph->locals.size())
                    expression->type = mGraph->locals[local.value].type;
            }
        }
        const auto* sourceType = mModule->findType(expression->type);
        if (!sourceType ||
            (sourceType->kind != TypeKind::Array &&
             sourceType->kind != TypeKind::Slice)) {
            error(location,
                  "materialized iterator recipes require the later canonical subphase");
            return false;
        }
        plan.sourceType = expression->type;
        plan.source = std::move(expression);
        plan.mode = sourceType->kind == TypeKind::Slice
            ? IteratorMode::Shared : IteratorMode::Consuming;
        plan.itemType = sourceType->kind == TypeKind::Slice
            ? TypeRef{} : sourceType->innerTypeId;
        return true;
    }

    if (call->iteratorOp == IteratorOp::Range) {
        if (call->args.size() != 2 ||
            call->iteratorInputType.empty() ||
            call->iteratorOutputType.empty()) {
            error(location, "range recipe has no canonical start/end contract");
            return false;
        }
        plan.mode = IteratorMode::Range;
        plan.rangeStart = std::move(call->args[0]);
        plan.rangeEnd = std::move(call->args[1]);
        plan.itemType = call->iteratorOutputType;
        return true;
    }

    std::function<TypeRef(const Expr*)> sourceTypeOf =
        [this, &sourceTypeOf](const Expr* expression) -> TypeRef {
        if (!expression) return {};
        if (!expression->type.empty()) return expression->type;
        if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(
                expression)) {
            const LocalId local = lookupLocal(identifier->name);
            if (!local.empty() && local.value < mGraph->locals.size())
                return mGraph->locals[local.value].type;
        }
        if (const auto* move = dynamic_cast<const MoveExpr*>(expression))
            return sourceTypeOf(move->operand.get());
        if (const auto* borrow = dynamic_cast<const BorrowExpr*>(expression))
            return sourceTypeOf(borrow->operand.get());
        return {};
    };

    auto* member = dynamic_cast<FieldAccessExpr*>(call->callee.get());
    if (!member) {
        error(location, "iterator recipe call has no canonical receiver");
        return false;
    }
    if (call->iteratorOp == IteratorOp::Iter ||
        call->iteratorOp == IteratorOp::IterMut ||
        call->iteratorOp == IteratorOp::IntoIter) {
        if (!call->args.empty() || !member->object) {
            error(location, "iterator source adapter has an invalid canonical shape");
            return false;
        }
        plan.sourceType = sourceTypeOf(member->object.get());
        plan.source = std::move(member->object);
        plan.mode = call->iteratorOp == IteratorOp::Iter
            ? IteratorMode::Shared
            : (call->iteratorOp == IteratorOp::IterMut
                ? IteratorMode::Mutable : IteratorMode::Consuming);
        plan.itemType = call->iteratorOutputType;
        return true;
    }
    if (call->iteratorOp == IteratorOp::Take) {
        if (call->args.size() != 1 || !member->object) {
            error(location, "iterator take adapter has an invalid canonical shape");
            return false;
        }
        auto receiver = std::move(member->object);
        auto argument = std::move(call->args.front());
        const TypeRef inputType = call->iteratorInputType;
        const TypeRef outputType = call->iteratorOutputType;
        if (!parseIteratorRecipe(std::move(receiver), plan, location))
            return false;
        if (inputType != plan.itemType || outputType != inputType) {
            error(location,
                  "iterator take adapter disagrees with its canonical item type");
            return false;
        }
        plan.steps.push_back({
            IteratorOp::Take, std::move(argument), {},
            inputType, outputType});
        plan.itemType = outputType;
        return true;
    }
    if (call->iteratorOp == IteratorOp::Map ||
        call->iteratorOp == IteratorOp::Filter) {
        if (call->args.size() != 1 || !member->object) {
            error(location,
                  "iterator map/filter adapter has an invalid canonical shape");
            return false;
        }
        auto receiver = std::move(member->object);
        auto callable = std::move(call->args.front());
        const TypeRef inputType = call->iteratorInputType;
        const TypeRef outputType = call->iteratorOutputType;
        if (!parseIteratorRecipe(std::move(receiver), plan, location))
            return false;
        if (inputType != plan.itemType || outputType.empty()) {
            error(location,
                  "iterator map/filter adapter disagrees with its canonical item type");
            return false;
        }
        const auto* closure = mModule->findType(callable->type);
        TypeRef expectedResult = outputType;
        if (call->iteratorOp == IteratorOp::Filter)
            for (const auto& type : mModule->typeTable)
                if (type.kind == TypeKind::Bool) expectedResult = type.id;
        if (!closure ||
            (closure->kind != TypeKind::Function &&
             closure->kind != TypeKind::Closure) ||
            closure->parameterTypeIds != TypeRefVec{inputType} ||
            closure->returnTypeId != expectedResult ||
            (call->iteratorOp == IteratorOp::Filter &&
             outputType != inputType)) {
            error(location,
                  "iterator map/filter requires one canonical callable");
            return false;
        }
        plan.steps.push_back({call->iteratorOp, std::move(callable), {},
                              inputType, outputType});
        plan.itemType = outputType;
        return true;
    }
    error(location, "unsupported compiler iterator recipe operation");
    return false;
}

bool ControlFlowBuilder::bindIteratorRecipe(IteratorRecipePlan& plan) {
    const bool previousRecipeAllowance = mBindingIteratorRecipe;
    mBindingIteratorRecipe = true;
    bool valid = true;
    if (!plan.materialized) {
        if (plan.mode == IteratorMode::Range) {
            valid = bindExpr(plan.rangeStart.get()) &&
                bindExpr(plan.rangeEnd.get());
        } else {
            valid = bindExpr(plan.source.get());
        }
    }
    for (auto& step : plan.steps)
        if (step.argument && !bindExpr(step.argument.get())) valid = false;
    mBindingIteratorRecipe = previousRecipeAllowance;
    return valid;
}

bool ControlFlowBuilder::validateIteratorRecipe(
    IteratorRecipePlan& plan, const TypeRef& expectedItem,
    const SourceLocation& location, bool allowAffineItems) {
    TypeRef indexType;
    TypeRef sizeType;
    for (const auto& type : mModule->typeTable) {
        if (type.kind == TypeKind::I32) indexType = type.id;
        if (type.kind == TypeKind::USize) sizeType = type.id;
    }
    if (indexType.empty()) {
        error(location, "iterator recipe requires a frozen i32 compiler type");
        return false;
    }
    if (plan.mode == IteratorMode::Range) {
        if (plan.materialized) {
            const auto* initial = mGraph->findLocal(plan.materializedIndex);
            const auto* limit = mGraph->findLocal(plan.materializedLimit);
            if (!initial || !limit || initial->type != indexType ||
                limit->type != indexType) {
                error(location,
                      "materialized range has no canonical i32 cursor state");
                return false;
            }
        } else if (!plan.rangeStart || !plan.rangeEnd ||
                   plan.rangeStart->type != indexType ||
                   plan.rangeEnd->type != indexType) {
            error(location,
                  "range recipe must be normalized to i32 start/end/item values");
            return false;
        }
    } else {
        const auto* sourceType = mModule->findType(plan.sourceType);
        if (!sourceType ||
            (sourceType->kind != TypeKind::Array &&
             sourceType->kind != TypeKind::Slice) ||
            sourceType->innerTypeId.empty()) {
            error(location,
                  "iterator recipe source is not a frozen array or slice");
            return false;
        }
        if (sourceType->kind == TypeKind::Slice &&
            plan.mode != IteratorMode::Shared) {
            error(location, "read-only slice recipes require shared iteration");
            return false;
        }
        if (sourceType->kind == TypeKind::Slice && sizeType.empty()) {
            error(location, "slice recipe requires a frozen usize compiler type");
            return false;
        }
        const auto* elementType = mModule->findType(sourceType->innerTypeId);
        if (sourceType->kind == TypeKind::Array &&
            plan.mode == IteratorMode::Consuming &&
            (!elementType ||
             (elementType->sysmeta.resource.usage !=
                  luna::ownership::Usage::Copy &&
              (!allowAffineItems ||
               (plan.materialized && !plan.materializedOwnsSource) ||
               elementType->sysmeta.resource.usage !=
                   luna::ownership::Usage::Affine ||
               !elementType->sysmeta.resource.cleanupRequired)))) {
            error(location,
                  "move-only consuming arrays require projected canonical cleanup state");
            return false;
        }
        if (sourceType->kind == TypeKind::Slice && plan.itemType.empty())
            plan.itemType = expectedItem;
        if (plan.materialized) {
            const auto* source = mGraph->findLocal(plan.materializedSource);
            if (!source || source->type != plan.sourceType) {
                error(location,
                      "materialized iterator has no canonical source local");
                return false;
            }
        }
    }

    const TypeRef sourceItemType = plan.steps.empty()
        ? plan.itemType : plan.steps.front().inputType;
    if (plan.mode == IteratorMode::Range) {
        if (sourceItemType != indexType) {
            error(location, "range recipe adapter input is not canonical i32");
            return false;
        }
    } else {
        const auto* frozenSource = mModule->findType(plan.sourceType);
        const auto* frozenItem = mModule->findType(sourceItemType);
        const bool consumingItem = frozenSource &&
            sourceItemType == frozenSource->innerTypeId;
        const bool borrowedItem = frozenSource && frozenItem &&
            frozenItem->kind == TypeKind::Reference &&
            frozenItem->innerTypeId == frozenSource->innerTypeId &&
            frozenItem->isMutable == (plan.mode == IteratorMode::Mutable);
        if ((plan.mode == IteratorMode::Consuming && !consumingItem) ||
            ((plan.mode == IteratorMode::Shared ||
              plan.mode == IteratorMode::Mutable) && !borrowedItem)) {
            error(location,
                  "iterator recipe source mode disagrees with its first item type");
            return false;
        }
    }
    if (plan.itemType != expectedItem) {
        error(location,
              "iterator recipe item type disagrees with its consumer binding");
        return false;
    }

    TypeRef currentItemType = sourceItemType;
    for (const auto& step : plan.steps) {
        const auto* argumentLocal = mGraph->findLocal(step.argumentLocal);
        const TypeRef argumentType = step.argument
            ? step.argument->type
            : (argumentLocal ? argumentLocal->type : TypeRef{});
        if ((step.argument != nullptr) == !step.argumentLocal.empty()) {
            error(location,
                  "iterator adapter has ambiguous materialized argument state");
            return false;
        }
        if (step.op == IteratorOp::Take) {
            const auto* input = mModule->findType(step.inputType);
            if (argumentType != indexType || !input ||
                step.inputType != currentItemType ||
                step.outputType != step.inputType ||
                (input->sysmeta.resource.usage !=
                     luna::ownership::Usage::Copy &&
                 (!allowAffineItems ||
                  input->sysmeta.resource.usage !=
                      luna::ownership::Usage::Affine))) {
                error(location,
                      "iterator take requires canonical item and i32 count state");
                return false;
            }
            currentItemType = step.outputType;
            continue;
        }
        if (step.op != IteratorOp::Map && step.op != IteratorOp::Filter) {
            error(location,
                  "iterator recipe contains an unsupported canonical adapter");
            return false;
        }
        const auto* input = mModule->findType(step.inputType);
        const auto* output = mModule->findType(step.outputType);
        const auto* callable = mModule->findType(argumentType);
        const auto* lambda = step.argument
            ? dynamic_cast<const LambdaExpr*>(step.argument.get()) : nullptr;
        // A capture-free callable is a Function type. A Copy-capture closure
        // is a Closure type whose environment fields are all Copy. Both are
        // acceptable as canonical map/filter adapters (C016 CL010 extends
        // closure capture to iterator recipes).
        const bool copyCaptureClosure = lambda && callable &&
            callable->kind == TypeKind::Closure &&
            !lambda->captures.empty() &&
            argumentType == lambda->closureType;
        const bool commonCallable = input && output && callable &&
            step.inputType == currentItemType &&
            (callable->kind == TypeKind::Function ||
             callable->kind == TypeKind::Closure) &&
            (!lambda ||
             (!lambda->body && lambda->controlFlow &&
              argumentType == lambda->closureType &&
              (lambda->captures.empty() || copyCaptureClosure))) &&
            callable->parameterContracts.size() == 1;
        const bool copyPath = commonCallable &&
            callable->parameterContracts.front().usage ==
                luna::ownership::Usage::Copy &&
            input->sysmeta.resource.usage ==
                luna::ownership::Usage::Copy &&
            output->sysmeta.resource.usage ==
                luna::ownership::Usage::Copy &&
            callable->returnContract.usage ==
                luna::ownership::Usage::Copy;
        const bool copyToAffineMap = allowAffineItems && commonCallable &&
            step.op == IteratorOp::Map &&
            callable->parameterContracts.front().usage ==
                luna::ownership::Usage::Copy &&
            input->sysmeta.resource.usage ==
                luna::ownership::Usage::Copy &&
            output->sysmeta.resource.usage ==
                luna::ownership::Usage::Affine &&
            callable->returnContract.relation ==
                luna::ownership::Relation::Owned &&
            callable->returnContract.usage ==
                luna::ownership::Usage::Affine;
        const bool affineMapOutput = output && callable &&
            ((output->sysmeta.resource.usage ==
                  luna::ownership::Usage::Copy &&
              callable->returnContract.usage ==
                  luna::ownership::Usage::Copy) ||
             (output->sysmeta.resource.usage ==
                  luna::ownership::Usage::Affine &&
              callable->returnContract.relation ==
                  luna::ownership::Relation::Owned &&
              callable->returnContract.usage ==
                  luna::ownership::Usage::Affine));
        const bool affineOwningMap = allowAffineItems &&
            commonCallable && step.op == IteratorOp::Map &&
            input->sysmeta.resource.usage ==
                luna::ownership::Usage::Affine &&
            callable->parameterContracts.front().relation ==
                luna::ownership::Relation::Owned &&
            callable->parameterContracts.front().usage ==
                luna::ownership::Usage::Affine &&
            affineMapOutput;
        const bool affineFilter = allowAffineItems && commonCallable &&
            step.op == IteratorOp::Filter &&
            step.outputType == step.inputType &&
            input->sysmeta.resource.usage ==
                luna::ownership::Usage::Affine &&
            output->sysmeta.resource.usage ==
                luna::ownership::Usage::Affine &&
            callable->parameterContracts.front().relation ==
                luna::ownership::Relation::SharedBorrow &&
            callable->parameterContracts.front().usage ==
                luna::ownership::Usage::Copy &&
            callable->returnContract.usage ==
                luna::ownership::Usage::Copy;
        if (!copyPath && !copyToAffineMap &&
            !affineOwningMap && !affineFilter) {
            error(location,
                  "non-Copy map/filter item or callable contract requires "
                  "canonical per-item ownership state");
            return false;
        }
        currentItemType = step.outputType;
    }
    return true;
}


} // namespace moon
