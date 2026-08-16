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
        call->compileTimeDeclarationId = nominalDeclarationIdentity(
            mContext.mProgram, "fn", symbol, selected);
        call->resolvedSymbolName = symbol;
        return Type::makeDeclarationRef(selectedType);
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
    return TyString;
}

void CompileTimeEvaluator::enterConstScope() { mContext.mConstScopes.emplace_back(); }


void CompileTimeEvaluator::exitConstScope() {
    if (mContext.mConstScopes.size() > 1) mContext.mConstScopes.pop_back();
}


void CompileTimeEvaluator::defineConst(const std::string& name, const ConstValue& value) {
    if (mContext.mConstScopes.empty()) enterConstScope();
    mContext.mConstScopes.back()[name] = value;
}


const CompileTimeEvaluator::ConstValue* CompileTimeEvaluator::lookupConst(const std::string& name) const {
    for (auto it = mContext.mConstScopes.rbegin(); it != mContext.mConstScopes.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return &found->second;
    }
    return nullptr;
}

std::optional<CompileTimeEvaluator::ConstValue>

CompileTimeEvaluator::evaluateConstExpr(Expr* expr, const std::unordered_map<std::string, ConstValue>& locals) {
    if (!expr) return std::nullopt;
    if (auto* value = dynamic_cast<IntLiteralExpr*>(expr)) return value->value;
    if (auto* value = dynamic_cast<FloatLiteralExpr*>(expr)) return value->value;
    if (auto* value = dynamic_cast<BoolLiteralExpr*>(expr)) return value->value;
    if (auto* value = dynamic_cast<StringLiteralExpr*>(expr)) return value->value;
    if (auto* value = dynamic_cast<IdentifierExpr*>(expr)) {
        auto local = locals.find(value->name);
        if (local != locals.end()) return local->second;
        if (auto* global = lookupConst(value->name)) return *global;
        return std::nullopt;
    }
    if (auto* call = dynamic_cast<CallExpr*>(expr)) {
        if (call->compileTimeValue) return *call->compileTimeValue;
        auto* callee = dynamic_cast<IdentifierExpr*>(call->callee.get());
        if (!callee) return std::nullopt;
        const std::string& constexprName = call->resolvedSymbolName.empty()
            ? callee->name : call->resolvedSymbolName;
        auto function = mContext.mConstexprFunctions.find(constexprName);
        if (function == mContext.mConstexprFunctions.end()) return std::nullopt;
        std::vector<ConstValue> args;
        for (auto& arg : call->args) {
            auto value = evaluateConstExpr(arg.get(), locals);
            if (!value) return std::nullopt;
            args.push_back(std::move(*value));
        }
        return evaluateConstFunction(function->second, args);
    }
    if (auto* unary = dynamic_cast<UnaryExpr*>(expr)) {
        auto operand = evaluateConstExpr(unary->operand.get(), locals);
        if (!operand) return std::nullopt;
        if (unary->op == TokenKind::Minus) {
            if (auto* i = std::get_if<int64_t>(&*operand)) return -*i;
            if (auto* f = std::get_if<double>(&*operand)) return -*f;
        }
        if (unary->op == TokenKind::Not) {
            if (auto* b = std::get_if<bool>(&*operand)) return !*b;
        }
        if (unary->op == TokenKind::Tilde) {
            if (auto* i = std::get_if<int64_t>(&*operand)) return ~*i;
        }
        return std::nullopt;
    }
    if (auto* binary = dynamic_cast<BinaryExpr*>(expr)) {
        auto lhs = evaluateConstExpr(binary->lhs.get(), locals);
        auto rhs = evaluateConstExpr(binary->rhs.get(), locals);
        if (!lhs || !rhs) return std::nullopt;
        auto li = std::get_if<int64_t>(&*lhs); auto ri = std::get_if<int64_t>(&*rhs);
        if (li && ri) {
            switch (binary->op) {
                case TokenKind::Plus: return *li + *ri; case TokenKind::Minus: return *li - *ri;
                case TokenKind::Star: return *li * *ri; case TokenKind::Slash: if (*ri) return *li / *ri; break;
                case TokenKind::Percent: if (*ri) return *li % *ri; break;
                case TokenKind::Ampersand: return *li & *ri; case TokenKind::BitOr: return *li | *ri;
                case TokenKind::BitXor: return *li ^ *ri; case TokenKind::ShiftLeft: return *li << *ri;
                case TokenKind::ShiftRight: return *li >> *ri; case TokenKind::EqEq: return *li == *ri;
                case TokenKind::Neq: return *li != *ri; case TokenKind::Lt: return *li < *ri;
                case TokenKind::LtEq: return *li <= *ri; case TokenKind::Gt: return *li > *ri;
                case TokenKind::GtEq: return *li >= *ri; default: break;
            }
        }
        auto lf = std::get_if<double>(&*lhs); auto rf = std::get_if<double>(&*rhs);
        if (lf && rf) {
            switch (binary->op) {
                case TokenKind::Plus: return *lf + *rf; case TokenKind::Minus: return *lf - *rf;
                case TokenKind::Star: return *lf * *rf; case TokenKind::Slash: if (*rf != 0) return *lf / *rf; break;
                case TokenKind::Percent: if (*rf != 0) return std::fmod(*lf, *rf); break;
                case TokenKind::EqEq: return *lf == *rf; case TokenKind::Neq: return *lf != *rf;
                case TokenKind::Lt: return *lf < *rf; case TokenKind::LtEq: return *lf <= *rf;
                case TokenKind::Gt: return *lf > *rf; case TokenKind::GtEq: return *lf >= *rf;
                default: break;
            }
        }
        auto lb = std::get_if<bool>(&*lhs); auto rb = std::get_if<bool>(&*rhs);
        if (lb && rb) {
            if (binary->op == TokenKind::AndAnd) return *lb && *rb;
            if (binary->op == TokenKind::OrOr) return *lb || *rb;
            if (binary->op == TokenKind::EqEq) return *lb == *rb;
            if (binary->op == TokenKind::Neq) return *lb != *rb;
        }
    }
    return std::nullopt;
}

