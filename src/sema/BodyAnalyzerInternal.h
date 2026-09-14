#pragma once

#include "BodyAnalyzer.h"

#include <unordered_set>

namespace body_analyzer_detail {

inline bool isNegativeIntegerLiteral(const Expr* expression) {
    const auto* unary = dynamic_cast<const UnaryExpr*>(expression);
    if (!unary || unary->op != TokenKind::Minus) return false;
    const auto* literal = dynamic_cast<const IntLiteralExpr*>(
        unary->operand.get());
    return literal && literal->value != 0;
}

inline bool hasLayoutDependentTypeParameter(
    const TypePtr& type,
    std::unordered_set<const Type*>& active) {
    if (!type || !active.insert(type.get()).second) return false;
    bool dependent = type->kind == TypeKind::TypeParam;
    if (!dependent && type->kind == TypeKind::Array)
        dependent = hasLayoutDependentTypeParameter(type->inner, active);
    if (!dependent && type->kind == TypeKind::Record)
        for (const auto& field : type->fields)
            dependent = dependent ||
                hasLayoutDependentTypeParameter(field.type, active);
    if (!dependent && type->kind == TypeKind::Enum)
        for (const auto& variant : type->variants)
            for (const auto& field : variant.fields)
                dependent = dependent ||
                    hasLayoutDependentTypeParameter(field, active);
    if (!dependent && type->kind == TypeKind::Result)
        for (const auto& argument : type->typeArgs)
            dependent = dependent ||
                hasLayoutDependentTypeParameter(argument, active);
    // Pointer-represented nominal products, references, raw pointers, shared
    // handles, slices, and device handles are representation barriers.
    active.erase(type.get());
    return dependent;
}

inline bool genericDropLayoutDependsOnParameter(const TypePtr& target) {
    if (!target) return false;
    std::unordered_set<const Type*> active;
    if (target->kind == TypeKind::Struct) {
        for (const auto& field : target->fields)
            if (hasLayoutDependentTypeParameter(field.type, active))
                return true;
        return false;
    }
    if (target->kind == TypeKind::Enum) {
        for (const auto& variant : target->variants)
            for (const auto& field : variant.fields)
                if (hasLayoutDependentTypeParameter(field, active))
                    return true;
    }
    return false;
}

inline bool isCompilerOnlyValue(const TypePtr& type) {
    if (!type) return false;
    switch (type->kind) {
        case TypeKind::SymbolSet:
            return true;
        case TypeKind::Enum:
            return type->domain == luna::types::TypeDomain::Compiler &&
                type->nominalId == luna::sysmeta::OptionTypeId;
        default:
            return false;
    }
}

} // namespace body_analyzer_detail
