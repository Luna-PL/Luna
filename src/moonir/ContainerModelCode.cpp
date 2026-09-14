#include "ContainerModel.h"
#include "ContainerModelInternal.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace moon {

using namespace container_detail;

namespace {

template <typename Ref>
void encodeTableRef(Encoder& encoder, Ref reference) {
    encoder.u32(reference.value);
}

template <typename Ref>
bool decodeTableRef(Decoder& decoder, Ref& reference) {
    return decoder.u32(reference.value);
}

template <typename Ref>
void encodeTableRefs(Encoder& encoder, const std::vector<Ref>& references) {
    encoder.rows(references, [&](const auto& reference) {
        encodeTableRef(encoder, reference);
    });
}

template <typename Ref>
bool decodeTableRefs(Decoder& decoder, std::vector<Ref>& references) {
    uint32_t count = 0;
    if (!decoder.rowCount(count)) return false;
    references.clear();
    for (uint32_t index = 0; index < count; ++index) {
        Ref reference;
        if (!decodeTableRef(decoder, reference)) return false;
        references.push_back(reference);
    }
    return true;
}

void encodeParam(Encoder& encoder, const Param& parameter) {
    encoder.string(parameter.name);
    encoder.boolean(parameter.isLinear);
    encoder.enumeration(parameter.usage);
    encoder.enumeration(parameter.relation);
    encoder.string(parameter.type.value);
}

bool decodeParam(Decoder& decoder, Param& parameter) {
    return decoder.string(parameter.name) &&
        decoder.boolean(parameter.isLinear) &&
        decoder.enumeration(parameter.usage, 2) &&
        decoder.enumeration(parameter.relation, 2) &&
        decoder.string(parameter.type.value);
}

bool encodeGraph(Encoder&, const ControlFlowGraph&, uint32_t,
                 const ContainerLimits&);
bool decodeGraph(Decoder&, std::unique_ptr<ControlFlowGraph>&, uint32_t,
                 const ContainerLimits&);

bool encodeExpr(Encoder& encoder, const Expr* expression, uint32_t depth,
                const ContainerLimits& limits);
bool decodeExpr(Decoder& decoder, std::unique_ptr<Expr>& expression,
                uint32_t depth, const ContainerLimits& limits);

bool encodeOptionalExpr(Encoder& encoder, const Expr* expression,
                        uint32_t depth, const ContainerLimits& limits) {
    encoder.boolean(expression != nullptr);
    return !expression || encodeExpr(encoder, expression, depth, limits);
}

bool decodeOptionalExpr(Decoder& decoder, std::unique_ptr<Expr>& expression,
                        uint32_t depth, const ContainerLimits& limits) {
    bool present = false;
    if (!decoder.boolean(present)) return false;
    if (!present) { expression.reset(); return true; }
    return decodeExpr(decoder, expression, depth, limits);
}

bool encodeExprVector(
    Encoder& encoder, const std::vector<std::unique_ptr<Expr>>& values,
    uint32_t depth, const ContainerLimits& limits) {
    encoder.rows(values, [&](const auto& value) {
        if (encoder.good()) encodeOptionalExpr(
            encoder, value.get(), depth, limits);
    });
    return encoder.good();
}

bool decodeExprVector(
    Decoder& decoder, std::vector<std::unique_ptr<Expr>>& values,
    uint32_t depth, const ContainerLimits& limits) {
    uint32_t count = 0;
    if (!decoder.rowCount(count)) return false;
    values.clear();
    for (uint32_t index = 0; index < count; ++index) {
        std::unique_ptr<Expr> value;
        if (!decodeOptionalExpr(decoder, value, depth, limits)) return false;
        values.push_back(std::move(value));
    }
    return true;
}

bool encodeExpr(Encoder& encoder, const Expr* expression, uint32_t depth,
                const ContainerLimits& limits) {
    if (!expression) return encoder.reject(
        "Moon Container code contains a null required expression");
    if (depth >= limits.maximumNestingDepth)
        return encoder.reject("Moon Container code exceeds the nesting limit");
    const auto opcode = codeExpressionOpcode(*expression);
    if (!opcode)
        return encoder.reject(
            "Moon Container code contains a structured-only expression");
    encoder.enumeration(*opcode);
    encoder.string(expression->type.value);
    encodeLocation(encoder, expression->location);
    const uint32_t nested = depth + 1;

    if (const auto* value = dynamic_cast<const IntLiteralExpr*>(expression)) {
        encoder.i64(value->value);
    } else if (const auto* value =
                   dynamic_cast<const FloatLiteralExpr*>(expression)) {
        uint64_t bits = 0;
        std::memcpy(&bits, &value->value, sizeof(bits));
        encoder.u64(bits);
    } else if (const auto* value =
                   dynamic_cast<const StringLiteralExpr*>(expression)) {
        encoder.string(value->value);
    } else if (const auto* value =
                   dynamic_cast<const BoolLiteralExpr*>(expression)) {
        encoder.boolean(value->value);
    } else if (dynamic_cast<const UnitExpr*>(expression)) {
    } else if (const auto* value =
                   dynamic_cast<const IdentifierExpr*>(expression)) {
        encoder.string(value->name);
        encodeTableRef(encoder, value->local);
        encodeReference(encoder, value->declaration);
    } else if (const auto* value =
                   dynamic_cast<const BinaryExpr*>(expression)) {
        if (!encodeOptionalExpr(encoder, value->lhs.get(), nested, limits)) return false;
        encoder.enumeration(value->op);
        if (!encodeOptionalExpr(encoder, value->rhs.get(), nested, limits)) return false;
    } else if (const auto* value =
                   dynamic_cast<const UnaryExpr*>(expression)) {
        encoder.enumeration(value->op);
        if (!encodeOptionalExpr(encoder, value->operand.get(), nested, limits)) return false;
    } else if (const auto* value = dynamic_cast<const CallExpr*>(expression)) {
        if (!encodeOptionalExpr(encoder, value->callee.get(), nested, limits) ||
            !encodeExprVector(encoder, value->args, nested, limits)) return false;
        encodeTypeRefs(encoder, value->typeArgs);
        encodeReference(encoder, value->calleeRef);
        encoder.boolean(value->returnsLinear);
        encoder.enumeration(value->returnUsage);
        encoder.string(value->intrinsicType.value);
        encoder.string(value->iteratorInputType.value);
        encoder.string(value->iteratorOutputType.value);
        encoder.enumeration(value->iteratorOp);
        encoder.string(value->iteratorRecipeStateName);
        encoder.string(value->iteratorRecipeSourceType.value);
        encoder.string(value->iteratorCollectTargetType.value);
        encoder.string(value->iteratorCollectBuilderType.value);
        encodeReference(encoder, value->iteratorCollectBegin);
        encodeReference(encoder, value->iteratorCollectPush);
        encodeReference(encoder, value->iteratorCollectFinish);
        encoder.boolean(value->compileTimeValue.has_value());
        if (value->compileTimeValue) encodeConstant(
            encoder, *value->compileTimeValue);
    } else if (const auto* value = dynamic_cast<const LaunchExpr*>(expression)) {
        encoder.string(value->kernelName);
        encodeReference(encoder, value->kernelRef);
        if (!encodeOptionalExpr(encoder, value->threads.get(), nested, limits) ||
            !encodeExprVector(encoder, value->args, nested, limits)) return false;
        encoder.rows(value->inFlightResources, [&](const auto& resource) {
            encoder.string(resource.first);
            encoder.boolean(resource.second);
        });
    } else if (const auto* value =
                   dynamic_cast<const VariantConstructExpr*>(expression)) {
        encoder.string(value->typeName);
        encoder.string(value->variantName);
        if (!encodeExprVector(encoder, value->args, nested, limits)) return false;
        encoder.string(value->constructedType.value);
    } else if (const auto* value =
                   dynamic_cast<const ResultConstructExpr*>(expression)) {
        encoder.boolean(value->isOk);
        if (!encodeOptionalExpr(encoder, value->payload.get(), nested, limits)) return false;
    } else if (const auto* value =
                   dynamic_cast<const FieldAccessExpr*>(expression)) {
        if (!encodeOptionalExpr(encoder, value->object.get(), nested, limits)) return false;
        encoder.string(value->field);
    } else if (const auto* value = dynamic_cast<const IndexExpr*>(expression)) {
        if (!encodeOptionalExpr(encoder, value->object.get(), nested, limits) ||
            !encodeOptionalExpr(encoder, value->index.get(), nested, limits)) return false;
    } else if (const auto* value =
                   dynamic_cast<const SliceLengthExpr*>(expression)) {
        if (!encodeOptionalExpr(encoder, value->slice.get(), nested, limits)) return false;
    } else if (const auto* value =
                   dynamic_cast<const ArrayLiteralExpr*>(expression)) {
        if (!encodeExprVector(encoder, value->elements, nested, limits)) return false;
        encoder.string(value->elementType.value);
    } else if (const auto* value =
                   dynamic_cast<const RecordLiteralExpr*>(expression)) {
        encoder.rows(value->fields, [&](const auto& field) {
            encoder.string(field.name);
            if (encoder.good()) encodeOptionalExpr(
                encoder, field.value.get(), nested, limits);
        });
    } else if (const auto* value =
                   dynamic_cast<const HeapAllocExpr*>(expression)) {
        if (!encodeOptionalExpr(encoder, value->initializer.get(), nested, limits)) return false;
        encoder.string(value->allocatedType.value);
        encoder.enumeration(value->storage);
    } else if (const auto* value =
                   dynamic_cast<const InitAllocationExpr*>(expression)) {
        encodeTableRef(encoder, value->allocation);
        encoder.string(value->allocatedType.value);
        encoder.enumeration(value->storage);
        encoder.rows(value->elements, [&](const auto& element) {
            encoder.u32(element.index);
            if (encoder.good()) encodeOptionalExpr(
                encoder, element.value.get(), nested, limits);
        });
    } else if (const auto* value = dynamic_cast<const MoveExpr*>(expression)) {
        if (!encodeOptionalExpr(encoder, value->operand.get(), nested, limits)) return false;
        encodeTableRef(encoder, value->nextUnread);
    } else if (const auto* value = dynamic_cast<const BorrowExpr*>(expression)) {
        encoder.boolean(value->isMutable);
        if (!encodeOptionalExpr(encoder, value->operand.get(), nested, limits)) return false;
    } else if (const auto* value = dynamic_cast<const DerefExpr*>(expression)) {
        if (!encodeOptionalExpr(encoder, value->operand.get(), nested, limits)) return false;
    } else if (const auto* value = dynamic_cast<const AddrOfExpr*>(expression)) {
        encoder.boolean(value->isMutable);
        if (!encodeOptionalExpr(encoder, value->operand.get(), nested, limits)) return false;
    } else if (const auto* value = dynamic_cast<const LambdaExpr*>(expression)) {
        if (value->body)
            return encoder.reject("Moon Container lambda retains a structured body");
        encoder.rows(value->params, [&](const auto& parameter) {
            encodeParam(encoder, parameter);
        });
        encoder.string(value->returnType.value);
        encoder.boolean(value->controlFlow != nullptr);
        if (value->controlFlow &&
            !encodeGraph(encoder, *value->controlFlow, nested, limits)) return false;
        encoder.string(value->closureType.value);
        encoder.rows(value->captures, [&](const auto& capture) {
            encoder.string(capture);
        });
        encoder.string(value->identitySuffix);
        encoder.string(value->envParamName);
    } else if (const auto* value =
                   dynamic_cast<const MakeClosureExpr*>(expression)) {
        if (!encodeOptionalExpr(encoder, value->lambda.get(), nested, limits) ||
            !encodeExprVector(encoder, value->capturedValues, nested, limits)) return false;
    } else if (const auto* value = dynamic_cast<const EnvLoadExpr*>(expression)) {
        encodeTableRef(encoder, value->envLocal);
        encoder.u64(value->fieldIndex);
    } else if (const auto* value = dynamic_cast<const AssignExpr*>(expression)) {
        encoder.enumeration(value->op);
        if (!encodeOptionalExpr(encoder, value->lhs.get(), nested, limits) ||
            !encodeOptionalExpr(encoder, value->rhs.get(), nested, limits)) return false;
    }
    return encoder.good();
}

bool decodeExpr(Decoder& decoder, std::unique_ptr<Expr>& expression,
                uint32_t depth, const ContainerLimits& limits) {
    if (depth >= limits.maximumNestingDepth)
        return decoder.reject("Moon Container code exceeds the nesting limit");
    CodeExpressionOpcode opcode;
    if (!decoder.enumeration(
            opcode, static_cast<uint32_t>(CodeExpressionOpcode::Assign)) ||
        static_cast<uint32_t>(opcode) == 0)
        return false;

    std::unique_ptr<Expr> decoded;
    switch (opcode) {
        case CodeExpressionOpcode::Integer: decoded = std::make_unique<IntLiteralExpr>(); break;
        case CodeExpressionOpcode::Floating: decoded = std::make_unique<FloatLiteralExpr>(); break;
        case CodeExpressionOpcode::String: decoded = std::make_unique<StringLiteralExpr>(); break;
        case CodeExpressionOpcode::Boolean: decoded = std::make_unique<BoolLiteralExpr>(); break;
        case CodeExpressionOpcode::Unit: decoded = std::make_unique<UnitExpr>(); break;
        case CodeExpressionOpcode::Identifier: decoded = std::make_unique<IdentifierExpr>(); break;
        case CodeExpressionOpcode::Binary: decoded = std::make_unique<BinaryExpr>(); break;
        case CodeExpressionOpcode::Unary: decoded = std::make_unique<UnaryExpr>(); break;
        case CodeExpressionOpcode::Call: decoded = std::make_unique<CallExpr>(); break;
        case CodeExpressionOpcode::ReservedDynamicSelect: return false;
        case CodeExpressionOpcode::Launch: decoded = std::make_unique<LaunchExpr>(); break;
        case CodeExpressionOpcode::VariantConstruct: decoded = std::make_unique<VariantConstructExpr>(); break;
        case CodeExpressionOpcode::ResultConstruct: decoded = std::make_unique<ResultConstructExpr>(); break;
        case CodeExpressionOpcode::FieldAccess: decoded = std::make_unique<FieldAccessExpr>(); break;
        case CodeExpressionOpcode::Index: decoded = std::make_unique<IndexExpr>(); break;
        case CodeExpressionOpcode::SliceLength: decoded = std::make_unique<SliceLengthExpr>(); break;
        case CodeExpressionOpcode::ArrayLiteral: decoded = std::make_unique<ArrayLiteralExpr>(); break;
        case CodeExpressionOpcode::RecordLiteral: decoded = std::make_unique<RecordLiteralExpr>(); break;
        case CodeExpressionOpcode::HeapAllocate: decoded = std::make_unique<HeapAllocExpr>(); break;
        case CodeExpressionOpcode::InitializeAllocation: decoded = std::make_unique<InitAllocationExpr>(); break;
        case CodeExpressionOpcode::Move: decoded = std::make_unique<MoveExpr>(); break;
        case CodeExpressionOpcode::Borrow: decoded = std::make_unique<BorrowExpr>(); break;
        case CodeExpressionOpcode::Dereference: decoded = std::make_unique<DerefExpr>(); break;
        case CodeExpressionOpcode::AddressOf: decoded = std::make_unique<AddrOfExpr>(); break;
        case CodeExpressionOpcode::Lambda: decoded = std::make_unique<LambdaExpr>(); break;
        case CodeExpressionOpcode::MakeClosure: decoded = std::make_unique<MakeClosureExpr>(); break;
        case CodeExpressionOpcode::EnvironmentLoad: decoded = std::make_unique<EnvLoadExpr>(); break;
        case CodeExpressionOpcode::Assign: decoded = std::make_unique<AssignExpr>(); break;
    }
    if (!decoded || !decoder.string(decoded->type.value) ||
        !decodeLocation(decoder, decoded->location))
        return false;
    const uint32_t nested = depth + 1;

    if (auto* value = dynamic_cast<IntLiteralExpr*>(decoded.get())) {
        if (!decoder.i64(value->value)) return false;
    } else if (auto* value = dynamic_cast<FloatLiteralExpr*>(decoded.get())) {
        uint64_t bits = 0;
        if (!decoder.u64(bits)) return false;
        std::memcpy(&value->value, &bits, sizeof(bits));
    } else if (auto* value = dynamic_cast<StringLiteralExpr*>(decoded.get())) {
        if (!decoder.string(value->value)) return false;
    } else if (auto* value = dynamic_cast<BoolLiteralExpr*>(decoded.get())) {
        if (!decoder.boolean(value->value)) return false;
    } else if (dynamic_cast<UnitExpr*>(decoded.get())) {
    } else if (auto* value = dynamic_cast<IdentifierExpr*>(decoded.get())) {
        if (!decoder.string(value->name) ||
            !decodeTableRef(decoder, value->local) ||
            !decodeReference(decoder, value->declaration)) return false;
    } else if (auto* value = dynamic_cast<BinaryExpr*>(decoded.get())) {
        if (!decodeOptionalExpr(decoder, value->lhs, nested, limits) ||
            !decoder.enumeration(
                value->op, static_cast<uint32_t>(Operator::Negate)) ||
            !decodeOptionalExpr(decoder, value->rhs, nested, limits)) return false;
    } else if (auto* value = dynamic_cast<UnaryExpr*>(decoded.get())) {
        if (!decoder.enumeration(
                value->op, static_cast<uint32_t>(Operator::Negate)) ||
            !decodeOptionalExpr(decoder, value->operand, nested, limits)) return false;
    } else if (auto* value = dynamic_cast<CallExpr*>(decoded.get())) {
        if (!decodeOptionalExpr(decoder, value->callee, nested, limits) ||
            !decodeExprVector(decoder, value->args, nested, limits) ||
            !decodeTypeRefs(decoder, value->typeArgs) ||
            !decodeReference(decoder, value->calleeRef) ||
            !decoder.boolean(value->returnsLinear) ||
            !decoder.enumeration(value->returnUsage, 2) ||
            !decoder.string(value->intrinsicType.value) ||
            !decoder.string(value->iteratorInputType.value) ||
            !decoder.string(value->iteratorOutputType.value) ||
            !decoder.enumeration(
                value->iteratorOp, static_cast<uint32_t>(IteratorOp::Collect)) ||
            !decoder.string(value->iteratorRecipeStateName) ||
            !decoder.string(value->iteratorRecipeSourceType.value) ||
            !decoder.string(value->iteratorCollectTargetType.value) ||
            !decoder.string(value->iteratorCollectBuilderType.value) ||
            !decodeReference(decoder, value->iteratorCollectBegin) ||
            !decodeReference(decoder, value->iteratorCollectPush) ||
            !decodeReference(decoder, value->iteratorCollectFinish)) return false;
        bool hasConstant = false;
        if (!decoder.boolean(hasConstant)) return false;
        if (hasConstant) {
            ConstantValue constant;
            if (!decodeConstant(decoder, constant)) return false;
            value->compileTimeValue = std::move(constant);
        }
    } else if (auto* value = dynamic_cast<LaunchExpr*>(decoded.get())) {
        if (!decoder.string(value->kernelName) ||
            !decodeReference(decoder, value->kernelRef) ||
            !decodeOptionalExpr(decoder, value->threads, nested, limits) ||
            !decodeExprVector(decoder, value->args, nested, limits)) return false;
        uint32_t count = 0;
        if (!decoder.rowCount(count)) return false;
        for (uint32_t index = 0; index < count; ++index) {
            std::pair<std::string, bool> resource;
            if (!decoder.string(resource.first) ||
                !decoder.boolean(resource.second)) return false;
            value->inFlightResources.push_back(std::move(resource));
        }
    } else if (auto* value = dynamic_cast<VariantConstructExpr*>(decoded.get())) {
        if (!decoder.string(value->typeName) ||
            !decoder.string(value->variantName) ||
            !decodeExprVector(decoder, value->args, nested, limits) ||
            !decoder.string(value->constructedType.value)) return false;
    } else if (auto* value = dynamic_cast<ResultConstructExpr*>(decoded.get())) {
        if (!decoder.boolean(value->isOk) ||
            !decodeOptionalExpr(decoder, value->payload, nested, limits)) return false;
    } else if (auto* value = dynamic_cast<FieldAccessExpr*>(decoded.get())) {
        if (!decodeOptionalExpr(decoder, value->object, nested, limits) ||
            !decoder.string(value->field)) return false;
    } else if (auto* value = dynamic_cast<IndexExpr*>(decoded.get())) {
        if (!decodeOptionalExpr(decoder, value->object, nested, limits) ||
            !decodeOptionalExpr(decoder, value->index, nested, limits)) return false;
    } else if (auto* value = dynamic_cast<SliceLengthExpr*>(decoded.get())) {
        if (!decodeOptionalExpr(decoder, value->slice, nested, limits)) return false;
    } else if (auto* value = dynamic_cast<ArrayLiteralExpr*>(decoded.get())) {
        if (!decodeExprVector(decoder, value->elements, nested, limits) ||
            !decoder.string(value->elementType.value)) return false;
    } else if (auto* value = dynamic_cast<RecordLiteralExpr*>(decoded.get())) {
        uint32_t count = 0;
        if (!decoder.rowCount(count)) return false;
        for (uint32_t index = 0; index < count; ++index) {
            RecordLiteralExpr::Field field;
            if (!decoder.string(field.name) ||
                !decodeOptionalExpr(decoder, field.value, nested, limits)) return false;
            value->fields.push_back(std::move(field));
        }
    } else if (auto* value = dynamic_cast<HeapAllocExpr*>(decoded.get())) {
        if (!decodeOptionalExpr(decoder, value->initializer, nested, limits) ||
            !decoder.string(value->allocatedType.value) ||
            !decoder.enumeration(value->storage, 0)) return false;
    } else if (auto* value = dynamic_cast<InitAllocationExpr*>(decoded.get())) {
        if (!decodeTableRef(decoder, value->allocation) ||
            !decoder.string(value->allocatedType.value) ||
            !decoder.enumeration(value->storage, 0)) return false;
        uint32_t count = 0;
        if (!decoder.rowCount(count)) return false;
        for (uint32_t index = 0; index < count; ++index) {
            InitAllocationExpr::Element element;
            if (!decoder.u32(element.index) ||
                !decodeOptionalExpr(
                    decoder, element.value, nested, limits)) return false;
            value->elements.push_back(std::move(element));
        }
    } else if (auto* value = dynamic_cast<MoveExpr*>(decoded.get())) {
        if (!decodeOptionalExpr(decoder, value->operand, nested, limits) ||
            !decodeTableRef(decoder, value->nextUnread)) return false;
    } else if (auto* value = dynamic_cast<BorrowExpr*>(decoded.get())) {
        if (!decoder.boolean(value->isMutable) ||
            !decodeOptionalExpr(decoder, value->operand, nested, limits)) return false;
    } else if (auto* value = dynamic_cast<DerefExpr*>(decoded.get())) {
        if (!decodeOptionalExpr(decoder, value->operand, nested, limits)) return false;
    } else if (auto* value = dynamic_cast<AddrOfExpr*>(decoded.get())) {
        if (!decoder.boolean(value->isMutable) ||
            !decodeOptionalExpr(decoder, value->operand, nested, limits)) return false;
    } else if (auto* value = dynamic_cast<LambdaExpr*>(decoded.get())) {
        uint32_t count = 0;
        if (!decoder.rowCount(count)) return false;
        for (uint32_t index = 0; index < count; ++index) {
            Param parameter;
            if (!decodeParam(decoder, parameter)) return false;
            value->params.push_back(std::move(parameter));
        }
        if (!decoder.string(value->returnType.value)) return false;
        bool hasGraph = false;
        if (!decoder.boolean(hasGraph)) return false;
        if (hasGraph &&
            !decodeGraph(decoder, value->controlFlow, nested, limits)) return false;
        if (!decoder.string(value->closureType.value) ||
            !decodeStringRows(decoder, value->captures) ||
            !decoder.string(value->identitySuffix) ||
            !decoder.string(value->envParamName)) return false;
    } else if (auto* value = dynamic_cast<MakeClosureExpr*>(decoded.get())) {
        std::unique_ptr<Expr> lambda;
        if (!decodeOptionalExpr(decoder, lambda, nested, limits)) return false;
        if (lambda) {
            auto* typed = dynamic_cast<LambdaExpr*>(lambda.get());
            if (!typed)
                return decoder.reject(
                    "Moon Container MakeClosure payload is not a Lambda");
            lambda.release();
            value->lambda.reset(typed);
        }
        if (!decodeExprVector(
                decoder, value->capturedValues, nested, limits)) return false;
    } else if (auto* value = dynamic_cast<EnvLoadExpr*>(decoded.get())) {
        if (!decodeTableRef(decoder, value->envLocal) ||
            !decoder.u64(value->fieldIndex)) return false;
    } else if (auto* value = dynamic_cast<AssignExpr*>(decoded.get())) {
        if (!decoder.enumeration(
                value->op, static_cast<uint32_t>(Operator::Negate)) ||
            !decodeOptionalExpr(decoder, value->lhs, nested, limits) ||
            !decodeOptionalExpr(decoder, value->rhs, nested, limits)) return false;
    }
    expression = std::move(decoded);
    return true;
}

void encodePlace(Encoder& encoder, const PlaceRef& place) {
    encodeTableRef(encoder, place.root);
    encoder.rows(place.projections, [&](const auto& projection) {
        encoder.enumeration(projection.kind);
        encoder.u64(projection.index);
        encodeTableRef(encoder, projection.dynamicIndex);
    });
}

bool decodePlace(Decoder& decoder, PlaceRef& place) {
    if (!decodeTableRef(decoder, place.root)) return false;
    uint32_t count = 0;
    if (!decoder.rowCount(count)) return false;
    for (uint32_t index = 0; index < count; ++index) {
        PlaceProjection projection;
        if (!decoder.enumeration(projection.kind, 3) ||
            !decoder.u64(projection.index) ||
            !decodeTableRef(decoder, projection.dynamicIndex)) return false;
        place.projections.push_back(projection);
    }
    return true;
}

void encodeEdge(Encoder& encoder, const ControlEdge& edge) {
    encodeTableRef(encoder, edge.target);
    encodeTableRefs(encoder, edge.cleanups);
}

bool decodeEdge(Decoder& decoder, ControlEdge& edge) {
    return decodeTableRef(decoder, edge.target) &&
        decodeTableRefs(decoder, edge.cleanups);
}

bool encodeOperation(Encoder& encoder, const Stmt& operation, uint32_t depth,
                     const ContainerLimits& limits) {
    const auto opcode = codeOperationOpcode(operation);
    if (!opcode)
        return encoder.reject(
            "Moon Container sealed CFG contains a structured operation");
    encoder.enumeration(*opcode);
    encodeLocation(encoder, operation.location);
    if (const auto* value = dynamic_cast<const LetStmt*>(&operation)) {
        encoder.string(value->name);
        encodeTableRef(encoder, value->local);
        encoder.boolean(value->isConst);
        encoder.boolean(value->isLinear);
        encoder.enumeration(value->usage);
        encoder.boolean(value->relation.has_value());
        if (value->relation) encoder.enumeration(*value->relation);
        encoder.string(value->type.value);
        if (!encodeOptionalExpr(
                encoder, value->initializer.get(), depth, limits)) return false;
        encoder.boolean(value->materializesIteratorRecipe);
        encoder.boolean(value->materializedIteratorOwnsSource);
        encoder.string(value->materializedIteratorSourceType.value);
    } else if (const auto* value = dynamic_cast<const AllocateStmt*>(&operation)) {
        encodeTableRef(encoder, value->local);
        encoder.string(value->allocatedType.value);
        encoder.enumeration(value->storage);
    } else if (const auto* value = dynamic_cast<const ExprStmt*>(&operation)) {
        if (!encodeOptionalExpr(encoder, value->expr.get(), depth, limits)) return false;
    } else if (const auto* value = dynamic_cast<const FreeStmt*>(&operation)) {
        if (!encodeOptionalExpr(encoder, value->operand.get(), depth, limits)) return false;
        encoder.enumeration(value->action);
        encoder.boolean(value->isImplicit);
    } else if (const auto* value = dynamic_cast<const AwaitStmt*>(&operation)) {
        if (!encodeOptionalExpr(encoder, value->event.get(), depth, limits)) return false;
    }
    return encoder.good();
}

bool decodeOperation(Decoder& decoder, std::unique_ptr<Stmt>& operation,
                     uint32_t depth, const ContainerLimits& limits) {
    CodeOperationOpcode opcode;
    if (!decoder.enumeration(
            opcode, static_cast<uint32_t>(CodeOperationOpcode::Await)) ||
        static_cast<uint32_t>(opcode) == 0)
        return false;
    std::unique_ptr<Stmt> decoded;
    switch (opcode) {
        case CodeOperationOpcode::Let: decoded = std::make_unique<LetStmt>(); break;
        case CodeOperationOpcode::Allocate: decoded = std::make_unique<AllocateStmt>(); break;
        case CodeOperationOpcode::Expression: decoded = std::make_unique<ExprStmt>(); break;
        case CodeOperationOpcode::Free: decoded = std::make_unique<FreeStmt>(); break;
        case CodeOperationOpcode::Await: decoded = std::make_unique<AwaitStmt>(); break;
    }
    if (!decoded || !decodeLocation(decoder, decoded->location)) return false;
    if (auto* value = dynamic_cast<LetStmt*>(decoded.get())) {
        if (!decoder.string(value->name) ||
            !decodeTableRef(decoder, value->local) ||
            !decoder.boolean(value->isConst) ||
            !decoder.boolean(value->isLinear) ||
            !decoder.enumeration(value->usage, 2)) return false;
        bool hasRelation = false;
        if (!decoder.boolean(hasRelation)) return false;
        if (hasRelation) {
            luna::ownership::Relation relation;
            if (!decoder.enumeration(relation, 2)) return false;
            value->relation = relation;
        }
        if (!decoder.string(value->type.value) ||
            !decodeOptionalExpr(decoder, value->initializer, depth, limits) ||
            !decoder.boolean(value->materializesIteratorRecipe) ||
            !decoder.boolean(value->materializedIteratorOwnsSource) ||
            !decoder.string(value->materializedIteratorSourceType.value)) return false;
    } else if (auto* value = dynamic_cast<AllocateStmt*>(decoded.get())) {
        if (!decodeTableRef(decoder, value->local) ||
            !decoder.string(value->allocatedType.value) ||
            !decoder.enumeration(value->storage, 0)) return false;
    } else if (auto* value = dynamic_cast<ExprStmt*>(decoded.get())) {
        if (!decodeOptionalExpr(decoder, value->expr, depth, limits)) return false;
    } else if (auto* value = dynamic_cast<FreeStmt*>(decoded.get())) {
        if (!decodeOptionalExpr(decoder, value->operand, depth, limits) ||
            !decoder.enumeration(value->action, 7) ||
            !decoder.boolean(value->isImplicit)) return false;
    } else if (auto* value = dynamic_cast<AwaitStmt*>(decoded.get())) {
        if (!decodeOptionalExpr(decoder, value->event, depth, limits)) return false;
    }
    operation = std::move(decoded);
    return true;
}

bool encodeTerminator(Encoder& encoder, const Terminator& terminator,
                      uint32_t depth, const ContainerLimits& limits) {
    encoder.enumeration(terminator.kind);
    encodeLocation(encoder, terminator.location);
    if (!encodeOptionalExpr(
            encoder, terminator.operand.get(), depth, limits)) return false;
    encoder.string(terminator.switchType.value);
    encodeEdge(encoder, terminator.primary);
    encodeEdge(encoder, terminator.secondary);
    encoder.rows(terminator.cases, [&](const auto& switchCase) {
        encoder.u32(switchCase.tag);
        encodeEdge(encoder, switchCase.edge);
        encodeTableRefs(encoder, switchCase.bindings);
    });
    encodeTableRefs(encoder, terminator.exitCleanups);
    return encoder.good();
}

bool decodeTerminator(Decoder& decoder, Terminator& terminator,
                      uint32_t depth, const ContainerLimits& limits) {
    if (!decoder.enumeration(terminator.kind, 7) ||
        !decodeLocation(decoder, terminator.location) ||
        !decodeOptionalExpr(decoder, terminator.operand, depth, limits) ||
        !decoder.string(terminator.switchType.value) ||
        !decodeEdge(decoder, terminator.primary) ||
        !decodeEdge(decoder, terminator.secondary)) return false;
    uint32_t count = 0;
    if (!decoder.rowCount(count)) return false;
    for (uint32_t index = 0; index < count; ++index) {
        SwitchEdge switchCase;
        if (!decoder.u32(switchCase.tag) ||
            !decodeEdge(decoder, switchCase.edge) ||
            !decodeTableRefs(decoder, switchCase.bindings)) return false;
        terminator.cases.push_back(std::move(switchCase));
    }
    return decodeTableRefs(decoder, terminator.exitCleanups);
}

bool encodeGraph(Encoder& encoder, const ControlFlowGraph& graph,
                 uint32_t depth, const ContainerLimits& limits) {
    if (depth >= limits.maximumNestingDepth)
        return encoder.reject("Moon Container code exceeds the nesting limit");
    if (!graph.sealed)
        return encoder.reject("Moon Container code contains an unsealed CFG");
    encoder.boolean(graph.sealed);
    encodeTableRef(encoder, graph.entry);
    encodeTableRef(encoder, graph.rootRegion);
    encodeTableRef(encoder, graph.rootScope);
    encoder.rows(graph.blocks, [&](const auto& block) {
        encodeTableRef(encoder, block.id);
        encodeTableRef(encoder, block.region);
        encodeTableRef(encoder, block.scope);
        encodeLocation(encoder, block.location);
        encoder.rows(block.operations, [&](const auto& operation) {
            if (!operation) {
                encoder.reject("Moon Container CFG contains a null operation");
            } else if (encoder.good()) {
                encodeOperation(encoder, *operation, depth, limits);
            }
        });
        if (encoder.good()) encodeTerminator(
            encoder, block.terminator, depth, limits);
    });
    encoder.rows(graph.regions, [&](const auto& region) {
        encodeTableRef(encoder, region.id);
        encodeTableRef(encoder, region.parent);
        encoder.enumeration(region.kind);
        encodeTableRef(encoder, region.scope);
        encodeTableRef(encoder, region.entry);
        encodeTableRef(encoder, region.exit);
        encodeTableRefs(encoder, region.blocks);
        encodeLocation(encoder, region.location);
        encodeReference(encoder, region.fragment);
        encodeTableRefs(encoder, region.parameters);
    });
    encoder.rows(graph.scopes, [&](const auto& scope) {
        encodeTableRef(encoder, scope.id);
        encodeTableRef(encoder, scope.parent);
        encodeTableRef(encoder, scope.region);
        encodeTableRefs(encoder, scope.locals);
        encodeTableRefs(encoder, scope.cleanups);
        encodeLocation(encoder, scope.location);
    });
    encoder.rows(graph.locals, [&](const auto& local) {
        encodeTableRef(encoder, local.id);
        encodeTableRef(encoder, local.scope);
        encoder.enumeration(local.kind);
        encoder.string(local.name);
        encoder.string(local.type.value);
        encoder.enumeration(local.usage);
        encoder.enumeration(local.relation);
    });
    encoder.rows(graph.cleanups, [&](const auto& cleanup) {
        encodeTableRef(encoder, cleanup.id);
        encodeTableRef(encoder, cleanup.scope);
        encodePlace(encoder, cleanup.place);
        encoder.string(cleanup.type.value);
        encoder.enumeration(cleanup.kind);
        encoder.enumeration(cleanup.action);
        encoder.boolean(cleanup.guard.has_value());
        if (cleanup.guard) {
            encodeTableRef(encoder, cleanup.guard->nextUnread);
            encoder.u64(cleanup.guard->elementIndex);
        }
    });
    return encoder.good();
}

bool decodeGraph(Decoder& decoder, std::unique_ptr<ControlFlowGraph>& graph,
                 uint32_t depth, const ContainerLimits& limits) {
    if (depth >= limits.maximumNestingDepth)
        return decoder.reject("Moon Container code exceeds the nesting limit");
    auto decoded = std::make_unique<ControlFlowGraph>();
    if (!decoder.boolean(decoded->sealed) || !decoded->sealed ||
        !decodeTableRef(decoder, decoded->entry) ||
        !decodeTableRef(decoder, decoded->rootRegion) ||
        !decodeTableRef(decoder, decoded->rootScope))
        return decoder.reject("Moon Container code contains an unsealed or truncated CFG");
    uint32_t count = 0;
    if (!decoder.rowCount(count)) return false;
    for (uint32_t index = 0; index < count; ++index) {
        BasicBlock block;
        if (!decodeTableRef(decoder, block.id) || block.id.value != index ||
            !decodeTableRef(decoder, block.region) ||
            !decodeTableRef(decoder, block.scope) ||
            !decodeLocation(decoder, block.location))
            return decoder.reject("Moon Container block table is not canonical");
        uint32_t operationCount = 0;
        if (!decoder.rowCount(operationCount)) return false;
        for (uint32_t operationIndex = 0;
             operationIndex < operationCount; ++operationIndex) {
            std::unique_ptr<Stmt> operation;
            if (!decodeOperation(
                    decoder, operation, depth, limits)) return false;
            block.operations.push_back(std::move(operation));
        }
        if (!decodeTerminator(
                decoder, block.terminator, depth, limits)) return false;
        decoded->blocks.push_back(std::move(block));
    }
    if (!decoder.rowCount(count)) return false;
    for (uint32_t index = 0; index < count; ++index) {
        RegionRecord region;
        if (!decodeTableRef(decoder, region.id) || region.id.value != index ||
            !decodeTableRef(decoder, region.parent) ||
            !decoder.enumeration(region.kind, 7) ||
            !decodeTableRef(decoder, region.scope) ||
            !decodeTableRef(decoder, region.entry) ||
            !decodeTableRef(decoder, region.exit) ||
            !decodeTableRefs(decoder, region.blocks) ||
            !decodeLocation(decoder, region.location) ||
            !decodeReference(decoder, region.fragment) ||
            !decodeTableRefs(decoder, region.parameters))
            return decoder.reject("Moon Container region table is not canonical");
        decoded->regions.push_back(std::move(region));
    }
    if (!decoder.rowCount(count)) return false;
    for (uint32_t index = 0; index < count; ++index) {
        ScopeRecord scope;
        if (!decodeTableRef(decoder, scope.id) || scope.id.value != index ||
            !decodeTableRef(decoder, scope.parent) ||
            !decodeTableRef(decoder, scope.region) ||
            !decodeTableRefs(decoder, scope.locals) ||
            !decodeTableRefs(decoder, scope.cleanups) ||
            !decodeLocation(decoder, scope.location))
            return decoder.reject("Moon Container scope table is not canonical");
        decoded->scopes.push_back(std::move(scope));
    }
    if (!decoder.rowCount(count)) return false;
    for (uint32_t index = 0; index < count; ++index) {
        LocalRecord local;
        if (!decodeTableRef(decoder, local.id) || local.id.value != index ||
            !decodeTableRef(decoder, local.scope) ||
            !decoder.enumeration(local.kind, 4) ||
            !decoder.string(local.name) ||
            !decoder.string(local.type.value) ||
            !decoder.enumeration(local.usage, 2) ||
            !decoder.enumeration(local.relation, 2))
            return decoder.reject("Moon Container local table is not canonical");
        decoded->locals.push_back(std::move(local));
    }
    if (!decoder.rowCount(count)) return false;
    for (uint32_t index = 0; index < count; ++index) {
        CleanupRecord cleanup;
        if (!decodeTableRef(decoder, cleanup.id) || cleanup.id.value != index ||
            !decodeTableRef(decoder, cleanup.scope) ||
            !decodePlace(decoder, cleanup.place) ||
            !decoder.string(cleanup.type.value) ||
            !decoder.enumeration(cleanup.kind, 1) ||
            !decoder.enumeration(cleanup.action, 7))
            return decoder.reject("Moon Container cleanup table is not canonical");
        bool hasGuard = false;
        if (!decoder.boolean(hasGuard)) return false;
        if (hasGuard) {
            CleanupGuard guard;
            if (!decodeTableRef(decoder, guard.nextUnread) ||
                !decoder.u64(guard.elementIndex)) return false;
            cleanup.guard = guard;
        }
        decoded->cleanups.push_back(std::move(cleanup));
    }
    graph = std::move(decoded);
    return true;
}

bool encodeFunction(
    Encoder& encoder, const FunctionDecl& function,
    const ContainerLimits& limits) {
    encodeReference(encoder, {function.symbolId, function.contractId});
    encoder.string(function.packageId);
    encoder.string(function.modulePath);
    encoder.string(function.name);
    encoder.string(function.generatedSymbolName);
    encoder.boolean(function.isKernel);
    encoder.boolean(function.isCodegenReachable);
    encoder.boolean(function.isExtern);
    encoder.boolean(function.isConstexpr);
    encoder.boolean(function.isSelector);
    encoder.string(function.abi);
    encoder.string(function.linkName);
    encoder.rows(function.typeParams, [&](const auto& parameter) {
        encoder.string(parameter);
    });
    encoder.rows(function.params, [&](const auto& parameter) {
        encodeParam(encoder, parameter);
    });
    encoder.string(function.returnType.value);
    encoder.boolean(function.returnsLinear);
    encoder.enumeration(function.returnUsage);
    encoder.boolean(function.isTemplateInstance);
    encoder.rows(function.concreteTypeArgs, [&](const auto& argument) {
        encoder.string(argument.value);
    });
    encodeLocation(encoder, function.location);
    encoder.boolean(function.controlFlow != nullptr);
    return !function.controlFlow ||
        encodeGraph(encoder, *function.controlFlow, 0, limits);
}

bool decodeFunction(
    Decoder& decoder, std::unique_ptr<FunctionDecl>& function,
    DeclarationRef& reference, const ContainerLimits& limits) {
    auto decoded = std::make_unique<FunctionDecl>();
    if (!decodeReference(decoder, reference) ||
        !decoder.string(decoded->packageId) ||
        !decoder.string(decoded->modulePath) ||
        !decoder.string(decoded->name) ||
        !decoder.string(decoded->generatedSymbolName) ||
        !decoder.boolean(decoded->isKernel) ||
        !decoder.boolean(decoded->isCodegenReachable) ||
        !decoder.boolean(decoded->isExtern) ||
        !decoder.boolean(decoded->isConstexpr) ||
        !decoder.boolean(decoded->isSelector) ||
        !decoder.string(decoded->abi) ||
        !decoder.string(decoded->linkName)) return false;

    uint32_t count = 0;
    if (!decoder.rowCount(count)) return false;
    for (uint32_t index = 0; index < count; ++index) {
        std::string parameter;
        if (!decoder.string(parameter)) return false;
        decoded->typeParams.push_back(std::move(parameter));
    }
    if (!decoder.rowCount(count)) return false;
    for (uint32_t index = 0; index < count; ++index) {
        Param parameter;
        if (!decodeParam(decoder, parameter)) return false;
        decoded->params.push_back(std::move(parameter));
    }
    if (!decoder.string(decoded->returnType.value) ||
        !decoder.boolean(decoded->returnsLinear) ||
        !decoder.enumeration(decoded->returnUsage, 2) ||
        !decoder.boolean(decoded->isTemplateInstance) ||
        !decoder.rowCount(count)) return false;
    for (uint32_t index = 0; index < count; ++index) {
        TypeRef argument;
        if (!decoder.string(argument.value)) return false;
        decoded->concreteTypeArgs.push_back(std::move(argument));
    }
    if (!decodeLocation(decoder, decoded->location)) return false;
    bool hasGraph = false;
    if (!decoder.boolean(hasGraph)) return false;
    if (hasGraph &&
        !decodeGraph(decoder, decoded->controlFlow, 0, limits)) return false;
    function = std::move(decoded);
    return true;
}

} // namespace
bool container_detail::encodeCodeRows(
    const Module& source, const Module& declarationModel,
    std::vector<uint8_t>& output,
    std::string& error, const ContainerLimits& limits) {
    std::vector<const FunctionDecl*> functions;
    collectConcreteFunctions(source.declarations, functions);
    functions.erase(std::remove_if(
        functions.begin(), functions.end(), [&](const auto* function) {
            return !findDeclarationRecord(
                declarationModel,
                {function->symbolId, function->contractId});
        }), functions.end());
    std::sort(functions.begin(), functions.end(), [](const auto* left,
                                                     const auto* right) {
        return left->symbolId.value < right->symbolId.value;
    });

    std::string previousSymbol;
    for (const auto* function : functions) {
        if (function->symbolId.empty() || function->contractId.empty()) {
            error = "Moon Container function has no complete declaration reference";
            output.clear(); return false;
        }
        if (!previousSymbol.empty() &&
            function->symbolId.value <= previousSymbol) {
            error = "Moon Container functions have duplicate SymbolIds";
            output.clear(); return false;
        }
        previousSymbol = function->symbolId.value;
        const auto* declaration = findDeclarationRecord(
            declarationModel, {function->symbolId, function->contractId});
        if (!declaration || declaration->kind != DeclarationKind::Function ||
            declaration->id != function->declarationId ||
            declaration->familyId != function->familyId ||
            declaration->sourceName != function->name ||
            declaration->linkageName != function->generatedSymbolName) {
            error = "Moon Container function disagrees with its declaration row";
            output.clear(); return false;
        }
        if (function->body) {
            error = "Moon Container code cannot contain a structured function body";
            output.clear(); return false;
        }
        if (function->isExtern == static_cast<bool>(function->controlFlow)) {
            error = function->isExtern
                ? "Moon Container extern function carries a CFG"
                : "Moon Container concrete function has no CFG";
            output.clear(); return false;
        }
        if (function->controlFlow && !function->controlFlow->sealed) {
            error = "Moon Container function carries an unsealed CFG";
            output.clear(); return false;
        }
    }

    Encoder encoder(limits);
    encoder.rows(functions, [&](const auto* function) {
        if (encoder.good()) encodeFunction(encoder, *function, limits);
    });
    if (!encoder.good()) {
        error = encoder.error(); output.clear(); return false;
    }
    output = encoder.finish(); error.clear(); return true;
}