std::optional<CompileTimeEvaluator::ConstValue>

CompileTimeEvaluator::evaluateConstFunction(FunctionDecl* function, const std::vector<ConstValue>& args) {
    if (!function || !function->isConstexpr || function->params.size() != args.size())
        return std::nullopt;
    if (++mContext.mConstEvaluationDepth > 128) {
        --mContext.mConstEvaluationDepth;
        return std::nullopt;
    }
    std::unordered_map<std::string, ConstValue> locals;
    for (size_t i = 0; i < args.size(); ++i) locals[function->params[i].name] = args[i];
    std::optional<ConstValue> result;
    bool completed = evaluateConstBlock(function->body.get(), locals, result);
    --mContext.mConstEvaluationDepth;
    return completed ? result : std::nullopt;
}


bool CompileTimeEvaluator::evaluateConstBlock(BlockStmt* block,
                                          std::unordered_map<std::string, ConstValue>& locals,
                                          std::optional<ConstValue>& result) {
    if (!block) return false;
    for (auto& statement : block->stmts) {
        if (auto* let = dynamic_cast<LetStmt*>(statement.get())) {
            auto value = evaluateConstExpr(let->initializer.get(), locals);
            if (!value) return false;
            locals[let->name] = *value;
        } else if (auto* ret = dynamic_cast<ReturnStmt*>(statement.get())) {
            if (!ret->value) return false;
            result = evaluateConstExpr(ret->value.get(), locals);
            return result.has_value();
        } else if (auto* conditional = dynamic_cast<IfStmt*>(statement.get())) {
            auto condition = evaluateConstExpr(conditional->cond.get(), locals);
            auto boolValue = condition ? std::get_if<bool>(&*condition) : nullptr;
            if (!boolValue) return false;
            if (*boolValue) return evaluateConstBlock(conditional->thenBlock.get(), locals, result);
            if (auto* elseBlock = dynamic_cast<BlockStmt*>(conditional->elseBranch.get()))
                return evaluateConstBlock(elseBlock, locals, result);
            return false;
        } else {
            return false;
        }
    }
    return false;
}

std::optional<CompileTimeEvaluator::ConstValue>


