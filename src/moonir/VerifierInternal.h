#pragma once

#include "MoonIR.h"

#include <algorithm>
#include <cctype>
#include <variant>

namespace moon::verifier_detail {

inline bool isGeneric(const FunctionDecl& function) {
    return !function.typeParams.empty() && !function.isTemplateInstance;
}

inline bool isIntegerMetadataType(TypeKind kind) {
    switch (kind) {
        case TypeKind::I8:
        case TypeKind::I16:
        case TypeKind::I32:
        case TypeKind::I64:
        case TypeKind::U8:
        case TypeKind::U16:
        case TypeKind::U32:
        case TypeKind::U64:
        case TypeKind::USize:
        case TypeKind::ISize:
            return true;
        default:
            return false;
    }
}

inline bool metadataConstantMatches(
    const ConstantValue& value, const TypeRecord* type) {
    if (!type) return false;
    if (std::holds_alternative<int64_t>(value))
        return isIntegerMetadataType(type->kind);
    if (std::holds_alternative<double>(value))
        return type->kind == TypeKind::F32 || type->kind == TypeKind::F64;
    if (std::holds_alternative<bool>(value))
        return type->kind == TypeKind::Bool;
    if (std::holds_alternative<std::string>(value))
        return type->kind == TypeKind::String || type->kind == TypeKind::CStr;
    return false;
}

inline luna::ownership::Usage frozenUsage(
    const Module& module, const TypeRef& reference) {
    const auto* type = module.findType(reference);
    return type ? type->sysmeta.resource.usage
                : luna::ownership::Usage::Copy;
}

inline bool validIdentifier(const std::string& value) {
    if (value.empty() ||
        (!std::isalpha(static_cast<unsigned char>(value[0])) &&
         value[0] != '_'))
        return false;
    return std::all_of(
        value.begin() + 1, value.end(), [](unsigned char character) {
            return std::isalnum(character) || character == '_';
        });
}

inline bool validSeparatedName(
    const std::string& value, const std::string& separator,
    bool emptyAllowed = false) {
    if (value.empty()) return emptyAllowed;
    size_t begin = 0;
    for (;;) {
        const size_t end = value.find(separator, begin);
        const std::string component = value.substr(
            begin,
            end == std::string::npos ? std::string::npos : end - begin);
        if (!validIdentifier(component)) return false;
        if (end == std::string::npos) return true;
        begin = end + separator.size();
    }
}

} // namespace moon::verifier_detail