bool ContainerModelCodec::encodeCode(
    const Module& module, std::vector<uint8_t>& output,
    std::string& error, const ContainerLimits& limits) {
    return encodeCodeRows(module, module, output, error, limits);
}

bool ContainerModelCodec::decodeCode(
    const std::vector<uint8_t>& input, Module& module,
    std::string& error, const ContainerLimits& limits) {
    if (input.size() > limits.maximumContainerBytes) {
        error = "Moon Container code payload exceeds the configured byte limit";
        return false;
    }
    Decoder decoder(input, limits);
    uint32_t count = 0;
    if (!decoder.rowCount(count)) {
        error = decoder.error(); return false;
    }

    std::vector<std::unique_ptr<Decl>> declarations;
    std::string previousSymbol;
    for (uint32_t index = 0; index < count; ++index) {
        std::unique_ptr<FunctionDecl> function;
        DeclarationRef reference;
        if (!decodeFunction(decoder, function, reference, limits)) {
            error = decoder.error().empty()
                ? "Moon Container function row contains an invalid scalar"
                : decoder.error();
            return false;
        }
        if (!reference.complete() ||
            (!previousSymbol.empty() &&
             reference.symbol.value <= previousSymbol)) {
            error = "Moon Container functions are incomplete, duplicate, or out of order";
            return false;
        }
        previousSymbol = reference.symbol.value;
        const auto* declaration = findDeclarationRecord(module, reference);
        if (!declaration || declaration->kind != DeclarationKind::Function ||
            declaration->sourceName != function->name ||
            declaration->linkageName != function->generatedSymbolName) {
            error = "Moon Container function does not match its declaration row";
            return false;
        }
        if (isGenericRecipe(*function)) {
            error = "Moon Container code contains a generic function recipe";
            return false;
        }
        if (function->isExtern == static_cast<bool>(function->controlFlow)) {
            error = function->isExtern
                ? "Moon Container extern function carries a CFG"
                : "Moon Container concrete function has no CFG";
            return false;
        }
        function->declarationId = declaration->id;
        function->familyId = declaration->familyId;
        function->symbolId = declaration->symbolId;
        function->contractId = declaration->contractId;
        function->retention = declaration->retention;
        function->metadata = declaration->metadata;
        function->sysmeta = declaration->sysmeta;
        function->isExported = std::any_of(
            module.exports.begin(), module.exports.end(),
            [&](const auto& exported) {
                return exported.declaration == reference;
            });
        declarations.push_back(std::move(function));
    }
    if (!decoder.finish("code")) {
        error = decoder.error(); return false;
    }

    module.declarations = std::move(declarations);
    module.rebuildIndexes();
    error.clear();
    return true;
}


} // namespace moon