CompileTimeEvaluator::evaluateConstraintExpr(
    Expr* expr, const std::unordered_map<std::string, TypePtr>& bindings,
    std::vector<std::string>& active) {
    if (!expr) return std::nullopt;
    if (auto* value = dynamic_cast<IntLiteralExpr*>(expr)) return value->value;
    if (auto* value = dynamic_cast<FloatLiteralExpr*>(expr)) return value->value;
    if (auto* value = dynamic_cast<BoolLiteralExpr*>(expr)) return value->value;
    if (auto* value = dynamic_cast<StringLiteralExpr*>(expr)) return value->value;
    if (auto* identifier = dynamic_cast<IdentifierExpr*>(expr)) {
        if (auto* value = lookupConst(identifier->name)) return *value;
        return std::nullopt;
    }
    if (auto* unary = dynamic_cast<UnaryExpr*>(expr)) {
        auto operand = evaluateConstraintExpr(unary->operand.get(), bindings, active);
        if (!operand) return std::nullopt;
        if (unary->op == TokenKind::Not) {
            if (auto* value = std::get_if<bool>(&*operand)) return !*value;
        }
        if (unary->op == TokenKind::Minus) {
            if (auto* value = std::get_if<int64_t>(&*operand)) return -*value;
            if (auto* value = std::get_if<double>(&*operand)) return -*value;
        }
        return std::nullopt;
    }
    if (auto* binary = dynamic_cast<BinaryExpr*>(expr)) {
        auto lhs = evaluateConstraintExpr(binary->lhs.get(), bindings, active);
        if (!lhs) return std::nullopt;
        if (binary->op == TokenKind::AndAnd) {
            auto* boolean = std::get_if<bool>(&*lhs);
            if (!boolean) return std::nullopt;
            if (!*boolean) return false;
        }
        if (binary->op == TokenKind::OrOr) {
            auto* boolean = std::get_if<bool>(&*lhs);
            if (!boolean) return std::nullopt;
            if (*boolean) return true;
        }
        auto rhs = evaluateConstraintExpr(binary->rhs.get(), bindings, active);
        if (!rhs) return std::nullopt;
        auto li = std::get_if<int64_t>(&*lhs);
        auto ri = std::get_if<int64_t>(&*rhs);
        if (li && ri) {
            switch (binary->op) {
                case TokenKind::Plus: return *li + *ri;
                case TokenKind::Minus: return *li - *ri;
                case TokenKind::Star: return *li * *ri;
                case TokenKind::Slash: if (*ri != 0) return *li / *ri; break;
                case TokenKind::Percent: if (*ri != 0) return *li % *ri; break;
                case TokenKind::EqEq: return *li == *ri;
                case TokenKind::Neq: return *li != *ri;
                case TokenKind::Lt: return *li < *ri;
                case TokenKind::LtEq: return *li <= *ri;
                case TokenKind::Gt: return *li > *ri;
                case TokenKind::GtEq: return *li >= *ri;
                default: break;
            }
        }
        auto lb = std::get_if<bool>(&*lhs);
        auto rb = std::get_if<bool>(&*rhs);
        if (lb && rb) {
            if (binary->op == TokenKind::AndAnd) return *lb && *rb;
            if (binary->op == TokenKind::OrOr) return *lb || *rb;
            if (binary->op == TokenKind::EqEq) return *lb == *rb;
            if (binary->op == TokenKind::Neq) return *lb != *rb;
        }
        auto ls = std::get_if<std::string>(&*lhs);
        auto rs = std::get_if<std::string>(&*rhs);
        if (ls && rs) {
            if (binary->op == TokenKind::EqEq) return *ls == *rs;
            if (binary->op == TokenKind::Neq) return *ls != *rs;
        }
        return std::nullopt;
    }
    auto* call = dynamic_cast<CallExpr*>(expr);
    auto* callee = call
        ? dynamic_cast<IdentifierExpr*>(call->callee.get()) : nullptr;
    if (!call || !callee || !call->args.empty()) return std::nullopt;

    const std::string conceptKey =
        mContext.sourceDeclarationKey(callee->name, false);
    if (mContext.mConcepts.count(conceptKey)) {
        TypeVec arguments;
        for (auto& argument : call->typeArgASTs)
            arguments.push_back(mContext.resolved(
                mContext.resolveTypeAST(argument.get(), bindings)));
        auto value = evaluateConstraint(conceptKey, arguments, active);
        return value ? std::optional<ConstValue>(*value) : std::nullopt;
    }

    auto resolveArgument = [&](size_t index) -> TypePtr {
        if (index >= call->typeArgASTs.size()) return TyUnknown;
        return mContext.resolved(mContext.resolveTypeAST(
            call->typeArgASTs[index].get(), bindings));
    };
    const bool binaryRelation = callee->name == "type_same" ||
        callee->name == "type_same_shape" ||
        callee->name == "type_abi_compatible";
    if (binaryRelation) {
        if (call->typeArgASTs.size() != 2) return std::nullopt;
        TypePtr lhs = resolveArgument(0);
        TypePtr rhs = resolveArgument(1);
        if (callee->name == "type_same")
            return luna::types::sameType(lhs, rhs);
        if (callee->name == "type_same_shape")
            return luna::types::sameShape(lhs, rhs);
        return luna::types::isAbiCompatible(lhs, rhs);
    }
    if (call->typeArgASTs.size() != 1) return std::nullopt;
    TypePtr type = resolveArgument(0);
    if (!type || type->kind == TypeKind::Unknown ||
        type->kind == TypeKind::TypeParam ||
        type->kind == TypeKind::InferenceVar)
        return std::nullopt;
    if (callee->name == "type_is_struct")
        return type->kind == TypeKind::Struct;
    if (callee->name == "type_is_enum")
        return type->kind == TypeKind::Enum;
    if (callee->name == "type_is_nominal")
        return !type->nominalId.empty();
    if (callee->name == "type_is_structural")
        return type->identityMode == luna::types::IdentityMode::Structural;
    if (callee->name == "type_is_meta")
        return type->domain == luna::types::TypeDomain::Meta;
    if (callee->name == "type_is_reference")
        return type->kind == TypeKind::Reference;
    if (callee->name == "type_field_count") {
        if (type->kind != TypeKind::Struct &&
            type->kind != TypeKind::Record)
            return std::nullopt;
        return static_cast<int64_t>(type->fields.size());
    }
    if (callee->name == "type_variant_count") {
        if (type->kind != TypeKind::Enum) return std::nullopt;
        return static_cast<int64_t>(type->variants.size());
    }
    if (callee->name == "type_id")
        return luna::types::typeId(type).value;
    if (callee->name == "type_shape")
        return luna::types::shapeId(type).value;
    if (callee->name == "type_size")
        return static_cast<int64_t>(luna::layout::valueSize(type));
    if (callee->name == "type_alignment")
        return static_cast<int64_t>(luna::layout::valueAlignment(type));
    return std::nullopt;
}

