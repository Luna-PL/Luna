#include "ControlFlowBuilder.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <type_traits>
#include <unordered_set>

namespace moon {

std::optional<ControlFlowBuilder::OpenBlock>
ControlFlowBuilder::lowerShortCircuitExpression(
    std::unique_ptr<BinaryExpr> expression, OpenBlock current,
    RegionId region, ScopeId scope, std::unique_ptr<Expr>& replacement) {
    if (!expression || !expression->lhs || !expression->rhs ||
        (expression->op != Operator::LogicalAnd &&
         expression->op != Operator::LogicalOr)) {
        error(expression ? expression->location : SourceLocation{},
              "short-circuit expression has an invalid canonical shape");
        return std::nullopt;
    }
    const auto* resultType = mModule->findType(expression->type);
    if (!resultType || resultType->kind != TypeKind::Bool) {
        error(expression->location,
              "short-circuit expression must have frozen bool operands");
        return std::nullopt;
    }

    auto lhs = normalizeControlFlowExpression(
        expression->lhs, std::move(current), region, scope, false);
    if (!lhs) return std::nullopt;
    current = std::move(*lhs);
    if (!bindExpr(expression->lhs.get())) return std::nullopt;
    if (expression->lhs->type != expression->type) {
        error(expression->lhs->location,
              "short-circuit expression must have frozen bool operands");
        return std::nullopt;
    }

    const std::string name =
        "$short-circuit." + std::to_string(mExpressionCounter++);
    const LocalId result = addLocal(
        scope, LocalKind::Synthetic, name, expression->type,
        luna::ownership::Usage::Copy);
    if (result.empty()) return std::nullopt;

    auto initial = std::make_unique<BoolLiteralExpr>();
    initial->location = expression->location;
    initial->value = expression->op == Operator::LogicalOr;
    initial->type = expression->type;
    auto declaration = std::make_unique<LetStmt>();
    declaration->location = expression->location;
    declaration->name = name;
    declaration->local = result;
    declaration->usage = luna::ownership::Usage::Copy;
    declaration->relation = mGraph->locals[result.value].relation;
    declaration->type = expression->type;
    declaration->initializer = std::move(initial);
    mGraph->blocks[current.block.value].operations.push_back(
        std::move(declaration));

    const BlockId rhsEntry = addBlock(
        region, scope, expression->rhs->location);
    const BlockId merge = addBlock(
        region, scope, expression->location);
    auto& branch = mGraph->blocks[current.block.value].terminator;
    branch.kind = TerminatorKind::Branch;
    branch.location = expression->location;
    branch.operand = std::move(expression->lhs);
    if (expression->op == Operator::LogicalAnd) {
        branch.primary.target = rhsEntry;
        branch.secondary.target = merge;
    } else {
        branch.primary.target = merge;
        branch.secondary.target = rhsEntry;
    }

    auto rhs = normalizeControlFlowExpression(
        expression->rhs, OpenBlock{rhsEntry, {}}, region, scope, false);
    if (rhs) {
        if (!bindExpr(expression->rhs.get())) return std::nullopt;
        if (expression->rhs->type != expression->type) {
            error(expression->rhs->location,
                  "short-circuit expression must have frozen bool operands");
            return std::nullopt;
        }
        auto destination = std::make_unique<IdentifierExpr>();
        destination->location = expression->location;
        destination->name = name;
        destination->local = result;
        destination->type = expression->type;
        auto assignment = std::make_unique<AssignExpr>();
        assignment->location = expression->location;
        assignment->op = Operator::Assign;
        assignment->lhs = std::move(destination);
        assignment->rhs = std::move(expression->rhs);
        assignment->type = expression->type;
        auto statement = std::make_unique<ExprStmt>();
        statement->location = expression->location;
        statement->expr = std::move(assignment);
        mGraph->blocks[rhs->block.value].operations.push_back(
            std::move(statement));
        connectJump(*rhs, merge);
    }

    auto value = std::make_unique<IdentifierExpr>();
    value->location = expression->location;
    value->name = name;
    value->local = result;
    value->type = expression->type;
    replacement = std::move(value);
    return OpenBlock{merge, {}};
}

std::optional<ControlFlowBuilder::OpenBlock>
ControlFlowBuilder::lowerAllocationElements(
    const SourceLocation& location, const TypeRef& resultType,
    const TypeRef& allocatedType, HeapStorageKind storage,
    std::vector<InitAllocationExpr::Element> elements,
    OpenBlock current, RegionId region, ScopeId scope,
    std::unique_ptr<Expr>& replacement) {
    const auto* allocated = mModule->findType(allocatedType);
    const auto* result = mModule->findType(resultType);
    if (!allocated || !result || resultType != allocatedType) {
        error(location,
              "allocation result disagrees with its frozen allocated type");
        return std::nullopt;
    }
    if (storage != HeapStorageKind::Unique) {
        error(location,
              "canonical allocation supports only unique storage");
        return std::nullopt;
    }
    std::unordered_set<uint32_t> indices;
    for (const auto& element : elements) {
        if (!element.value || !indices.insert(element.index).second) {
            error(location,
                  "allocation initializer contains a missing or duplicate element");
            return std::nullopt;
        }
        TypeRef expected;
        if (allocated->kind == TypeKind::Struct ||
            allocated->kind == TypeKind::Record) {
            if (element.index >= allocated->fields.size()) {
                error(location,
                      "allocation initializer element is outside its frozen product type");
                return std::nullopt;
            }
            expected = allocated->fields[element.index].type;
        } else if (element.index == 0) {
            expected = allocatedType;
        } else {
            error(location,
                  "scalar allocation has a nonzero initializer index");
            return std::nullopt;
        }
        if (element.value->type.empty()) {
            // The structured backend defers type resolution to codegen.
            // Resolve inline from the expression structure so the canonical
            // path has typed elements before the frozen-layout check.
            std::function<TypeRef(Expr*)> resolveType = [&](Expr* e) -> TypeRef {
                if (!e) return {};
                if (!e->type.empty()) return e->type;
                if (auto* move = dynamic_cast<MoveExpr*>(e))
                    return resolveType(move->operand.get());
                if (auto* id = dynamic_cast<IdentifierExpr*>(e)) {
                    if (!id->name.empty()) {
                        const LocalId local = lookupLocal(id->name);
                        if (!local.empty() && local.value < mGraph->locals.size())
                            return mGraph->locals[local.value].type;
                    }
                }
                if (auto* binary = dynamic_cast<BinaryExpr*>(e)) {
                    auto t = resolveType(binary->lhs.get());
                    if (!t.empty()) return t;
                    return resolveType(binary->rhs.get());
                }
                if (auto* field = dynamic_cast<FieldAccessExpr*>(e)) {
                    auto objectType = resolveType(field->object.get());
                    if (!objectType.empty()) {
                        const auto* ot = mModule->findType(objectType);
                        if (ot) {
                            if (ot->kind == TypeKind::Reference)
                                ot = mModule->findType(ot->innerTypeId);
                            if (ot)
                                for (const auto& f : ot->fields)
                                    if (f.name == field->field) return f.type;
                        }
                    }
                }
                if (auto* envLoad = dynamic_cast<EnvLoadExpr*>(e)) {
                    if (envLoad->fieldIndex < mGraph->locals.size()) {
                        const auto& envLocal = mGraph->locals[envLoad->envLocal.value];
                        const auto* closureType = mModule->findType(envLocal.type);
                        if (closureType && envLoad->fieldIndex < closureType->capturedFields.size())
                            return closureType->capturedFields[envLoad->fieldIndex].type;
                    }
                }
                return {};
            };
            element.value->type = resolveType(element.value.get());
        }
        if (element.value->type != expected) {
            error(element.value->location,
                  "allocation initializer element disagrees with its frozen layout type");
            return std::nullopt;
        }
    }
    const size_t expectedCount =
        allocated->kind == TypeKind::Struct ||
            allocated->kind == TypeKind::Record
        ? allocated->fields.size() : 1;
    if (elements.size() != expectedCount) {
        error(location,
              "allocation initializer does not cover its frozen layout");
        return std::nullopt;
    }

    const std::string name =
        "$allocation.raw." + std::to_string(mExpressionCounter++);
    const LocalId allocation = addLocal(
        scope, LocalKind::Allocation, name, allocatedType,
        luna::ownership::Usage::Affine,
        luna::ownership::Relation::Owned, false);
    if (allocation.empty()) return std::nullopt;
    const CleanupId rawCleanup = addCleanup(
        allocation, allocatedType,
        luna::ownership::CleanupAction::Deallocate,
        CleanupKind::Allocation);
    if (rawCleanup.empty()) return std::nullopt;

    auto allocate = std::make_unique<AllocateStmt>();
    allocate->location = location;
    allocate->local = allocation;
    allocate->allocatedType = allocatedType;
    allocate->storage = storage;
    mGraph->blocks[current.block.value].operations.push_back(
        std::move(allocate));

    const size_t cleanupDepth = mActiveExpressionCleanups.size();
    mActiveExpressionCleanups.push_back(rawCleanup);
    std::vector<std::unique_ptr<Expr>*> operands;
    operands.reserve(elements.size());
    for (auto& element : elements) operands.push_back(&element.value);
    auto normalized = normalizeOrderedOperands(
        operands, std::move(current), region, scope);
    mActiveExpressionCleanups.resize(cleanupDepth);
    if (!normalized) return std::nullopt;

    auto initialized = std::make_unique<InitAllocationExpr>();
    initialized->location = location;
    initialized->type = resultType;
    initialized->allocation = allocation;
    initialized->allocatedType = allocatedType;
    initialized->storage = storage;
    initialized->elements = std::move(elements);
    replacement = std::move(initialized);
    return normalized;
}

std::optional<ControlFlowBuilder::OpenBlock>
ControlFlowBuilder::lowerRecordAllocation(
    std::unique_ptr<RecordLiteralExpr> expression, OpenBlock current,
    RegionId region, ScopeId scope, std::unique_ptr<Expr>& replacement) {
    if (!expression) return std::nullopt;
    const auto* type = mModule->findType(expression->type);
    if (!type || type->kind != TypeKind::Struct) {
        error(expression->location,
              "allocating record initializer has no frozen named struct type");
        return std::nullopt;
    }
    std::vector<InitAllocationExpr::Element> elements;
    elements.reserve(expression->fields.size());
    std::unordered_set<uint32_t> indices;
    for (auto& field : expression->fields) {
        size_t index = type->fields.size();
        for (size_t candidate = 0; candidate < type->fields.size();
             ++candidate)
            if (type->fields[candidate].name == field.name) {
                index = candidate;
                break;
            }
        if (index >= type->fields.size() ||
            !indices.insert(static_cast<uint32_t>(index)).second) {
            error(expression->location,
                  "allocating struct initializer has an unknown or duplicate field");
            return std::nullopt;
        }
        InitAllocationExpr::Element element;
        element.index = static_cast<uint32_t>(index);
        element.value = std::move(field.value);
        elements.push_back(std::move(element));
    }
    if (elements.size() != type->fields.size()) {
        error(expression->location,
              "allocating struct initializer is missing a frozen field");
        return std::nullopt;
    }
    return lowerAllocationElements(
        expression->location, expression->type, expression->type,
        HeapStorageKind::Unique, std::move(elements),
        std::move(current), region, scope, replacement);
}

std::optional<ControlFlowBuilder::OpenBlock>
ControlFlowBuilder::lowerHeapAllocation(
    std::unique_ptr<HeapAllocExpr> expression, OpenBlock current,
    RegionId region, ScopeId scope, std::unique_ptr<Expr>& replacement) {
    if (!expression || !expression->initializer) {
        error(expression ? expression->location : SourceLocation{},
              "heap allocation has no initializer");
        return std::nullopt;
    }
    auto* call = dynamic_cast<CallExpr*>(expression->initializer.get());
    if (!call) {
        error(expression->location,
              "heap allocation initializer is not a constructor call");
        return std::nullopt;
    }
    std::vector<InitAllocationExpr::Element> elements;
    elements.reserve(call->args.size());
    for (size_t index = 0; index < call->args.size(); ++index) {
        InitAllocationExpr::Element element;
        element.index = static_cast<uint32_t>(index);
        element.value = std::move(call->args[index]);
        elements.push_back(std::move(element));
    }
    return lowerAllocationElements(
        expression->location, expression->type,
        expression->allocatedType, expression->storage,
        std::move(elements), std::move(current), region, scope,
        replacement);
}

std::optional<ControlFlowBuilder::OpenBlock>
ControlFlowBuilder::lowerTryExpression(
    std::unique_ptr<TryExpr> expression, OpenBlock current,
    RegionId region, ScopeId scope, std::unique_ptr<Expr>& replacement) {
    if (!expression || !expression->operand) {
        error(expression ? expression->location : SourceLocation{},
              "error propagation has no operand");
        return std::nullopt;
    }

    auto operand = normalizeControlFlowExpression(
        expression->operand, std::move(current), region, scope, false);
    if (!operand) return std::nullopt;
    current = std::move(*operand);
    if (!bindExpr(expression->operand.get())) return std::nullopt;

    const auto* sourceResult = mModule->findType(expression->resultType);
    const auto* targetResult = mModule->findType(
        expression->propagatedResultType);
    const auto* valueType = mModule->findType(expression->valueType);
    const auto* errorType = mModule->findType(expression->errorType);
    const auto* targetErrorType = mModule->findType(
        expression->propagatedErrorType);
    if (!sourceResult || sourceResult->kind != TypeKind::Result ||
        sourceResult->typeArgumentIds.size() != 2 ||
        sourceResult->typeArgumentIds[0] != expression->valueType ||
        sourceResult->typeArgumentIds[1] != expression->errorType ||
        !targetResult || targetResult->kind != TypeKind::Result ||
        targetResult->typeArgumentIds.size() != 2 ||
        targetResult->typeArgumentIds[1] !=
            expression->propagatedErrorType ||
        !valueType || !errorType || !targetErrorType ||
        expression->operand->type != expression->resultType ||
        expression->type != expression->valueType) {
        error(expression->location,
              "error propagation disagrees with its frozen Result types");
        return std::nullopt;
    }

    const DeclarationRecord* conversion = nullptr;
    const TypeRecord* conversionType = nullptr;
    if (expression->errorType == expression->propagatedErrorType) {
        if (!expression->errorConversion.empty()) {
            error(expression->location,
                  "error propagation has an unnecessary From conversion");
            return std::nullopt;
        }
    } else {
        conversion = mModule->findDeclaration(expression->errorConversion);
        conversionType = conversion
            ? mModule->findType(conversion->type) : nullptr;
        if (!conversion || conversion->kind != DeclarationKind::Function ||
            !conversionType || conversionType->kind != TypeKind::Function ||
            conversionType->parameterTypeIds !=
                TypeRefVec{expression->errorType} ||
            conversionType->returnTypeId !=
                expression->propagatedErrorType ||
            conversionType->parameterContracts.size() != 1 ||
            conversionType->parameterContracts.front().relation !=
                luna::ownership::Relation::Owned ||
            conversionType->returnContract.relation !=
                luna::ownership::Relation::Owned) {
            error(expression->location,
                  "error propagation From witness has no canonical owned conversion contract");
            return std::nullopt;
        }
    }

    const std::string identity =
        std::to_string(mExpressionCounter++);
    const auto usageOf = [](const TypeRecord* type) {
        return type
            ? type->sysmeta.resource.usage
            : luna::ownership::Usage::Copy;
    };
    const LocalId successLocal = addLocal(
        scope, LocalKind::Pattern, "$try.value." + identity,
        expression->valueType, usageOf(valueType));
    const LocalId errorLocal = addLocal(
        scope, LocalKind::Pattern, "$try.error." + identity,
        expression->errorType, usageOf(errorType));
    if (successLocal.empty() || errorLocal.empty())
        return std::nullopt;

    const BlockId success = addBlock(region, scope, expression->location);
    const BlockId failure = addBlock(region, scope, expression->location);
    const BlockId invalid = addBlock(region, scope, expression->location);
    auto& invalidTerminator = mGraph->blocks[invalid.value].terminator;
    invalidTerminator.kind = TerminatorKind::Unreachable;
    invalidTerminator.location = expression->location;

    const auto identifier = [this, &expression](LocalId local) {
        auto value = std::make_unique<IdentifierExpr>();
        value->location = expression->location;
        if (!local.empty() && local.value < mGraph->locals.size()) {
            const auto& record = mGraph->locals[local.value];
            value->name = record.name;
            value->local = local;
            value->type = record.type;
        }
        return value;
    };
    const auto transferIfNeeded = [&expression, &usageOf](
        std::unique_ptr<Expr> value, const TypeRecord* type) {
        if (!type || !luna::ownership::isMoveOnly(usageOf(type)))
            return value;
        auto transfer = std::make_unique<MoveExpr>();
        transfer->location = expression->location;
        transfer->type = type->id;
        transfer->operand = std::move(value);
        return std::unique_ptr<Expr>(std::move(transfer));
    };

    std::unique_ptr<Expr> propagatedError = transferIfNeeded(
        identifier(errorLocal), errorType);
    if (conversion && conversionType) {
        auto call = std::make_unique<CallExpr>();
        call->location = expression->location;
        call->calleeRef = expression->errorConversion;
        call->type = expression->propagatedErrorType;
        call->returnUsage = conversionType->returnContract.usage;
        call->returnsLinear =
            call->returnUsage == luna::ownership::Usage::Linear;
        auto callee = std::make_unique<IdentifierExpr>();
        callee->location = expression->location;
        callee->name = conversion->sourceName;
        callee->declaration = expression->errorConversion;
        callee->type = conversion->type;
        call->callee = std::move(callee);
        call->args.push_back(std::move(propagatedError));
        propagatedError = std::move(call);
    }

    auto propagatedResult = std::make_unique<ResultConstructExpr>();
    propagatedResult->location = expression->location;
    propagatedResult->isOk = false;
    propagatedResult->payload = std::move(propagatedError);
    propagatedResult->type = expression->propagatedResultType;
    if (!bindExpr(propagatedResult.get())) return std::nullopt;

    auto failureCleanups = lowerCleanupObligations(
        expression->cleanups, scope);
    failureCleanups.insert(
        failureCleanups.end(), current.cleanups.begin(),
        current.cleanups.end());
    failureCleanups.insert(
        failureCleanups.end(), mActiveExpressionCleanups.begin(),
        mActiveExpressionCleanups.end());
    auto& failureTerminator = mGraph->blocks[failure.value].terminator;
    failureTerminator.kind = TerminatorKind::Return;
    failureTerminator.location = expression->location;
    failureTerminator.operand = std::move(propagatedResult);
    failureTerminator.exitCleanups = canonicalCleanupOrder(
        failureCleanups, scope, std::nullopt);

    Terminator switchTerminator;
    switchTerminator.kind = TerminatorKind::Switch;
    switchTerminator.location = expression->location;
    switchTerminator.operand = transferIfNeeded(
        std::move(expression->operand), sourceResult);
    switchTerminator.switchType = expression->resultType;
    switchTerminator.primary.target = invalid;
    SwitchEdge errorEdge;
    errorEdge.tag = 0;
    errorEdge.edge.target = failure;
    errorEdge.bindings.push_back(errorLocal);
    switchTerminator.cases.push_back(std::move(errorEdge));
    SwitchEdge successEdge;
    successEdge.tag = 1;
    successEdge.edge.target = success;
    successEdge.bindings.push_back(successLocal);
    switchTerminator.cases.push_back(std::move(successEdge));
    mGraph->blocks[current.block.value].terminator =
        std::move(switchTerminator);

    replacement = transferIfNeeded(
        identifier(successLocal), valueType);
    return OpenBlock{success, {}};
}

std::optional<ControlFlowBuilder::OpenBlock>
ControlFlowBuilder::lowerBlockExpression(
    std::unique_ptr<BlockExpr> expression, OpenBlock current,
    RegionId region, ScopeId scope, std::unique_ptr<Expr>& replacement) {
    if (!expression || !expression->block) {
        error(expression ? expression->location : SourceLocation{},
              "block expression has no structured body");
        return std::nullopt;
    }
    const auto* resultType = mModule->findType(expression->type);
    if (!resultType || resultType->kind != TypeKind::Unit) {
        error(expression->location,
              "block expression must have the frozen unit type");
        return std::nullopt;
    }

    const SourceLocation location = expression->location;
    auto body = lowerNestedBlock(
        std::move(expression->block), region, scope,
        RegionKind::Lexical);
    connectJump(current, body.entry);
    if (!body.exit) return std::nullopt;

    const BlockId continuation = addBlock(region, scope, location);
    connectJump(*body.exit, continuation);
    mGraph->regions[body.region.value].exit = continuation;

    auto unit = std::make_unique<UnitExpr>();
    unit->location = location;
    unit->type = expression->type;
    replacement = std::move(unit);
    return OpenBlock{continuation, {}};
}

std::optional<ControlFlowBuilder::OpenBlock>
ControlFlowBuilder::lowerIfExpression(
    std::unique_ptr<IfExpr> expression, OpenBlock current,
    RegionId region, ScopeId scope, std::unique_ptr<Expr>& replacement) {
    if (!expression || !expression->cond || !expression->thenExpr ||
        !expression->elseExpr) {
        error(expression ? expression->location : SourceLocation{},
              "if expression has an incomplete canonical shape");
        return std::nullopt;
    }
    const auto* resultType = mModule->findType(expression->type);
    if (!resultType || resultType->kind != TypeKind::Unit ||
        expression->thenExpr->type != expression->type ||
        expression->elseExpr->type != expression->type) {
        error(expression->location,
              "block-style if expression must have unit-typed branches");
        return std::nullopt;
    }

    auto condition = normalizeControlFlowExpression(
        expression->cond, std::move(current), region, scope, false);
    if (!condition) return std::nullopt;
    current = std::move(*condition);
    if (!bindExpr(expression->cond.get())) return std::nullopt;

    struct BuiltArm {
        BlockId entry;
        std::optional<OpenBlock> exit;
    };
    const auto lowerArm = [this, region, scope](
        std::unique_ptr<Expr> arm) -> BuiltArm {
        BuiltArm built;
        if (!arm) {
            error({}, "if expression has a null branch");
            return built;
        }
        const SourceLocation location = arm->location;
        built.entry = addBlock(region, scope, location);
        built.exit = normalizeControlFlowExpression(
            arm, OpenBlock{built.entry, {}}, region, scope, true);
        if (!built.exit || !arm || dynamic_cast<UnitExpr*>(arm.get()))
            return built;
        if (!bindExpr(arm.get())) {
            built.exit = std::nullopt;
            return built;
        }
        auto statement = std::make_unique<ExprStmt>();
        statement->location = location;
        statement->expr = std::move(arm);
        mGraph->blocks[built.exit->block.value].operations.push_back(
            std::move(statement));
        return built;
    };

    auto thenArm = lowerArm(std::move(expression->thenExpr));
    auto elseArm = lowerArm(std::move(expression->elseExpr));
    if (thenArm.entry.empty() || elseArm.entry.empty())
        return std::nullopt;

    auto& terminator = mGraph->blocks[current.block.value].terminator;
    terminator.kind = TerminatorKind::Branch;
    terminator.location = expression->location;
    terminator.operand = std::move(expression->cond);
    terminator.primary.target = thenArm.entry;
    terminator.secondary.target = elseArm.entry;

    if (!thenArm.exit && !elseArm.exit) return std::nullopt;
    const BlockId merge = addBlock(region, scope, expression->location);
    if (thenArm.exit) connectJump(*thenArm.exit, merge);
    if (elseArm.exit) connectJump(*elseArm.exit, merge);

    auto unit = std::make_unique<UnitExpr>();
    unit->location = expression->location;
    unit->type = expression->type;
    replacement = std::move(unit);
    return OpenBlock{merge, {}};
}

std::optional<ControlFlowBuilder::OpenBlock>
ControlFlowBuilder::normalizeControlFlowExpression(
    std::unique_ptr<Expr>& expression, OpenBlock current,
    RegionId region, ScopeId scope, bool discardUnitResult) {
    if (!expression) return current;
    if (dynamic_cast<TryExpr*>(expression.get())) {
        std::unique_ptr<TryExpr> owned(
            static_cast<TryExpr*>(expression.release()));
        return lowerTryExpression(
            std::move(owned), std::move(current), region, scope,
            expression);
    }
    if (dynamic_cast<BlockExpr*>(expression.get())) {
        std::unique_ptr<BlockExpr> owned(
            static_cast<BlockExpr*>(expression.release()));
        return lowerBlockExpression(
            std::move(owned), std::move(current), region, scope,
            expression);
    }
    if (dynamic_cast<IfExpr*>(expression.get())) {
        std::unique_ptr<IfExpr> owned(
            static_cast<IfExpr*>(expression.release()));
        return lowerIfExpression(
            std::move(owned), std::move(current), region, scope,
            expression);
    }
    if (auto* binary = dynamic_cast<BinaryExpr*>(expression.get());
        binary && (binary->op == Operator::LogicalAnd ||
                   binary->op == Operator::LogicalOr)) {
        std::unique_ptr<BinaryExpr> owned(
            static_cast<BinaryExpr*>(expression.release()));
        return lowerShortCircuitExpression(
            std::move(owned), std::move(current), region, scope,
            expression);
    }
    if (auto* record = dynamic_cast<RecordLiteralExpr*>(expression.get())) {
        const auto* type = mModule->findType(record->type);
        if (type && type->kind == TypeKind::Struct) {
            if (discardUnitResult) {
                error(record->location,
                      "owning struct allocation result cannot be discarded");
                return std::nullopt;
            }
            std::unique_ptr<RecordLiteralExpr> owned(
                static_cast<RecordLiteralExpr*>(expression.release()));
            return lowerRecordAllocation(
                std::move(owned), std::move(current), region, scope,
                expression);
        }
    }
    if (dynamic_cast<HeapAllocExpr*>(expression.get())) {
        if (discardUnitResult) {
            error(expression->location,
                  "owning heap allocation result cannot be discarded");
            return std::nullopt;
        }
        std::unique_ptr<HeapAllocExpr> owned(
            static_cast<HeapAllocExpr*>(expression.release()));
        return lowerHeapAllocation(
            std::move(owned), std::move(current), region, scope,
            expression);
    }
    auto* call = dynamic_cast<CallExpr*>(expression.get());
    if (call && containsIteratorTerminal(call) &&
        (call->iteratorOp == IteratorOp::Fold ||
         call->iteratorOp == IteratorOp::ForEach ||
         call->iteratorOp == IteratorOp::Count ||
         call->iteratorOp == IteratorOp::Collect)) {
        std::unique_ptr<CallExpr> owned(
            static_cast<CallExpr*>(expression.release()));
        return lowerIteratorTerminal(
            std::move(owned), std::move(current), region, scope,
            discardUnitResult, expression);
    }

    if (call && call->iteratorOp == IteratorOp::None) {
        std::vector<std::unique_ptr<Expr>*> operands;
        operands.reserve(call->args.size() + 1);
        operands.push_back(&call->callee);
        for (auto& argument : call->args)
            operands.push_back(&argument);
        return normalizeOrderedOperands(
            operands, std::move(current), region, scope);
    }
    if (auto* binary = dynamic_cast<BinaryExpr*>(expression.get())) {
        return normalizeOrderedOperands(
            {&binary->lhs, &binary->rhs}, std::move(current),
            region, scope);
    }
    if (auto* unary = dynamic_cast<UnaryExpr*>(expression.get()))
        return normalizeControlFlowExpression(
            unary->operand, std::move(current), region, scope, false);
    if (auto* field = dynamic_cast<FieldAccessExpr*>(expression.get()))
        return normalizeControlFlowExpression(
            field->object, std::move(current), region, scope, false);
    if (auto* index = dynamic_cast<IndexExpr*>(expression.get()))
        return normalizeOrderedOperands(
            {&index->object, &index->index}, std::move(current),
            region, scope);
    if (auto* length = dynamic_cast<SliceLengthExpr*>(expression.get()))
        return normalizeControlFlowExpression(
            length->slice, std::move(current), region, scope, false);
    if (auto* array = dynamic_cast<ArrayLiteralExpr*>(expression.get())) {
        std::vector<std::unique_ptr<Expr>*> operands;
        operands.reserve(array->elements.size());
        for (auto& element : array->elements)
            operands.push_back(&element);
        return normalizeOrderedOperands(
            operands, std::move(current), region, scope);
    }
    if (auto* record = dynamic_cast<RecordLiteralExpr*>(expression.get())) {
        const auto* recordType = mModule->findType(record->type);
        if (!recordType ||
            (recordType->kind != TypeKind::Record &&
             recordType->kind != TypeKind::Struct)) {
            error(record->location,
                  "record initializer has no frozen product type");
            return std::nullopt;
        }
        if (recordType->kind == TypeKind::Record) {
            std::vector<std::unique_ptr<Expr>*> operands;
            operands.reserve(record->fields.size());
            for (auto& field : record->fields)
                operands.push_back(&field.value);
            return normalizeOrderedOperands(
                operands, std::move(current), region, scope);
        }
        error(record->location,
              "allocating struct initializer escaped canonical allocation lowering");
        return std::nullopt;
    }
    if (auto* launch = dynamic_cast<LaunchExpr*>(expression.get())) {
        std::vector<std::unique_ptr<Expr>*> operands;
        operands.reserve(launch->args.size() + 1);
        operands.push_back(&launch->threads);
        for (auto& argument : launch->args)
            operands.push_back(&argument);
        return normalizeOrderedOperands(
            operands, std::move(current), region, scope);
    }
    if (auto* variant =
            dynamic_cast<VariantConstructExpr*>(expression.get())) {
        std::vector<std::unique_ptr<Expr>*> operands;
        operands.reserve(variant->args.size());
        for (auto& argument : variant->args)
            operands.push_back(&argument);
        return normalizeOrderedOperands(
            operands, std::move(current), region, scope);
    }
    if (auto* result =
            dynamic_cast<ResultConstructExpr*>(expression.get()))
        return normalizeControlFlowExpression(
            result->payload, std::move(current), region, scope, false);
    if (auto* move = dynamic_cast<MoveExpr*>(expression.get()))
        return normalizeControlFlowExpression(
            move->operand, std::move(current), region, scope, false);
    if (auto* borrow = dynamic_cast<BorrowExpr*>(expression.get()))
        return normalizeControlFlowExpression(
            borrow->operand, std::move(current), region, scope, false);
    if (auto* dereference = dynamic_cast<DerefExpr*>(expression.get()))
        return normalizeControlFlowExpression(
            dereference->operand, std::move(current), region, scope, false);
    if (auto* address = dynamic_cast<AddrOfExpr*>(expression.get()))
        return normalizeControlFlowExpression(
            address->operand, std::move(current), region, scope, false);
    if (auto* assignment = dynamic_cast<AssignExpr*>(expression.get())) {
        if (containsIteratorTerminal(assignment->lhs.get())) {
            error(assignment->lhs->location,
                  "iterator terminal cannot form an assignment destination");
            return std::nullopt;
        }
        return normalizeControlFlowExpression(
            assignment->rhs, std::move(current), region, scope, false);
    }
    return current;
}

std::optional<ControlFlowBuilder::OpenBlock>
ControlFlowBuilder::normalizeOrderedOperands(
    const std::vector<std::unique_ptr<Expr>*>& operands,
    OpenBlock current, RegionId region, ScopeId scope) {
    const size_t cleanupDepth = mActiveExpressionCleanups.size();
    const auto restoreCleanupDepth = [this, cleanupDepth] {
        mActiveExpressionCleanups.resize(cleanupDepth);
    };
    for (size_t index = 0; index < operands.size(); ++index) {
        auto* operand = operands[index];
        if (!operand || !containsPendingControlFlow(operand->get())) continue;

        bool allowLinear = true;
        for (size_t following = index; following < operands.size();
             ++following) {
            const auto* candidate = operands[following];
            if (candidate && containsPotentialEarlyExit(candidate->get())) {
                allowLinear = false;
                break;
            }
        }

        // A later control-flow operand must not move evaluation of any
        // preceding value across its new CFG. Each eager value is sequenced
        // in source order. Non-trivial unit expressions become ordinary
        // statements; other values are frozen into synthetic locals. Copy
        // values are read normally; affine values are explicitly transferred
        // once, while their cleanup rows remain visible to any early exit
        // until the parent is emitted.
        for (size_t previous = 0; previous < index; ++previous) {
            auto* earlier = operands[previous];
            if (earlier &&
                !hoistOrderedOperand(
                    *earlier, current, scope, allowLinear)) {
                restoreCleanupDepth();
                return std::nullopt;
            }
        }

        auto normalized = normalizeControlFlowExpression(
            *operand, std::move(current), region, scope, false);
        if (!normalized) {
            restoreCleanupDepth();
            return std::nullopt;
        }
        current = std::move(*normalized);
        if (containsPendingControlFlow(operand->get())) {
            error((*operand)->location,
                  "control-flow expression remains in an unsupported position");
            restoreCleanupDepth();
            return std::nullopt;
        }
    }
    restoreCleanupDepth();
    return current;
}


} // namespace moon
