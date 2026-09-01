#pragma once

#include "TypeSystem.h"
#include "../core/TypeRelations.h"
#include "../parser/AST.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unordered_map>
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

inline luna::sysmeta::Facts declarationContractFacts(
    const Decl* declaration, luna::sysmeta::DeclarationKind kind,
    const TypePtr& type) {
    luna::sysmeta::Facts facts;
    if (type) {
        if (kind == luna::sysmeta::DeclarationKind::Function ||
            kind == luna::sysmeta::DeclarationKind::Fragment ||
            kind == luna::sysmeta::DeclarationKind::Slot) {
            facts = type->sysmeta;
        } else {
            // MoonIR declaration records inherit the represented type's
            // resource contract, while non-executable declarations keep
            // their own default control/capability/ABI facts.
            facts.resource = type->sysmeta.resource;
        }
        // MoonIR seals its type graph before finalizing declaration
        // ContractIds. Mirror the same derived resource fields here so the
        // semantic snapshot and the sealed table cannot disagree merely
        // because cleanup/lifetime facts were materialized at different
        // phases.
        const auto resource = resourceContractForType(type);
        facts.resource.usage = resource.usage;
        facts.resource.cleanup = resource.cleanup;
        facts.resource.cleanupRequired = resource.cleanupRequired;
        facts.resource.recursiveCleanup = resource.recursiveCleanup;
        facts.resource.lifetime = resource.lifetime;
        facts.resource.relation = resource.relation;
    }
    facts.capability.runtimeRetained = declaration &&
        declaration->retention != RetentionKind::CompileTime;
    if (const auto* function =
            dynamic_cast<const FunctionDecl*>(declaration)) {
        facts.capability.ffi = function->isExtern || !function->abi.empty();
        facts.capability.gpu = function->isKernel;
        facts.capability.hostOnly = !function->isKernel;
        facts.abi.stableBoundary =
            function->isExtern || !function->abi.empty();
    }
    return facts;
}

inline std::string declarationCanonicalContract(
    const Decl* declaration, luna::sysmeta::DeclarationKind kind,
    const TypePtr& type,
    const luna::identity::SymbolId& dropGlueSymbol = {},
    const luna::identity::ContractId& dropGlueContract = {}) {
    const auto facts = declarationContractFacts(declaration, kind, type);
    return luna::sysmeta::canonicalDeclarationContract(
        kind, luna::types::typeId(type), facts,
        dropGlueSymbol, dropGlueContract);
}

inline bool containsInferenceIdentityImpl(
    const TypePtr& input, std::unordered_set<const Type*>& visited) {
    const TypePtr type = input;
    if (!type || !visited.insert(type.get()).second) return false;
    if (type->kind == TypeKind::InferenceVar) return true;
    if (containsInferenceIdentityImpl(type->inner, visited) ||
        containsInferenceIdentityImpl(type->returnType, visited))
        return true;
    for (const auto& argument : type->typeArgs)
        if (containsInferenceIdentityImpl(argument, visited)) return true;
    for (const auto& parameter : type->paramTypes)
        if (containsInferenceIdentityImpl(parameter, visited)) return true;
    for (const auto& field : type->fields)
        if (containsInferenceIdentityImpl(field.type, visited)) return true;
    for (const auto& variant : type->variants)
        for (const auto& field : variant.fields)
            if (containsInferenceIdentityImpl(field, visited)) return true;
    for (const auto& field : type->capturedFields)
        if (containsInferenceIdentityImpl(field.type, visited)) return true;
    return false;
}

