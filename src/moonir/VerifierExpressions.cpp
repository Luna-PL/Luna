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

void Verifier::verifyExpr(const Expr* expr, const Module& module,
                          const std::string& owner) {
    if (!expr) {
        error({}, "null expression in '" + owner + "'");
        return;
    }
    if (auto* literal = dynamic_cast<const IntLiteralExpr*>(expr)) {
        verifyType(literal->type, literal->location,
                   "integer literal expression", module);
        const auto* type = module.findType(literal->type);
        if (type && !isIntegerMetadataType(type->kind))
            error(literal->location,
                  "canonical integer literal does not have an integer type");
    } else if (auto* literal = dynamic_cast<const FloatLiteralExpr*>(expr)) {
        verifyType(literal->type, literal->location,
                   "floating-point literal expression", module);
        const auto* type = module.findType(literal->type);
        if (type && type->kind != TypeKind::F32 && type->kind != TypeKind::F64)
            error(literal->location,
                  "canonical floating-point literal does not have a floating-point type");
    } else if (auto* literal = dynamic_cast<const StringLiteralExpr*>(expr)) {
        verifyType(literal->type, literal->location,
                   "string literal expression", module);
        const auto* type = module.findType(literal->type);
        if (type && type->kind != TypeKind::String &&
            type->kind != TypeKind::CStr)
            error(literal->location,
                  "canonical string literal does not have a string type");
    } else if (auto* literal = dynamic_cast<const BoolLiteralExpr*>(expr)) {
        verifyType(literal->type, literal->location,
                   "boolean literal expression", module);
        const auto* type = module.findType(literal->type);
        if (type && type->kind != TypeKind::Bool)
            error(literal->location,
                  "canonical boolean literal does not have boolean type");
    } else if (auto* unit = dynamic_cast<const UnitExpr*>(expr)) {
        verifyType(unit->type, unit->location, "unit expression", module);
        const auto* type = module.findType(unit->type);
        if (!type || type->kind != TypeKind::Unit)
            error(unit->location,
                  "canonical unit expression does not have unit type");
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
        if (!closure ||
            (closure->kind != TypeKind::Function &&
             closure->kind != TypeKind::Closure)) {
            error(lambda->location,
                  "lambda closure type is not a frozen function or closure type");
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
            if (closure->kind == TypeKind::Closure) {
                if (closure->capturedFields.size() !=
                    lambda->captures.size())
                    error(lambda->location,
                          "lambda capture list disagrees with its closure environment");
                const size_t comparableCaptures = std::min(
                    closure->capturedFields.size(),
                    lambda->captures.size());
                for (size_t index = 0; index < comparableCaptures; ++index)
                    if (lambda->captures[index] !=
                        closure->capturedFields[index].name)
                        error(lambda->location,
                              "lambda capture name disagrees with its closure environment");
                if (lambda->envParamName.empty())
                    error(lambda->location,
                          "capturing lambda has no environment parameter name");
            } else if (!lambda->captures.empty()) {
                error(lambda->location,
                      "capture-free lambda records captures");
            } else if (!lambda->envParamName.empty()) {
                error(lambda->location,
                      "capture-free lambda declares an environment parameter");
            }
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
        if (!lambda->captures.empty() &&
            (!closure || closure->kind != TypeKind::Closure))
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
            const bool hasEnvironment =
                !lambda->envParamName.empty();
            const size_t expectedArity =
                lambda->params.size() + (hasEnvironment ? 1 : 0);
            if (parameters.size() != expectedArity) {
                error(lambda->location,
                      "lambda canonical body parameter table has the wrong arity");
            } else {
                size_t offset = 0;
                if (hasEnvironment) {
                    const auto& actual = *parameters[0];
                    if (actual.scope != lambda->controlFlow->rootScope ||
                        actual.name != lambda->envParamName ||
                        actual.type != lambda->closureType)
                        error(lambda->location,
                              "lambda canonical environment parameter disagrees with its closure type");
                    offset = 1;
                }
                for (size_t index = 0;
                     index < lambda->params.size(); ++index) {
                    const auto& expected = lambda->params[index];
                    const auto& actual = *parameters[index + offset];
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
    } else if (auto* closureExpr =
                   dynamic_cast<const MakeClosureExpr*>(expr)) {
        verifyType(closureExpr->type, closureExpr->location,
                   "closure construction type", module);
        const auto* closureType = module.findType(closureExpr->type);
        if (!closureType || closureType->kind != TypeKind::Closure)
            error(closureExpr->location,
                  "closure construction has no frozen closure type");
        if (!closureExpr->lambda)
            error(closureExpr->location,
                  "closure construction has no lambda executable");
        else {
            if (closureExpr->lambda->closureType != closureExpr->type)
                error(closureExpr->location,
                      "closure construction type disagrees with its lambda type");
            verifyExpr(closureExpr->lambda.get(), module, owner);
        }
        if (closureType &&
            closureType->capturedFields.size() !=
                closureExpr->capturedValues.size())
            error(closureExpr->location,
                  "closure environment arity disagrees with its closure type");
        const size_t comparableValues = std::min(
            closureType ? closureType->capturedFields.size() : 0,
            closureExpr->capturedValues.size());
        for (size_t index = 0; index < comparableValues; ++index) {
            const auto& value = closureExpr->capturedValues[index];
            if (!value) {
                error(closureExpr->location,
                      "closure captured value is null");
                continue;
            }
            verifyExpr(value.get(), module, owner);
            if (closureType &&
                value->type !=
                    closureType->capturedFields[index].type)
                error(value->location,
                      "closure captured value type disagrees with its environment field");
            if (closureType &&
                dynamic_cast<const IdentifierExpr*>(value.get()) &&
                static_cast<const IdentifierExpr*>(value.get())->name !=
                    closureType->capturedFields[index].name)
                error(value->location,
                      "closure captured value name disagrees with its environment field");
            if (!dynamic_cast<const IdentifierExpr*>(value.get()) &&
                !dynamic_cast<const EnvLoadExpr*>(value.get()))
                error(value->location,
                      "closure captured value is not a local read");
        }
    } else if (auto* envLoad = dynamic_cast<const EnvLoadExpr*>(expr)) {
        verifyType(envLoad->type, envLoad->location,
                   "environment load type", module);
        if (envLoad->envLocal.empty())
            error(envLoad->location,
                  "environment load has no environment local");
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
    for (const auto& field : type->capturedFields) {
        verifyType(field.type, location,
                   context + ".capture." + field.name, module,
                   allowTypeParameter);
    }
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