std::optional<bool> CompileTimeEvaluator::evaluateConstraint(
    const std::string& name, const TypeVec& arguments,
    std::vector<std::string>& active) {
    auto found = mContext.mConcepts.find(name);
    if (found == mContext.mConcepts.end()) {
        const auto key = mContext.sourceDeclarationKey(name, false);
        found = mContext.mConcepts.find(key);
    }
    if (found == mContext.mConcepts.end() ||
        found->second->typeParams.size() != arguments.size())
        return std::nullopt;
    if (std::find(active.begin(), active.end(), found->first) != active.end())
        return std::nullopt;
    active.push_back(found->first);
    std::unordered_map<std::string, TypePtr> bindings;
    for (size_t index = 0; index < arguments.size(); ++index)
        bindings[found->second->typeParams[index]] = arguments[index];
    auto value = evaluateConstraintExpr(
        found->second->predicate.get(), bindings, active);
    active.pop_back();
    if (!value) return std::nullopt;
    if (auto* boolean = std::get_if<bool>(&*value)) return *boolean;
    return std::nullopt;
}

std::optional<CompileTimeEvaluator::SelectorValue>

CompileTimeEvaluator::evaluateSelectorExpr(
    Expr* expr, std::unordered_map<std::string, SelectorValue>& locals) {
    if (!expr) return std::nullopt;
    if (auto* value = dynamic_cast<IntLiteralExpr*>(expr)) return value->value;
    if (auto* value = dynamic_cast<FloatLiteralExpr*>(expr)) return value->value;
    if (auto* value = dynamic_cast<BoolLiteralExpr*>(expr)) return value->value;
    if (auto* value = dynamic_cast<StringLiteralExpr*>(expr)) return value->value;
    if (auto* identifier = dynamic_cast<IdentifierExpr*>(expr)) {
        auto local = locals.find(identifier->name);
        if (local != locals.end()) return local->second;
        if (auto* value = lookupConst(identifier->name))
            return std::visit([](const auto& item) -> SelectorValue {
                return item;
            }, *value);
        return std::nullopt;
    }
    if (auto* field = dynamic_cast<FieldAccessExpr*>(expr)) {
        auto object = evaluateSelectorExpr(field->object.get(), locals);
        auto* metadata = object
            ? std::get_if<SelectorMetadataValue>(&*object) : nullptr;
        if (!metadata) return std::nullopt;
        for (const auto& [key, schema] : mContext.mMetadataSchemas) {
            const auto symbol = schema->generatedSymbolName.empty()
                ? schema->name : schema->generatedSymbolName;
            if (nominalDeclarationIdentity(
                    mContext.mProgram, "meta", symbol, schema) != metadata->schemaId)
                continue;
            for (size_t index = 0; index < schema->fields.size(); ++index) {
                if (schema->fields[index].name != field->field ||
                    index >= metadata->fields.size())
                    continue;
                return std::visit([](const auto& item) -> SelectorValue {
                    return item;
                }, metadata->fields[index]);
            }
        }
        return std::nullopt;
    }
    if (auto* unary = dynamic_cast<UnaryExpr*>(expr)) {
        auto operand = evaluateSelectorExpr(unary->operand.get(), locals);
        if (!operand) return std::nullopt;
        if (unary->op == TokenKind::Not) {
            if (auto* value = std::get_if<bool>(&*operand)) return !*value;
        } else if (unary->op == TokenKind::Minus) {
            if (auto* value = std::get_if<int64_t>(&*operand)) return -*value;
            if (auto* value = std::get_if<double>(&*operand)) return -*value;
        } else if (unary->op == TokenKind::Tilde) {
            if (auto* value = std::get_if<int64_t>(&*operand)) return ~*value;
        }
        return std::nullopt;
    }
    if (auto* binary = dynamic_cast<BinaryExpr*>(expr)) {
        auto lhs = evaluateSelectorExpr(binary->lhs.get(), locals);
        if (!lhs) return std::nullopt;
        if (binary->op == TokenKind::AndAnd) {
            auto* value = std::get_if<bool>(&*lhs);
            if (!value) return std::nullopt;
            if (!*value) return false;
        } else if (binary->op == TokenKind::OrOr) {
            auto* value = std::get_if<bool>(&*lhs);
            if (!value) return std::nullopt;
            if (*value) return true;
        }
        auto rhs = evaluateSelectorExpr(binary->rhs.get(), locals);
        if (!rhs) return std::nullopt;
        auto li = std::get_if<int64_t>(&*lhs);
        auto ri = std::get_if<int64_t>(&*rhs);
        if (li && ri) {
            switch (binary->op) {
                case TokenKind::Plus: return *li + *ri;
                case TokenKind::Minus: return *li - *ri;
                case TokenKind::Star: return *li * *ri;
                case TokenKind::Slash: if (*ri != 0) return *li / *ri; break;
                case TokenKind::Percent: if (*ri != 0) return *li % *ri; break;
                case TokenKind::EqEq: return *li == *ri;
                case TokenKind::Neq: return *li != *ri;
                case TokenKind::Lt: return *li < *ri;
                case TokenKind::LtEq: return *li <= *ri;
                case TokenKind::Gt: return *li > *ri;
                case TokenKind::GtEq: return *li >= *ri;
                case TokenKind::Ampersand: return *li & *ri;
                case TokenKind::BitOr: return *li | *ri;
                case TokenKind::BitXor: return *li ^ *ri;
                default: break;
            }
        }
        auto lf = std::get_if<double>(&*lhs);
        auto rf = std::get_if<double>(&*rhs);
        if (lf && rf) {
            switch (binary->op) {
                case TokenKind::Plus: return *lf + *rf;
                case TokenKind::Minus: return *lf - *rf;
                case TokenKind::Star: return *lf * *rf;
                case TokenKind::Slash: if (*rf != 0.0) return *lf / *rf; break;
                case TokenKind::EqEq: return *lf == *rf;
                case TokenKind::Neq: return *lf != *rf;
                case TokenKind::Lt: return *lf < *rf;
                case TokenKind::LtEq: return *lf <= *rf;
                case TokenKind::Gt: return *lf > *rf;
                case TokenKind::GtEq: return *lf >= *rf;
                default: break;
            }
        }
        auto lb = std::get_if<bool>(&*lhs);
        auto rb = std::get_if<bool>(&*rhs);
        if (lb && rb) {
            if (binary->op == TokenKind::AndAnd) return *lb && *rb;
            if (binary->op == TokenKind::OrOr) return *lb || *rb;
            if (binary->op == TokenKind::EqEq) return *lb == *rb;
            if (binary->op == TokenKind::Neq) return *lb != *rb;
        }
        auto ls = std::get_if<std::string>(&*lhs);
        auto rs = std::get_if<std::string>(&*rhs);
        if (ls && rs) {
            if (binary->op == TokenKind::EqEq) return *ls == *rs;
            if (binary->op == TokenKind::Neq) return *ls != *rs;
            if (binary->op == TokenKind::Lt) return *ls < *rs;
            if (binary->op == TokenKind::LtEq) return *ls <= *rs;
            if (binary->op == TokenKind::Gt) return *ls > *rs;
            if (binary->op == TokenKind::GtEq) return *ls >= *rs;
        }
        return std::nullopt;
    }
    if (auto* assignment = dynamic_cast<AssignExpr*>(expr)) {
        auto* identifier = dynamic_cast<IdentifierExpr*>(assignment->lhs.get());
        if (!identifier || !locals.count(identifier->name)) return std::nullopt;
        auto value = evaluateSelectorExpr(assignment->rhs.get(), locals);
        if (!value) return std::nullopt;
        if (assignment->op == TokenKind::Eq) {
            locals[identifier->name] = *value;
            return *value;
        }
        const TokenKind operation =
            assignment->op == TokenKind::PlusEq ? TokenKind::Plus :
            assignment->op == TokenKind::MinusEq ? TokenKind::Minus :
            assignment->op == TokenKind::StarEq ? TokenKind::Star :
            assignment->op == TokenKind::SlashEq ? TokenKind::Slash :
            TokenKind::Percent;
        auto current = locals[identifier->name];
        auto li = std::get_if<int64_t>(&current);
        auto ri = std::get_if<int64_t>(&*value);
        if (!li || !ri) return std::nullopt;
        int64_t updated = *li;
        if (operation == TokenKind::Plus) updated += *ri;
        else if (operation == TokenKind::Minus) updated -= *ri;
        else if (operation == TokenKind::Star) updated *= *ri;
        else if (operation == TokenKind::Slash && *ri != 0) updated /= *ri;
        else if (operation == TokenKind::Percent && *ri != 0) updated %= *ri;
        else return std::nullopt;
        locals[identifier->name] = updated;
        return updated;
    }
    if (auto* call = dynamic_cast<CallExpr*>(expr)) {
        auto* callee = dynamic_cast<IdentifierExpr*>(call->callee.get());
        if (!callee) return std::nullopt;
        if (callee->name == "declaration_of" && call->args.size() == 1) {
            auto* target =
                dynamic_cast<IdentifierExpr*>(call->args.front().get());
            if (!target) return std::nullopt;
            auto family = mContext.mFunctionFamilies.find(
                mContext.sourceDeclarationKey(target->name, false));
            if (family == mContext.mFunctionFamilies.end()) return std::nullopt;
            TypePtr requested;
            if (call->typeArgASTs.size() == 1)
                requested = mContext.resolved(mContext.resolveTypeAST(
                    call->typeArgASTs.front().get(), {}));
            FunctionDecl* selected = nullptr;
            for (auto* candidate : family->second) {
                TypeVec parameters;
                std::vector<luna::ownership::Contract> contracts;
                for (const auto& parameter : candidate->params) {
                    parameters.push_back(mContext.resolved(parameter.inferredType));
                    contracts.push_back(
                        {parameter.relation, parameter.usage});
                }
                TypePtr callable = Type::makeFunction(
                    std::move(parameters),
                    mContext.resolved(candidate->inferredReturnType),
                    std::move(contracts),
                    {luna::ownership::Relation::Owned,
                     candidate->returnUsage});
                if (requested &&
                    !luna::types::sameType(requested, callable))
                    continue;
                if (selected) return std::nullopt;
                selected = candidate;
            }
            if (!selected) return std::nullopt;
            const auto symbol = selected->generatedSymbolName.empty()
                ? selected->name : selected->generatedSymbolName;
            return SelectorDeclarationValue{
                nominalDeclarationIdentity(
                    mContext.mProgram, "fn", symbol, selected)};
        }
        std::vector<SelectorValue> arguments;
        for (auto& argument : call->args) {
            auto value = evaluateSelectorExpr(argument.get(), locals);
            if (!value) return std::nullopt;
            arguments.push_back(std::move(*value));
        }
        if (callee->name == "declaration_count" && arguments.size() == 1) {
            auto* view = std::get_if<SelectorDeclarationViewValue>(&arguments[0]);
            if (view) return static_cast<int64_t>(view->declarationIds.size());
            return std::nullopt;
        }
        if (callee->name == "declaration_at" && arguments.size() == 2) {
            auto* view = std::get_if<SelectorDeclarationViewValue>(&arguments[0]);
            auto* index = std::get_if<int64_t>(&arguments[1]);
            if (!view || !index || *index < 0 ||
                static_cast<size_t>(*index) >= view->declarationIds.size())
                return std::nullopt;
            return SelectorDeclarationValue{
                view->declarationIds[static_cast<size_t>(*index)]};
        }
        if ((callee->name == "declaration_id" ||
             callee->name == "declaration_signature") &&
            arguments.size() == 1) {
            auto* declaration =
                std::get_if<SelectorDeclarationValue>(&arguments[0]);
            if (!declaration) return std::nullopt;
            if (callee->name == "declaration_id")
                return declaration->declarationId;
            const auto* candidate = mContext.mActiveSelectorView
                ? mContext.mActiveSelectorView->find(declaration->declarationId) : nullptr;
            if (!candidate || !candidate->callableType)
                return std::nullopt;
            return SelectorValue(
                luna::types::typeId(candidate->callableType).value);
        }
        if ((callee->name == "metadata" ||
             callee->name == "declaration_has_metadata") &&
            arguments.size() == 1 && call->typeArgASTs.size() == 1) {
            auto* declaration =
                std::get_if<SelectorDeclarationValue>(&arguments[0]);
            if (!declaration || !mContext.mActiveSelectorView) return std::nullopt;
            TypePtr schema = mContext.resolved(
                mContext.resolveTypeAST(call->typeArgASTs.front().get(), {}));
            const auto* candidate =
                mContext.mActiveSelectorView->find(declaration->declarationId);
            if (!candidate || schema->kind != TypeKind::Metadata)
                return std::nullopt;
            SelectorMetadataViewValue matches;
            for (const auto& metadata : candidate->metadata) {
                if (metadata.schemaId == schema->nominalId)
                    matches.values.push_back(
                        {metadata.schemaId, metadata.values});
            }
            if (callee->name == "declaration_has_metadata")
                return !matches.values.empty();
            return matches;
        }
        if (callee->name == "select_unique" && arguments.size() == 2) {
            auto* view = std::get_if<SelectorDeclarationViewValue>(&arguments[0]);
            auto* wanted = std::get_if<SelectorMetadataValue>(&arguments[1]);
            if (!view || !wanted || !mContext.mActiveSelectorView) return std::nullopt;
            std::optional<std::string> match;
            for (const auto& id : view->declarationIds) {
                const auto* candidate = mContext.mActiveSelectorView->find(id);
                if (!candidate) continue;
                for (const auto& metadata : candidate->metadata) {
                    if (metadata.schemaId != wanted->schemaId ||
                        metadata.values != wanted->fields)
                        continue;
                    if (match) return std::nullopt;
                    match = id;
                }
            }
            return SelectorDeclarationValue{match ? *match : std::string()};
        }
        const std::string metadataKey =
            mContext.sourceDeclarationKey(callee->name, false);
        auto metadata = mContext.mMetadataSchemas.find(metadataKey);
        if (metadata != mContext.mMetadataSchemas.end()) {
            std::vector<ConstValue> fields;
            for (const auto& argument : arguments) {
                if (auto* value = std::get_if<int64_t>(&argument))
                    fields.push_back(*value);
                else if (auto* value = std::get_if<double>(&argument))
                    fields.push_back(*value);
                else if (auto* value = std::get_if<bool>(&argument))
                    fields.push_back(*value);
                else if (auto* value = std::get_if<std::string>(&argument))
                    fields.push_back(*value);
                else return std::nullopt;
            }
            const auto symbol = metadata->second->generatedSymbolName.empty()
                ? metadata->second->name
                : metadata->second->generatedSymbolName;
            return SelectorMetadataValue{
                nominalDeclarationIdentity(
                    mContext.mProgram, "meta", symbol, metadata->second),
                std::move(fields)};
        }
        auto function = mContext.mFunctionFamilies.find(
            mContext.sourceDeclarationKey(callee->name, false));
        if (function == mContext.mFunctionFamilies.end() ||
            function->second.size() != 1 ||
            !function->second.front()->isConstexpr ||
            function->second.front()->params.size() != arguments.size())
            return std::nullopt;
        if (++mContext.mConstEvaluationDepth > 128) {
            --mContext.mConstEvaluationDepth;
            return std::nullopt;
        }
        std::unordered_map<std::string, SelectorValue> functionLocals;
        for (size_t index = 0; index < arguments.size(); ++index)
            functionLocals[function->second.front()->params[index].name] =
                arguments[index];
        std::optional<SelectorValue> result;
        bool returned = false;
        const bool evaluated = evaluateSelectorBlock(
                function->second.front()->body.get(), functionLocals,
                result, returned);
        --mContext.mConstEvaluationDepth;
        if (!evaluated || !returned)
            return std::nullopt;
        return result;
    }
    return std::nullopt;
}


