#include "CompileTimeEvaluator.h"

#include "SemanticAnalysisSupport.h"
#include "../core/TypeLayout.h"
#include "../core/TypeRelations.h"
#include "../parser/AST.h"
#include "../selector/Selector.h"
#include <cmath>
#include <unordered_set>
#include <utility>

TypePtr CompileTimeEvaluator::analyzeReflectionCall(CallExpr* call, const std::string& name) {
    const bool isBinaryRelation = name == "type_same" ||
        name == "type_same_shape" || name == "type_abi_compatible";
    if (isBinaryRelation) {
        if (call->typeArgASTs.size() != 2 || !call->args.empty()) {
            mContext.error(name + " expects exactly two type arguments and no value arguments",
                  call->line, call->col);
            return TyUnknown;
        }
        auto lhs = mContext.resolved(mContext.resolveTypeAST(call->typeArgASTs[0].get(), {}));
        auto rhs = mContext.resolved(mContext.resolveTypeAST(call->typeArgASTs[1].get(), {}));
        if (name == "type_same")
            call->compileTimeValue = luna::types::sameType(lhs, rhs);
        else if (name == "type_same_shape")
            call->compileTimeValue = luna::types::sameShape(lhs, rhs);
        else
            call->compileTimeValue = luna::types::isAbiCompatible(lhs, rhs);
        call->resultType = TyBool;
        return call->resultType;
    }

    TypePtr type;
    if (!call->typeArgASTs.empty()) {
        if (call->typeArgASTs.size() != 1) {
            mContext.error(name + " expects exactly one type argument");
            return TyUnknown;
        }
        type = mContext.resolved(mContext.resolveTypeAST(call->typeArgASTs[0].get(), {}));
    } else if (call->args.size() == 1) {
        type = mContext.resolved(mContext.analyzeExpr(call->args[0].get()));
    } else {
        mContext.error(name + " expects either `<Type>()` or one value argument",
              call->line, call->col);
        return TyUnknown;
    }

    auto kindName = [](const TypePtr& t) {
        switch (t->kind) {
            case TypeKind::I8: case TypeKind::I16: case TypeKind::I32: case TypeKind::I64:
            case TypeKind::U8: case TypeKind::U16: case TypeKind::U32: case TypeKind::U64:
            case TypeKind::USize: case TypeKind::ISize: return std::string("integer");
            case TypeKind::F32: case TypeKind::F64: return std::string("float");
            case TypeKind::Bool: return std::string("bool");
            case TypeKind::String: case TypeKind::CStr: return std::string("string");
            case TypeKind::RawPointer: return std::string("raw_pointer");
            case TypeKind::Reference: return std::string("reference");
            case TypeKind::Function: return std::string("function");
            case TypeKind::Struct: return std::string("struct");
            case TypeKind::Record: return std::string("record");
            case TypeKind::Enum: return std::string("enum");
            case TypeKind::Result: return std::string("result");
            case TypeKind::Trait: return std::string("trait");
            case TypeKind::Unit: return std::string("unit");
            case TypeKind::Never: return std::string("never");
            default: return std::string("unknown");
        }
    };
    const auto containsTypeParameter = [](const TypePtr& root) {
        std::unordered_set<const Type*> active;
        std::function<bool(const TypePtr&)> visit =
            [&](const TypePtr& current) -> bool {
                if (!current || !active.insert(current.get()).second)
                    return false;
                if (current->kind == TypeKind::TypeParam) return true;
                if (visit(current->inner) || visit(current->returnType))
                    return true;
                for (const auto& item : current->typeArgs)
                    if (visit(item)) return true;
                for (const auto& item : current->paramTypes)
                    if (visit(item)) return true;
                for (const auto& field : current->fields)
                    if (visit(field.type)) return true;
                for (const auto& variant : current->variants)
                    for (const auto& field : variant.fields)
                        if (visit(field)) return true;
                return false;
            };
        return visit(root);
    };
    auto constIndex = [&]() -> std::optional<size_t> {
        if (call->args.size() != 1) {
            mContext.error(name + " requires one compile-time integer index");
            return std::nullopt;
        }
        mContext.analyzeExpr(call->args[0].get());
        auto value = evaluateConstExpr(call->args[0].get(), {});
        if (!value || !std::holds_alternative<int64_t>(*value) || std::get<int64_t>(*value) < 0) {
            mContext.error(name + " requires a non-negative compile-time integer index");
            return std::nullopt;
        }
        return static_cast<size_t>(std::get<int64_t>(*value));
    };

    if (name == "type_of") call->compileTimeValue = type->toString();
    else if (name == "type_kind") call->compileTimeValue = kindName(type);
    else if (name == "type_id")
        call->compileTimeValue = luna::types::typeId(type).value;
    else if (name == "type_shape")
        call->compileTimeValue = luna::types::shapeId(type).value;
    else if (name == "type_domain") {
        switch (type->domain) {
            case luna::types::TypeDomain::Value: call->compileTimeValue = std::string("value"); break;
            case luna::types::TypeDomain::Meta: call->compileTimeValue = std::string("meta"); break;
            case luna::types::TypeDomain::Compiler: call->compileTimeValue = std::string("compiler"); break;
            case luna::types::TypeDomain::Inference: call->compileTimeValue = std::string("inference"); break;
            case luna::types::TypeDomain::Error: call->compileTimeValue = std::string("error"); break;
        }
    }
    else if (name == "type_nominal") call->compileTimeValue = type->nominalId;
    else if (name == "type_size") {
        // Generic templates are analyzed before instantiation. Leave layout
        // reflection unfrozen until their concrete clone is analyzed.
        if (!containsTypeParameter(type))
            call->compileTimeValue = static_cast<int64_t>(
                luna::layout::valueSize(type));
    }
    else if (name == "type_alignment") {
        if (!containsTypeParameter(type))
            call->compileTimeValue = static_cast<int64_t>(
                luna::layout::valueAlignment(type));
    }
    else if (name == "type_is_struct") call->compileTimeValue = type->kind == TypeKind::Struct;
    else if (name == "type_is_enum") call->compileTimeValue = type->kind == TypeKind::Enum;
    else if (name == "type_is_nominal") call->compileTimeValue = !type->nominalId.empty();
    else if (name == "type_is_structural")
        call->compileTimeValue =
            type->identityMode == luna::types::IdentityMode::Structural;
    else if (name == "type_is_meta")
        call->compileTimeValue = type->domain == luna::types::TypeDomain::Meta;
    else if (name == "type_is_reference") call->compileTimeValue = type->kind == TypeKind::Reference;
    else if (name == "type_field_count") {
        if (type->kind != TypeKind::Struct && type->kind != TypeKind::Record) {
            mContext.error(name + " requires a struct or record type"); return TyUnknown;
        }
        call->compileTimeValue = static_cast<int64_t>(type->fields.size());
    } else if (name == "type_field_name" || name == "type_field_type") {
        if (type->kind != TypeKind::Struct && type->kind != TypeKind::Record) {
            mContext.error(name + " requires a struct or record type"); return TyUnknown;
        }
        auto index = constIndex();
        if (!index) return TyUnknown;
        if (*index >= type->fields.size()) {
            mContext.error(name + " index " + std::to_string(*index) + " is out of range"); return TyUnknown;
        }
        call->compileTimeValue = name == "type_field_name"
            ? type->fields[*index].name : type->fields[*index].type->toString();
    } else if (name == "type_variant_count") {
        if (type->kind != TypeKind::Enum) { mContext.error(name + " requires an enum type"); return TyUnknown; }
        call->compileTimeValue = static_cast<int64_t>(type->variants.size());
    } else if (name == "type_variant_name" || name == "type_variant_field_count") {
        if (type->kind != TypeKind::Enum) { mContext.error(name + " requires an enum type"); return TyUnknown; }
        auto index = constIndex();
        if (!index) return TyUnknown;
        if (*index >= type->variants.size()) {
            mContext.error(name + " index " + std::to_string(*index) + " is out of range"); return TyUnknown;
        }
        call->compileTimeValue = name == "type_variant_name"
            ? std::variant<int64_t, double, bool, std::string>(type->variants[*index].name)
            : std::variant<int64_t, double, bool, std::string>(static_cast<int64_t>(type->variants[*index].fields.size()));
    }

    if (name == "type_size" || name == "type_alignment" ||
        name == "type_field_count" || name == "type_variant_count" ||
        name == "type_variant_field_count") {
        call->resultType = TyI32;
        return call->resultType;
    }
    if (name == "type_is_struct" || name == "type_is_enum" ||
        name == "type_is_nominal" || name == "type_is_structural" ||
        name == "type_is_meta" || name == "type_is_reference") {
        call->resultType = TyBool;
        return call->resultType;
    }
    call->resultType = TyString;
    return call->resultType;
}
TypePtr CompileTimeEvaluator::analyzeDeclarationReflectionCall(
    CallExpr* call, const std::string& name) {
    if (name == "declaration_of") {
        if (call->args.size() != 1) {
            mContext.error("declaration_of expects exactly one declaration name",
                  call->line, call->col);
            return TyUnknown;
        }
        auto* identifier =
            dynamic_cast<IdentifierExpr*>(call->args.front().get());
        if (!identifier) {
            mContext.error("declaration_of requires a statically named declaration",
                  call->line, call->col);
            return TyUnknown;
        }
        auto family = mContext.mFunctionFamilies.find(
            mContext.sourceDeclarationKey(identifier->name));
        if (family == mContext.mFunctionFamilies.end() || family->second.empty()) {
            mContext.error("unknown declaration '" + identifier->name + "'",
                  call->line, call->col);
            return TyUnknown;
        }
        TypePtr requested;
        if (!call->typeArgASTs.empty()) {
            if (call->typeArgASTs.size() != 1) {
                mContext.error("declaration_of accepts at most one callable type argument",
                      call->line, call->col);
                return TyUnknown;
            }
            requested = mContext.resolved(
                mContext.resolveTypeAST(call->typeArgASTs.front().get(), {}));
            if (requested->kind != TypeKind::Function)
                mContext.error("declaration_of type argument must be a callable type",
                      call->line, call->col);
        }

        FunctionDecl* selected = nullptr;
        TypePtr selectedType;
        for (auto* candidate : family->second) {
            TypeVec parameters;
            std::vector<luna::ownership::Contract> contracts;
            for (const auto& parameter : candidate->params) {
                parameters.push_back(mContext.resolved(parameter.inferredType));
                contracts.push_back({parameter.relation, parameter.usage});
            }
            TypePtr callable = Type::makeFunction(
                std::move(parameters), mContext.resolved(candidate->inferredReturnType),
                std::move(contracts),
                {luna::ownership::Relation::Owned, candidate->returnUsage});
            if (requested && !luna::types::sameType(requested, callable))
                continue;
            if (selected) {
                mContext.error("declaration_of '" + identifier->name +
                      "' is ambiguous; provide a unique callable signature "
                      "or use select for an open declaration family",
                      call->line, call->col);
                return TyUnknown;
            }
            selected = candidate;
            selectedType = callable;
        }
        if (!selected) {
            mContext.error("declaration_of found no declaration matching the requested "
                  "signature for '" + identifier->name + "'",
                  call->line, call->col);
            return TyUnknown;
        }
        const auto symbol = selected->generatedSymbolName.empty()
            ? selected->name : selected->generatedSymbolName;
        call->compileTimeDeclarationId =
            functionDeclarationIdentity(mContext.mProgram, selected);
        call->resolvedSymbolName = symbol;
        call->resultType = Type::makeDeclarationRef(selectedType);
        return call->resultType;
    }

    if (call->args.size() != 1) {
        mContext.error(name + " expects exactly one declaration_ref",
              call->line, call->col);
        return TyUnknown;
    }
    TypePtr reference = mContext.resolved(mContext.analyzeExpr(call->args.front().get()));
    if (reference->kind != TypeKind::DeclarationRef) {
        mContext.error(name + " expects a declaration_ref",
              call->line, call->col);
        return TyUnknown;
    }
    if (auto* nested =
            dynamic_cast<CallExpr*>(call->args.front().get());
        nested && !nested->compileTimeDeclarationId.empty()) {
        if (name == "declaration_id")
            call->compileTimeValue = nested->compileTimeDeclarationId;
        else if (reference->inner)
            call->compileTimeValue =
                luna::types::typeId(reference->inner).value;
    } else if (auto* identifier =
                   dynamic_cast<IdentifierExpr*>(call->args.front().get())) {
        auto* symbol = mContext.mSymTable.lookup(identifier->name);
        if (symbol && !symbol->compileTimeDeclarationId.empty()) {
            if (name == "declaration_id")
                call->compileTimeValue =
                    symbol->compileTimeDeclarationId;
            else if (reference->inner)
                call->compileTimeValue =
                    luna::types::typeId(reference->inner).value;
        }
    }
    // A statically expanded declaration_view loop has one concrete identity
    // per lowering iteration, but its callable signature is invariant and can
    // be folded during the single semantic analysis of the loop body.
    if (name == "declaration_signature" && !call->compileTimeValue &&
        reference->inner)
        call->compileTimeValue =
            luna::types::typeId(reference->inner).value;
    call->resultType = TyString;
    return call->resultType;
}