inline bool containsInferenceIdentity(const TypePtr& type) {
    std::unordered_set<const Type*> visited;
    return containsInferenceIdentityImpl(type, visited);
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

inline void appendSourceIdentityPart(
    std::string& output, const std::string& part) {
    output += std::to_string(part.size()) + ":" + part;
}

inline std::string sourceTypeIdentity(
    const TypeAST* type,
    const std::unordered_map<std::string, size_t>& typeParameters) {
    if (!type) return "inferred";
    if (const auto* named = dynamic_cast<const NamedTypeAST*>(type)) {
        std::string result = "named{";
        const auto parameter = typeParameters.find(named->name);
        appendSourceIdentityPart(
            result,
            parameter == typeParameters.end()
                ? named->name
                : "type-param@" + std::to_string(parameter->second));
        appendSourceIdentityPart(
            result,
            named->arrayLength
                ? std::to_string(*named->arrayLength)
                : std::string{});
        for (const auto& argument : named->typeArgs)
            appendSourceIdentityPart(
                result,
                sourceTypeIdentity(argument.get(), typeParameters));
        return result + "}";
    }
    if (const auto* reference = dynamic_cast<const RefTypeAST*>(type)) {
        std::string result = reference->isMutable
            ? "mutable-reference{" : "shared-reference{";
        appendSourceIdentityPart(
            result,
            sourceTypeIdentity(reference->inner.get(), typeParameters));
        return result + "}";
    }
    if (const auto* linear = dynamic_cast<const LinearTypeAST*>(type)) {
        std::string result = "linear{";
        appendSourceIdentityPart(
            result,
            sourceTypeIdentity(linear->inner.get(), typeParameters));
        return result + "}";
    }
    if (const auto* affine = dynamic_cast<const AffineTypeAST*>(type)) {
        std::string result = "affine{";
        appendSourceIdentityPart(
            result,
            sourceTypeIdentity(affine->inner.get(), typeParameters));
        return result + "}";
    }
    if (const auto* function = dynamic_cast<const FunctionTypeAST*>(type)) {
        std::string result = "function{";
        for (const auto& parameter : function->paramTypes)
            appendSourceIdentityPart(
                result,
                sourceTypeIdentity(parameter.get(), typeParameters));
        appendSourceIdentityPart(
            result,
            sourceTypeIdentity(function->returnType.get(), typeParameters));
        return result + "}";
    }
    if (const auto* record = dynamic_cast<const RecordTypeAST*>(type)) {
        std::vector<const RecordTypeAST::Field*> fields;
        fields.reserve(record->fields.size());
        for (const auto& field : record->fields)
            fields.push_back(&field);
        std::sort(
            fields.begin(), fields.end(),
            [](const auto* lhs, const auto* rhs) {
                return lhs->name < rhs->name;
            });
        std::string result = "record{";
        for (const auto* field : fields) {
            appendSourceIdentityPart(result, field->name);
            appendSourceIdentityPart(
                result,
                sourceTypeIdentity(field->type.get(), typeParameters));
        }
        return result + "}";
    }
    return "unknown-source-type";
}

inline std::string functionSourceSignatureIdentity(
    const FunctionDecl* function) {
    if (!function) return {};
    std::unordered_map<std::string, size_t> typeParameters;
    for (size_t index = 0; index < function->typeParams.size(); ++index)
        typeParameters.emplace(function->typeParams[index], index);
    std::string result = "function-signature{";
    appendSourceIdentityPart(
        result, std::to_string(function->typeParams.size()));
    for (const auto& parameter : function->params) {
        appendSourceIdentityPart(
            result,
            parameter.hasExplicitUsage
                ? std::string(luna::ownership::usageName(parameter.usage))
                : "default-usage");
        appendSourceIdentityPart(
            result,
            sourceTypeIdentity(parameter.type.get(), typeParameters));
    }
    appendSourceIdentityPart(
        result,
        sourceTypeIdentity(function->returnType.get(), typeParameters));
    return result + "}";
}

inline std::string declarationSourceIdentity(
    const std::string& base, const Decl* declaration) {
    std::string result = metadataDeclarationName(base, declaration);
    if (const auto* function =
            dynamic_cast<const FunctionDecl*>(declaration)) {
        result += "::" + functionSourceSignatureIdentity(function);
    }
    return result;
}

inline std::string functionDeclarationIdentity(
    const Program* program, const FunctionDecl* function) {
    if (!function) return {};
    return nominalDeclarationIdentity(
        program, "fn",
        declarationSourceIdentity(function->name, function),
        function);
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