bool CompileTimeEvaluator::evaluateSelectorBlock(
    BlockStmt* block, std::unordered_map<std::string, SelectorValue>& locals,
    std::optional<SelectorValue>& result, bool& returned) {
    if (!block) return false;
    for (auto& statement : block->stmts) {
        if (auto* binding = dynamic_cast<LetStmt*>(statement.get())) {
            auto value = evaluateSelectorExpr(binding->initializer.get(), locals);
            if (!value) return false;
            locals[binding->name] = std::move(*value);
        } else if (auto* ret = dynamic_cast<ReturnStmt*>(statement.get())) {
            if (!ret->value) return false;
            result = evaluateSelectorExpr(ret->value.get(), locals);
            returned = result.has_value();
            return returned;
        } else if (auto* conditional =
                       dynamic_cast<IfStmt*>(statement.get())) {
            auto condition =
                evaluateSelectorExpr(conditional->cond.get(), locals);
            auto* boolean = condition
                ? std::get_if<bool>(&*condition) : nullptr;
            if (!boolean) return false;
            if (*boolean) {
                if (!evaluateSelectorBlock(
                        conditional->thenBlock.get(), locals, result, returned))
                    return false;
            } else if (conditional->elseBranch) {
                if (auto* block = dynamic_cast<BlockStmt*>(
                        conditional->elseBranch.get())) {
                    if (!evaluateSelectorBlock(
                            block, locals, result, returned))
                        return false;
                } else {
                    auto wrapper = dynamic_cast<IfStmt*>(
                        conditional->elseBranch.get());
                    if (!wrapper) return false;
                    auto conditionValue =
                        evaluateSelectorExpr(wrapper->cond.get(), locals);
                    auto* nested = conditionValue
                        ? std::get_if<bool>(&*conditionValue) : nullptr;
                    if (!nested) return false;
                    if (*nested && !evaluateSelectorBlock(
                            wrapper->thenBlock.get(), locals, result, returned))
                        return false;
                    if (!*nested && wrapper->elseBranch) {
                        auto* nestedElse = dynamic_cast<BlockStmt*>(
                            wrapper->elseBranch.get());
                        if (!nestedElse || !evaluateSelectorBlock(
                                nestedElse, locals, result, returned))
                            return false;
                    }
                }
            }
            if (returned) return true;
        } else if (auto* loop = dynamic_cast<ForStmt*>(statement.get())) {
            auto iterable =
                evaluateSelectorExpr(loop->iterable.get(), locals);
            if (!iterable) return false;
            std::vector<SelectorValue> elements;
            if (auto* declarations =
                    std::get_if<SelectorDeclarationViewValue>(&*iterable)) {
                for (const auto& id : declarations->declarationIds)
                    elements.push_back(SelectorDeclarationValue{id});
            } else if (auto* metadata =
                           std::get_if<SelectorMetadataViewValue>(&*iterable)) {
                for (const auto& value : metadata->values)
                    elements.push_back(value);
            } else {
                return false;
            }
            auto previous = locals.find(loop->varName);
            std::optional<SelectorValue> saved =
                previous == locals.end()
                ? std::nullopt
                : std::optional<SelectorValue>(previous->second);
            for (auto& element : elements) {
                locals[loop->varName] = std::move(element);
                if (!evaluateSelectorBlock(
                        loop->body.get(), locals, result, returned))
                    return false;
                if (returned) return true;
            }
            if (saved) locals[loop->varName] = *saved;
            else locals.erase(loop->varName);
        } else if (auto* loop = dynamic_cast<WhileStmt*>(statement.get())) {
            for (size_t iteration = 0; iteration < 10000; ++iteration) {
                auto condition =
                    evaluateSelectorExpr(loop->cond.get(), locals);
                auto* boolean = condition
                    ? std::get_if<bool>(&*condition) : nullptr;
                if (!boolean) return false;
                if (!*boolean) break;
                if (!evaluateSelectorBlock(
                        loop->body.get(), locals, result, returned))
                    return false;
                if (returned) return true;
                if (iteration == 9999) return false;
            }
        } else if (auto* expression =
                       dynamic_cast<ExprStmt*>(statement.get())) {
            if (!evaluateSelectorExpr(expression->expr.get(), locals))
                return false;
        } else {
            return false;
        }
    }
    return true;
}


