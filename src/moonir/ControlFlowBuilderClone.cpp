#include "ControlFlowBuilderCloneInternal.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <type_traits>
#include <unordered_set>

namespace moon {
namespace control_flow_builder_detail {

constexpr size_t kMaxStructureDepth = 4096;
thread_local size_t gStructureDepth = 0;
thread_local bool gStructureExceeded = false;

class StructureDepthGuard {
public:
    StructureDepthGuard() {
        if (gStructureDepth >= kMaxStructureDepth)
            gStructureExceeded = true;
        ++gStructureDepth;
    }
    ~StructureDepthGuard() { --gStructureDepth; }
    StructureDepthGuard(const StructureDepthGuard&) = delete;
    StructureDepthGuard& operator=(const StructureDepthGuard&) = delete;
};

template <typename T>
std::unique_ptr<T> clonedNode(const T* source) {
    if (!source) return nullptr;
    auto result = std::make_unique<T>();
    result->location = source->location;
    if constexpr (std::is_base_of_v<Expr, T>) result->type = source->type;
    return result;
}

std::unique_ptr<Expr> cloneStructuredExpr(const Expr* source);
std::unique_ptr<Stmt> cloneStructuredStmt(const Stmt* source);

std::unique_ptr<BlockStmt> cloneStructuredBlock(const BlockStmt* source) {
    if (!source) return nullptr;
    if (gStructureExceeded) return nullptr;
    StructureDepthGuard depthGuard;
    auto result = clonedNode(source);
    result->stmts.reserve(source->stmts.size());
    for (const auto& statement : source->stmts) {
        auto cloned = cloneStructuredStmt(statement.get());
        if (!cloned) return nullptr;
        result->stmts.push_back(std::move(cloned));
    }
    return result;
}

std::unique_ptr<Expr> cloneStructuredExpr(const Expr* source) {
    if (!source) return nullptr;
    if (gStructureExceeded) return nullptr;
    StructureDepthGuard depthGuard;
    if (const auto* value = dynamic_cast<const IntLiteralExpr*>(source)) {
        auto result = clonedNode(value);
        result->value = value->value;
        return result;
    }
    if (const auto* value = dynamic_cast<const FloatLiteralExpr*>(source)) {
        auto result = clonedNode(value);
        result->value = value->value;
        return result;
    }
    if (const auto* value = dynamic_cast<const StringLiteralExpr*>(source)) {
        auto result = clonedNode(value);
        result->value = value->value;
        return result;
    }
    if (const auto* value = dynamic_cast<const BoolLiteralExpr*>(source)) {
        auto result = clonedNode(value);
        result->value = value->value;
        return result;
    }
    if (const auto* value = dynamic_cast<const UnitExpr*>(source))
        return clonedNode(value);
    if (const auto* value = dynamic_cast<const IdentifierExpr*>(source)) {
        auto result = clonedNode(value);
        result->name = value->name;
        result->local = value->local;
        result->declaration = value->declaration;
        return result;
    }
    if (const auto* value = dynamic_cast<const BinaryExpr*>(source)) {
        auto result = clonedNode(value);
        result->lhs = cloneStructuredExpr(value->lhs.get());
        result->op = value->op;
        result->rhs = cloneStructuredExpr(value->rhs.get());
        return result;
    }
    if (const auto* value = dynamic_cast<const UnaryExpr*>(source)) {
        auto result = clonedNode(value);
        result->op = value->op;
        result->operand = cloneStructuredExpr(value->operand.get());
        return result;
    }
    if (const auto* value = dynamic_cast<const CallExpr*>(source)) {
        auto result = clonedNode(value);
        result->callee = cloneStructuredExpr(value->callee.get());
        for (const auto& argument : value->args)
            result->args.push_back(cloneStructuredExpr(argument.get()));
        result->typeArgs = value->typeArgs;
        result->calleeRef = value->calleeRef;
        result->returnsLinear = value->returnsLinear;
        result->returnUsage = value->returnUsage;
        result->intrinsicType = value->intrinsicType;
        result->iteratorInputType = value->iteratorInputType;
        result->iteratorOutputType = value->iteratorOutputType;
        result->iteratorOp = value->iteratorOp;
        result->iteratorRecipeStateName = value->iteratorRecipeStateName;
        result->iteratorRecipeSourceType = value->iteratorRecipeSourceType;
        result->iteratorCollectTargetType = value->iteratorCollectTargetType;
        result->iteratorCollectBuilderType = value->iteratorCollectBuilderType;
        result->iteratorCollectBegin = value->iteratorCollectBegin;
        result->iteratorCollectPush = value->iteratorCollectPush;
        result->iteratorCollectFinish = value->iteratorCollectFinish;
        result->compileTimeValue = value->compileTimeValue;
        return result;
    }
    if (const auto* value = dynamic_cast<const LaunchExpr*>(source)) {
        auto result = clonedNode(value);
        result->kernelName = value->kernelName;
        result->kernelRef = value->kernelRef;
        result->threads = cloneStructuredExpr(value->threads.get());
        for (const auto& argument : value->args)
            result->args.push_back(cloneStructuredExpr(argument.get()));
        result->inFlightResources = value->inFlightResources;
        return result;
    }
    if (const auto* value = dynamic_cast<const VariantConstructExpr*>(source)) {
        auto result = clonedNode(value);
        result->typeName = value->typeName;
        result->variantName = value->variantName;
        for (const auto& argument : value->args)
            result->args.push_back(cloneStructuredExpr(argument.get()));
        result->constructedType = value->constructedType;
        return result;
    }
    if (const auto* value = dynamic_cast<const ResultConstructExpr*>(source)) {
        auto result = clonedNode(value);
        result->isOk = value->isOk;
        result->payload = cloneStructuredExpr(value->payload.get());
        return result;
    }
    if (const auto* value = dynamic_cast<const FieldAccessExpr*>(source)) {
        auto result = clonedNode(value);
        result->object = cloneStructuredExpr(value->object.get());
        result->field = value->field;
        return result;
    }
    if (const auto* value = dynamic_cast<const IndexExpr*>(source)) {
        auto result = clonedNode(value);
        result->object = cloneStructuredExpr(value->object.get());
        result->index = cloneStructuredExpr(value->index.get());
        return result;
    }
    if (const auto* value = dynamic_cast<const SliceLengthExpr*>(source)) {
        auto result = clonedNode(value);
        result->slice = cloneStructuredExpr(value->slice.get());
        return result;
    }
    if (const auto* value = dynamic_cast<const ArrayLiteralExpr*>(source)) {
        auto result = clonedNode(value);
        result->elements.reserve(value->elements.size());
        for (const auto& element : value->elements)
            result->elements.push_back(cloneStructuredExpr(element.get()));
        result->elementType = value->elementType;
        return result;
    }
    if (const auto* value = dynamic_cast<const RecordLiteralExpr*>(source)) {
        auto result = clonedNode(value);
        for (const auto& field : value->fields)
            result->fields.push_back(
                {field.name, cloneStructuredExpr(field.value.get())});
        return result;
    }
    if (const auto* value = dynamic_cast<const HeapAllocExpr*>(source)) {
        auto result = clonedNode(value);
        result->initializer = cloneStructuredExpr(value->initializer.get());
        result->allocatedType = value->allocatedType;
        result->storage = value->storage;
        return result;
    }
    if (const auto* value = dynamic_cast<const InitAllocationExpr*>(source)) {
        auto result = clonedNode(value);
        result->allocation = value->allocation;
        result->allocatedType = value->allocatedType;
        result->storage = value->storage;
        for (const auto& element : value->elements)
            result->elements.push_back(
                {element.index, cloneStructuredExpr(element.value.get())});
        return result;
    }
    if (const auto* value = dynamic_cast<const TryExpr*>(source)) {
        auto result = clonedNode(value);
        result->operand = cloneStructuredExpr(value->operand.get());
        result->resultType = value->resultType;
        result->propagatedResultType = value->propagatedResultType;
        result->valueType = value->valueType;
        result->errorType = value->errorType;
        result->propagatedErrorType = value->propagatedErrorType;
        result->errorConversion = value->errorConversion;
        result->cleanups = value->cleanups;
        return result;
    }
    if (const auto* value = dynamic_cast<const MoveExpr*>(source)) {
        auto result = clonedNode(value);
        result->operand = cloneStructuredExpr(value->operand.get());
        result->nextUnread = value->nextUnread;
        return result;
    }
    if (const auto* value = dynamic_cast<const BorrowExpr*>(source)) {
        auto result = clonedNode(value);
        result->isMutable = value->isMutable;
        result->operand = cloneStructuredExpr(value->operand.get());
        return result;
    }
    if (const auto* value = dynamic_cast<const DerefExpr*>(source)) {
        auto result = clonedNode(value);
        result->operand = cloneStructuredExpr(value->operand.get());
        return result;
    }
    if (const auto* value = dynamic_cast<const AddrOfExpr*>(source)) {
        auto result = clonedNode(value);
        result->isMutable = value->isMutable;
        result->operand = cloneStructuredExpr(value->operand.get());
        return result;
    }
    if (const auto* value = dynamic_cast<const BlockExpr*>(source)) {
        auto result = clonedNode(value);
        result->block = cloneStructuredBlock(value->block.get());
        return result;
    }
    if (const auto* value = dynamic_cast<const IfExpr*>(source)) {
        auto result = clonedNode(value);
        result->cond = cloneStructuredExpr(value->cond.get());
        result->thenExpr = cloneStructuredExpr(value->thenExpr.get());
        result->elseExpr = cloneStructuredExpr(value->elseExpr.get());
        return result;
    }
    if (const auto* value = dynamic_cast<const EnvLoadExpr*>(source)) {
        auto result = clonedNode(value);
        result->envLocal = value->envLocal;
        result->fieldIndex = value->fieldIndex;
        return result;
    }
    if (const auto* value = dynamic_cast<const MakeClosureExpr*>(source)) {
        if (!value->lambda || value->lambda->controlFlow) return nullptr;
        auto clonedLambda = cloneStructuredExpr(value->lambda.get());
        if (!clonedLambda) return nullptr;
        auto result = clonedNode(value);
        result->lambda = std::unique_ptr<LambdaExpr>(
            static_cast<LambdaExpr*>(clonedLambda.release()));
        for (const auto& captured : value->capturedValues) {
            auto clonedCaptured = cloneStructuredExpr(captured.get());
            if (captured && !clonedCaptured) return nullptr;
            result->capturedValues.push_back(std::move(clonedCaptured));
        }
        return result;
    }
    if (const auto* value = dynamic_cast<const LambdaExpr*>(source)) {
        // Composition precedes canonical lambda construction. Importing an
        // already-built child graph would require table remapping and is
        // intentionally not a structured clone operation.
        if (value->controlFlow) return nullptr;
        auto result = clonedNode(value);
        result->params = value->params;
        result->returnType = value->returnType;
        result->body = cloneStructuredBlock(value->body.get());
        result->closureType = value->closureType;
        result->captures = value->captures;
        result->identitySuffix = value->identitySuffix;
        result->envParamName = value->envParamName;
        return result;
    }
    if (const auto* value = dynamic_cast<const AssignExpr*>(source)) {
        auto result = clonedNode(value);
        result->op = value->op;
        result->lhs = cloneStructuredExpr(value->lhs.get());
        result->rhs = cloneStructuredExpr(value->rhs.get());
        return result;
    }
    return nullptr;
}

std::unique_ptr<Stmt> cloneStructuredStmt(const Stmt* source) {
    if (!source) return nullptr;
    if (gStructureExceeded) return nullptr;
    StructureDepthGuard depthGuard;
    if (const auto* statement = dynamic_cast<const BlockStmt*>(source))
        return cloneStructuredBlock(statement);
    if (const auto* statement = dynamic_cast<const LetStmt*>(source)) {
        auto result = clonedNode(statement);
        result->name = statement->name;
        result->local = statement->local;
        result->isConst = statement->isConst;
        result->isLinear = statement->isLinear;
        result->usage = statement->usage;
        result->relation = statement->relation;
        result->type = statement->type;
        result->initializer = cloneStructuredExpr(statement->initializer.get());
        result->materializesIteratorRecipe = statement->materializesIteratorRecipe;
        result->materializedIteratorOwnsSource =
            statement->materializedIteratorOwnsSource;
        result->materializedIteratorSourceType =
            statement->materializedIteratorSourceType;
        return result;
    }
    if (const auto* statement = dynamic_cast<const AllocateStmt*>(source)) {
        auto result = clonedNode(statement);
        result->local = statement->local;
        result->allocatedType = statement->allocatedType;
        result->storage = statement->storage;
        return result;
    }
    if (const auto* statement = dynamic_cast<const ReturnStmt*>(source)) {
        auto result = clonedNode(statement);
        result->value = cloneStructuredExpr(statement->value.get());
        result->autoFrees = statement->autoFrees;
        result->cleanups = statement->cleanups;
        return result;
    }
    if (const auto* statement = dynamic_cast<const ExprStmt*>(source)) {
        auto result = clonedNode(statement);
        result->expr = cloneStructuredExpr(statement->expr.get());
        return result;
    }
    if (const auto* statement = dynamic_cast<const IfStmt*>(source)) {
        auto result = clonedNode(statement);
        result->cond = cloneStructuredExpr(statement->cond.get());
        result->thenBlock = cloneStructuredBlock(statement->thenBlock.get());
        result->elseBranch = cloneStructuredStmt(statement->elseBranch.get());
        return result;
    }
    if (const auto* statement = dynamic_cast<const MatchStmt*>(source)) {
        auto result = clonedNode(statement);
        result->scrutinee = cloneStructuredExpr(statement->scrutinee.get());
        result->matchedType = statement->matchedType;
        for (const auto& arm : statement->arms) {
            MatchArm cloned;
            cloned.location = arm.location;
            cloned.variantName = arm.variantName;
            cloned.variantIndex = arm.variantIndex;
            cloned.bindings = arm.bindings;
            cloned.bindingTypes = arm.bindingTypes;
            cloned.bindingUsages = arm.bindingUsages;
            cloned.body = cloneStructuredBlock(arm.body.get());
            result->arms.push_back(std::move(cloned));
        }
        return result;
    }
    if (const auto* statement = dynamic_cast<const WhileStmt*>(source)) {
        auto result = clonedNode(statement);
        result->cond = cloneStructuredExpr(statement->cond.get());
        result->body = cloneStructuredBlock(statement->body.get());
        return result;
    }
    if (const auto* statement = dynamic_cast<const ForStmt*>(source)) {
        auto result = clonedNode(statement);
        result->varName = statement->varName;
        result->bindingUsage = statement->bindingUsage;
        result->iterable = cloneStructuredExpr(statement->iterable.get());
        result->body = cloneStructuredBlock(statement->body.get());
        result->elementType = statement->elementType;
        result->protocolNext = statement->protocolNext;
        result->protocolIteratorType = statement->protocolIteratorType;
        result->protocolOptionType = statement->protocolOptionType;
        result->protocolNoneVariant = statement->protocolNoneVariant;
        result->protocolSomeVariant = statement->protocolSomeVariant;
        result->protocolInto = statement->protocolInto;
        result->protocolInputType = statement->protocolInputType;
        result->protocolStateName = statement->protocolStateName;
        result->protocolStateNeedsCleanup = statement->protocolStateNeedsCleanup;
        result->protocolStateCleanup = statement->protocolStateCleanup;
        result->recipeStateName = statement->recipeStateName;
        result->recipeSourceType = statement->recipeSourceType;
        return result;
    }
    if (const auto* statement = dynamic_cast<const FreeStmt*>(source)) {
        auto result = clonedNode(statement);
        result->operand = cloneStructuredExpr(statement->operand.get());
        result->action = statement->action;
        result->isImplicit = statement->isImplicit;
        return result;
    }
    if (const auto* statement = dynamic_cast<const SlotDeclStmt*>(source)) {
        auto result = clonedNode(statement);
        result->name = statement->name;
        result->acceptedKind = statement->acceptedKind;
        result->acceptedCardinality = statement->acceptedCardinality;
        result->params = statement->params;
        result->defaultFragment = statement->defaultFragment;
        result->defaultFragmentRef = statement->defaultFragmentRef;
        result->structuralType = statement->structuralType;
        return result;
    }
    if (const auto* statement = dynamic_cast<const SlotInvokeStmt*>(source)) {
        auto result = clonedNode(statement);
        result->name = statement->name;
        result->acceptedKind = statement->acceptedKind;
        result->acceptedCardinality = statement->acceptedCardinality;
        for (const auto& argument : statement->args)
            result->args.push_back(cloneStructuredExpr(argument.get()));
        result->continuation = cloneStructuredBlock(statement->continuation.get());
        result->isImplicitCapture = statement->isImplicitCapture;
        result->interfaceParams = statement->interfaceParams;
        result->resolvedParamNames = statement->resolvedParamNames;
        result->defaultFragment = statement->defaultFragment;
        result->defaultFragmentRef = statement->defaultFragmentRef;
        result->structuralType = statement->structuralType;
        return result;
    }
    if (const auto* statement = dynamic_cast<const ResumeStmt*>(source))
        return clonedNode(statement);
    if (const auto* statement = dynamic_cast<const AbortStmt*>(source)) {
        auto result = clonedNode(statement);
        result->autoFrees = statement->autoFrees;
        result->cleanups = statement->cleanups;
        return result;
    }
    if (const auto* statement = dynamic_cast<const AwaitStmt*>(source)) {
        auto result = clonedNode(statement);
        result->event = cloneStructuredExpr(statement->event.get());
        return result;
    }
    if (const auto* statement = dynamic_cast<const ApplyStmt*>(source)) {
        auto result = clonedNode(statement);
        result->slotName = statement->slotName;
        result->fragmentName = statement->fragmentName;
        result->fragmentRef = statement->fragmentRef;
        result->body = cloneStructuredBlock(statement->body.get());
        return result;
    }
    return nullptr;
}

// In-place rewrite of capture reads inside a capturing lambda body: every
// IdentifierExpr naming a capture becomes an EnvLoad from the synthetic
// environment parameter (C016 CL009). Capture names arrive in canonical
// captured-field order, so the identifier position is the field index.
std::unique_ptr<Stmt> rewriteCaptureReadsStmt(
    std::unique_ptr<Stmt> stmt,
    const std::vector<std::string>& captures,
    const LocalId& envLocal,
    const TypeRef& closureType,
    const Module& module);

std::unique_ptr<Expr> rewriteCaptureReadsExpr(
    std::unique_ptr<Expr> expr,
    const std::vector<std::string>& captures,
    const LocalId& envLocal,
    const TypeRef& closureType,
    const Module& module) {
    if (!expr) return nullptr;
    if (gStructureExceeded) return expr;
    StructureDepthGuard depthGuard;
    if (auto* identifier = dynamic_cast<IdentifierExpr*>(expr.get())) {
        if (identifier->declaration.empty()) {
            const auto found = std::find(
                captures.begin(), captures.end(), identifier->name);
            if (found != captures.end()) {
                const size_t fieldIndex =
                    static_cast<size_t>(found - captures.begin());
                auto load = std::make_unique<EnvLoadExpr>();
                load->envLocal = envLocal;
                load->fieldIndex = fieldIndex;
                load->type = identifier->type;
                load->location = identifier->location;
                if (load->type.empty()) {
                    const auto* closure = module.findType(closureType);
                    if (closure &&
                        fieldIndex < closure->capturedFields.size())
                        load->type =
                            closure->capturedFields[fieldIndex].type;
                }
                return load;
            }
        }
        return expr;
    }
    const auto rewriteChildren = [&](std::vector<std::unique_ptr<Expr>>& children) {
        for (auto& child : children)
            child = rewriteCaptureReadsExpr(
                std::move(child), captures, envLocal, closureType, module);
    };
    if (auto* binary = dynamic_cast<BinaryExpr*>(expr.get())) {
        binary->lhs = rewriteCaptureReadsExpr(
            std::move(binary->lhs), captures, envLocal, closureType, module);
        binary->rhs = rewriteCaptureReadsExpr(
            std::move(binary->rhs), captures, envLocal, closureType, module);
    } else if (auto* unary = dynamic_cast<UnaryExpr*>(expr.get())) {
        unary->operand = rewriteCaptureReadsExpr(
            std::move(unary->operand), captures, envLocal, closureType, module);
    } else if (auto* call = dynamic_cast<CallExpr*>(expr.get())) {
        call->callee = rewriteCaptureReadsExpr(
            std::move(call->callee), captures, envLocal, closureType, module);
        rewriteChildren(call->args);
    } else if (auto* closure =
                   dynamic_cast<MakeClosureExpr*>(expr.get())) {
        for (auto& captured : closure->capturedValues)
            captured = rewriteCaptureReadsExpr(
                std::move(captured), captures, envLocal, closureType, module);
    } else if (auto* launch = dynamic_cast<LaunchExpr*>(expr.get())) {
        launch->threads = rewriteCaptureReadsExpr(
            std::move(launch->threads), captures, envLocal, closureType, module);
        rewriteChildren(launch->args);
    } else if (auto* variant = dynamic_cast<VariantConstructExpr*>(expr.get())) {
        rewriteChildren(variant->args);
    } else if (auto* result = dynamic_cast<ResultConstructExpr*>(expr.get())) {
        result->payload = rewriteCaptureReadsExpr(
            std::move(result->payload), captures, envLocal, closureType, module);
    } else if (auto* field = dynamic_cast<FieldAccessExpr*>(expr.get())) {
        field->object = rewriteCaptureReadsExpr(
            std::move(field->object), captures, envLocal, closureType, module);
    } else if (auto* index = dynamic_cast<IndexExpr*>(expr.get())) {
        index->object = rewriteCaptureReadsExpr(
            std::move(index->object), captures, envLocal, closureType, module);
        index->index = rewriteCaptureReadsExpr(
            std::move(index->index), captures, envLocal, closureType, module);
    } else if (auto* slice = dynamic_cast<SliceLengthExpr*>(expr.get())) {
        slice->slice = rewriteCaptureReadsExpr(
            std::move(slice->slice), captures, envLocal, closureType, module);
    } else if (auto* array = dynamic_cast<ArrayLiteralExpr*>(expr.get())) {
        rewriteChildren(array->elements);
    } else if (auto* record = dynamic_cast<RecordLiteralExpr*>(expr.get())) {
        for (auto& field : record->fields)
            field.value = rewriteCaptureReadsExpr(
                std::move(field.value), captures, envLocal, closureType, module);
    } else if (auto* alloc = dynamic_cast<HeapAllocExpr*>(expr.get())) {
        alloc->initializer = rewriteCaptureReadsExpr(
            std::move(alloc->initializer), captures, envLocal, closureType, module);
    } else if (auto* init = dynamic_cast<InitAllocationExpr*>(expr.get())) {
        for (auto& element : init->elements)
            element.value = rewriteCaptureReadsExpr(
                std::move(element.value), captures, envLocal, closureType, module);
    } else if (auto* attempt = dynamic_cast<TryExpr*>(expr.get())) {
        attempt->operand = rewriteCaptureReadsExpr(
            std::move(attempt->operand), captures, envLocal, closureType, module);
    } else if (auto* move = dynamic_cast<MoveExpr*>(expr.get())) {
        move->operand = rewriteCaptureReadsExpr(
            std::move(move->operand), captures, envLocal, closureType, module);
    } else if (auto* borrow = dynamic_cast<BorrowExpr*>(expr.get())) {
        borrow->operand = rewriteCaptureReadsExpr(
            std::move(borrow->operand), captures, envLocal, closureType, module);
    } else if (auto* deref = dynamic_cast<DerefExpr*>(expr.get())) {
        deref->operand = rewriteCaptureReadsExpr(
            std::move(deref->operand), captures, envLocal, closureType, module);
    } else if (auto* address = dynamic_cast<AddrOfExpr*>(expr.get())) {
        address->operand = rewriteCaptureReadsExpr(
            std::move(address->operand), captures, envLocal, closureType, module);
    } else if (auto* block = dynamic_cast<BlockExpr*>(expr.get())) {
        if (block->block) {
            auto activeCaptures = captures;
            for (auto& statement : block->block->stmts) {
                statement = rewriteCaptureReadsStmt(
                    std::move(statement), activeCaptures, envLocal,
                    closureType, module);
                if (const auto* declaration =
                        dynamic_cast<const LetStmt*>(statement.get()))
                    activeCaptures.erase(
                        std::remove(activeCaptures.begin(),
                                    activeCaptures.end(), declaration->name),
                        activeCaptures.end());
            }
        }
    } else if (auto* conditional = dynamic_cast<IfExpr*>(expr.get())) {
        conditional->cond = rewriteCaptureReadsExpr(
            std::move(conditional->cond), captures, envLocal, closureType, module);
        conditional->thenExpr = rewriteCaptureReadsExpr(
            std::move(conditional->thenExpr), captures, envLocal, closureType, module);
        conditional->elseExpr = rewriteCaptureReadsExpr(
            std::move(conditional->elseExpr), captures, envLocal, closureType, module);
    } else if (auto* assignment = dynamic_cast<AssignExpr*>(expr.get())) {
        assignment->lhs = rewriteCaptureReadsExpr(
            std::move(assignment->lhs), captures, envLocal, closureType, module);
        assignment->rhs = rewriteCaptureReadsExpr(
            std::move(assignment->rhs), captures, envLocal, closureType, module);
    }
    return expr;
}

std::unique_ptr<Stmt> rewriteCaptureReadsStmt(
    std::unique_ptr<Stmt> stmt,
    const std::vector<std::string>& captures,
    const LocalId& envLocal,
    const TypeRef& closureType,
    const Module& module) {
    if (!stmt) return nullptr;
    if (gStructureExceeded) return stmt;
    StructureDepthGuard depthGuard;
    if (auto* block = dynamic_cast<BlockStmt*>(stmt.get())) {
        auto activeCaptures = captures;
        for (auto& statement : block->stmts) {
            statement = rewriteCaptureReadsStmt(
                std::move(statement), activeCaptures, envLocal, closureType,
                module);
            if (const auto* declaration =
                    dynamic_cast<const LetStmt*>(statement.get()))
                activeCaptures.erase(
                    std::remove(activeCaptures.begin(),
                                activeCaptures.end(), declaration->name),
                    activeCaptures.end());
        }
        return stmt;
    }
    if (auto* declaration = dynamic_cast<LetStmt*>(stmt.get())) {
        declaration->initializer = rewriteCaptureReadsExpr(
            std::move(declaration->initializer), captures, envLocal,
            closureType, module);
        return stmt;
    }
    if (auto* returned = dynamic_cast<ReturnStmt*>(stmt.get())) {
        returned->value = rewriteCaptureReadsExpr(
            std::move(returned->value), captures, envLocal, closureType, module);
        return stmt;
    }
    if (auto* expression = dynamic_cast<ExprStmt*>(stmt.get())) {
        expression->expr = rewriteCaptureReadsExpr(
            std::move(expression->expr), captures, envLocal, closureType, module);
        return stmt;
    }
    if (auto* conditional = dynamic_cast<IfStmt*>(stmt.get())) {
        conditional->cond = rewriteCaptureReadsExpr(
            std::move(conditional->cond), captures, envLocal, closureType, module);
        for (auto& statement : conditional->thenBlock->stmts)
            statement = rewriteCaptureReadsStmt(
                std::move(statement), captures, envLocal, closureType, module);
        if (conditional->elseBranch)
            conditional->elseBranch = rewriteCaptureReadsStmt(
                std::move(conditional->elseBranch), captures, envLocal,
                closureType, module);
        return stmt;
    }
    if (auto* match = dynamic_cast<MatchStmt*>(stmt.get())) {
        match->scrutinee = rewriteCaptureReadsExpr(
            std::move(match->scrutinee), captures, envLocal, closureType, module);
        for (auto& arm : match->arms)
            for (auto& statement : arm.body->stmts)
                statement = rewriteCaptureReadsStmt(
                    std::move(statement), captures, envLocal, closureType, module);
        return stmt;
    }
    if (auto* loop = dynamic_cast<WhileStmt*>(stmt.get())) {
        loop->cond = rewriteCaptureReadsExpr(
            std::move(loop->cond), captures, envLocal, closureType, module);
        for (auto& statement : loop->body->stmts)
            statement = rewriteCaptureReadsStmt(
                std::move(statement), captures, envLocal, closureType, module);
        return stmt;
    }
    if (auto* loop = dynamic_cast<ForStmt*>(stmt.get())) {
        loop->iterable = rewriteCaptureReadsExpr(
            std::move(loop->iterable), captures, envLocal, closureType, module);
        for (auto& statement : loop->body->stmts)
            statement = rewriteCaptureReadsStmt(
                std::move(statement), captures, envLocal, closureType, module);
        return stmt;
    }
    if (auto* freeStmt = dynamic_cast<FreeStmt*>(stmt.get())) {
        freeStmt->operand = rewriteCaptureReadsExpr(
            std::move(freeStmt->operand), captures, envLocal, closureType, module);
        return stmt;
    }
    if (auto* invoke = dynamic_cast<SlotInvokeStmt*>(stmt.get())) {
        for (auto& argument : invoke->args)
            argument = rewriteCaptureReadsExpr(
                std::move(argument), captures, envLocal, closureType, module);
        if (invoke->continuation)
            for (auto& statement : invoke->continuation->stmts)
                statement = rewriteCaptureReadsStmt(
                    std::move(statement), captures, envLocal, closureType, module);
        return stmt;
    }
    if (auto* awaitStmt = dynamic_cast<AwaitStmt*>(stmt.get())) {
        awaitStmt->event = rewriteCaptureReadsExpr(
            std::move(awaitStmt->event), captures, envLocal, closureType, module);
        return stmt;
    }
    if (auto* apply = dynamic_cast<ApplyStmt*>(stmt.get())) {
        for (auto& statement : apply->body->stmts)
            statement = rewriteCaptureReadsStmt(
                std::move(statement), captures, envLocal, closureType, module);
        return stmt;
    }
    return stmt;
}

void resetStructureDepthLimit() {
    gStructureExceeded = false;
}

bool structureDepthExceeded() {
    return gStructureExceeded;
}

} // namespace control_flow_builder_detail
} // namespace moon
