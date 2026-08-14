#pragma once

#include "TypeSystem.h"
#include "../parser/AST.h"

#include <iomanip>
#include <sstream>
#include <unordered_set>

inline std::string displayTraitRef(const TraitRef& trait) {
    return trait.name;
}

inline std::string nominalDeclarationIdentity(
    const Program* program, const char* kind, const std::string& symbol,
    const Decl* declaration = nullptr) {
    std::string owner = declaration && !declaration->packageId.empty()
        ? declaration->packageId
        : (program && !program->packageName.empty() ? program->packageName : "main");
    if (declaration && !declaration->modulePath.empty())
        owner += "::" + declaration->modulePath;
    return owner + "::" + kind + "::" + symbol;
}

inline uint64_t stableMetadataHash(const std::string& value) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline std::string metadataExpressionKey(const Expr* expression) {
    if (auto* value = dynamic_cast<const IntLiteralExpr*>(expression))
        return "i:" + std::to_string(value->value);
    if (auto* value = dynamic_cast<const FloatLiteralExpr*>(expression)) {
        std::ostringstream output;
        output << "f:" << std::setprecision(17) << value->value;
        return output.str();
    }
    if (auto* value = dynamic_cast<const BoolLiteralExpr*>(expression))
        return value->value ? "b:1" : "b:0";
    if (auto* value = dynamic_cast<const StringLiteralExpr*>(expression))
        return "s:" + std::to_string(value->value.size()) + ":" + value->value;
    if (auto* value = dynamic_cast<const IdentifierExpr*>(expression))
        return "id:" + value->name;
    return "expr@" + expression->sourcePath + ":" +
           std::to_string(expression->line) + ":" + std::to_string(expression->col);
}

inline std::string metadataDeclarationName(const std::string& base,
                                           const Decl* declaration) {
    if (!declaration || declaration->metadata.empty()) return base;
    std::string key;
    for (const auto& attachment : declaration->metadata) {
        key += attachment.schemaName + "(";
        for (const auto& argument : attachment.arguments)
            key += metadataExpressionKey(argument.get()) + ";";
        key += ")";
    }
    std::ostringstream output;
    output << base << "__meta_" << std::hex << std::setw(16)
           << std::setfill('0') << stableMetadataHash(key);
    return output.str();
}

inline std::string effectivePackageId(const Program* program, const Decl* declaration) {
    if (declaration && !declaration->packageId.empty()) return declaration->packageId;
    if (program && !program->packageName.empty()) return program->packageName;
    return "main";
}

inline std::string nominalTypeOwner(const TypePtr& type) {
    if (!type || type->nominalId.empty()) return {};
    const size_t separator = type->nominalId.find("::");
    return separator == std::string::npos
        ? std::string{} : type->nominalId.substr(0, separator);
}

inline std::string qualifiedDeclarationKey(const std::string& packageId,
                                           const std::string& modulePath,
                                           const std::string& name) {
    return packageId + "::" + (modulePath.empty() ? "" : modulePath + "::") + name;
}

inline std::vector<std::string> splitQualifiedName(const std::string& name) {
    std::vector<std::string> parts;
    size_t begin = 0;
    while (begin <= name.size()) {
        const size_t separator = name.find("::", begin);
        parts.push_back(name.substr(begin, separator == std::string::npos
            ? std::string::npos : separator - begin));
        if (separator == std::string::npos) break;
        begin = separator + 2;
    }
    return parts;
}

inline bool reachesInlineType(
    const TypePtr& current, const Type* target,
    std::unordered_set<const Type*>& active) {
    if (!current) return false;
    if (current.get() == target) return true;
    if (!active.insert(current.get()).second) return false;
    bool reaches = false;
    switch (current->kind) {
        case TypeKind::Array:
            reaches = reachesInlineType(
                current->inner, target, active);
            break;
        case TypeKind::Result:
            for (const auto& argument : current->typeArgs)
                reaches = reaches ||
                    reachesInlineType(argument, target, active);
            break;
        case TypeKind::Enum:
            for (const auto& variant : current->variants)
                for (const auto& field : variant.fields)
                    reaches = reaches ||
                        reachesInlineType(field, target, active);
            break;
        default:
            // Product values and explicit pointer/reference/shared wrappers
            // are representation barriers in the current ABI.
            break;
    }
    active.erase(current.get());
    return reaches;
}

inline std::string isolatedLinkageName(const std::string& key,
                                       const std::string& sourceName) {
    std::ostringstream output;
    output << "__luna_" << std::hex << std::setw(16) << std::setfill('0')
           << stableMetadataHash(key) << "_" << sourceName;
    return output.str();
}


inline TypePtr substituteNominalType(
    const TypePtr& type,
    const std::unordered_map<std::string, TypePtr>& bindings) {
    if (!type) return TyUnknown;
    if (type->kind == TypeKind::TypeParam) {
        auto it = bindings.find(type->name);
        return it == bindings.end() ? type : it->second;
    }

    // Preserve every semantic property of the canonical type while replacing
    // type parameters recursively. This also covers arrays, slices, callable
    // ownership contracts, metadata views, slots, and fragments; maintaining
    // a partial kind switch here previously left generic signatures with
    // unresolved nested parameters.
    auto result = std::make_shared<Type>(*type);
    if (type->inner)
        result->inner = substituteNominalType(type->inner, bindings);
    result->typeArgs.clear();
    for (const auto& argument : type->typeArgs)
        result->typeArgs.push_back(
            substituteNominalType(argument, bindings));
    result->paramTypes.clear();
    for (const auto& parameter : type->paramTypes)
        result->paramTypes.push_back(
            substituteNominalType(parameter, bindings));
    if (type->returnType)
        result->returnType =
            substituteNominalType(type->returnType, bindings);
    result->fields.clear();
    for (const auto& field : type->fields)
        result->fields.push_back(
            {field.name, substituteNominalType(field.type, bindings)});
    result->capturedFields.clear();
    for (const auto& field : type->capturedFields)
        result->capturedFields.push_back(
            {field.name, substituteNominalType(field.type, bindings)});
    result->variants.clear();
    for (const auto& variant : type->variants) {
        TypeVariant copied;
        copied.name = variant.name;
        for (const auto& field : variant.fields)
            copied.fields.push_back(
                substituteNominalType(field, bindings));
        result->variants.push_back(std::move(copied));
    }
    return result;
}