std::optional<std::string> CompileTimeEvaluator::evaluateSelectorFunction(
    FunctionDecl* function, const luna::selector::DeclarationView& view,
    const std::vector<ConstValue>& arguments, std::string& failure) {
    failure.clear();
    if (!function || function->params.size() != arguments.size() + 1) {
        failure = "selector invocation does not match its declaration";
        return std::nullopt;
    }
    std::unordered_map<std::string, SelectorValue> locals;
    SelectorDeclarationViewValue input;
    for (const auto& candidate : view.candidates())
        input.declarationIds.push_back(candidate.declarationId);
    locals[function->params.front().name] = std::move(input);
    for (size_t index = 0; index < arguments.size(); ++index) {
        locals[function->params[index + 1].name] =
            std::visit([](const auto& item) -> SelectorValue {
                return item;
            }, arguments[index]);
    }
    const auto* previousView = mContext.mActiveSelectorView;
    const std::string previousPackage = mContext.mCurrentPackageId;
    const std::string previousModule = mContext.mCurrentModulePath;
    mContext.setDeclarationContext(function);
    mContext.mActiveSelectorView = &view;
    if (++mContext.mConstEvaluationDepth > 128) {
        --mContext.mConstEvaluationDepth;
        mContext.mActiveSelectorView = previousView;
        mContext.mCurrentPackageId = previousPackage;
        mContext.mCurrentModulePath = previousModule;
        failure = "selector recursion depth exceeded 128";
        return std::nullopt;
    }
    std::optional<SelectorValue> result;
    bool returned = false;
    const bool evaluated = evaluateSelectorBlock(
        function->body.get(), locals, result, returned);
    --mContext.mConstEvaluationDepth;
    mContext.mActiveSelectorView = previousView;
    mContext.mCurrentPackageId = previousPackage;
    mContext.mCurrentModulePath = previousModule;
    if (!evaluated) {
        failure = "selector body is not compile-time evaluable";
        return std::nullopt;
    }
    if (!returned || !result) {
        failure = "selector returned no declaration";
        return std::nullopt;
    }
    auto* declaration = std::get_if<SelectorDeclarationValue>(&*result);
    if (!declaration) {
        failure = "selector result is not a declaration_ref";
        return std::nullopt;
    }
    if (declaration->declarationId.empty()) {
        failure = "selector returned no legal declaration";
        return std::nullopt;
    }
    return declaration->declarationId;
}
