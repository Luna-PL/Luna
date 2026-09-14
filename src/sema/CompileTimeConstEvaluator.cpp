#include "CompileTimeEvaluator.h"

#include "../core/TypeLayout.h"
#include "../core/TypeRelations.h"
#include "../parser/AST.h"
#include "../selector/Selector.h"
#include "SemanticAnalysisSupport.h"
#include <cmath>
#include <unordered_set>
#include <utility>

void CompileTimeEvaluator::enterConstScope() { mContext.mConstScopes.emplace_back(); }

void CompileTimeEvaluator::exitConstScope() {
    if (mContext.mConstScopes.size() > 1) mContext.mConstScopes.pop_back();
}

void CompileTimeEvaluator::defineConst(const std::string& name, const ConstValue& value) {
    if (mContext.mConstScopes.empty()) enterConstScope();
    mContext.mConstScopes.back()[name] = value;
}

const CompileTimeEvaluator::ConstValue*
CompileTimeEvaluator::lookupConst(const std::string& name) const {
    for (auto it = mContext.mConstScopes.rbegin(); it != mContext.mConstScopes.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return &found->second;
    }
    return nullptr;
}

std::optional<CompileTimeEvaluator::ConstValue>

CompileTimeEvaluator::evaluateConstExpr(Expr* expr,
                                        const std::unordered_map<std::string, ConstValue>& locals) {
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
        const std::string& constexprName =
            call->resolvedSymbolName.empty() ? callee->name : call->resolvedSymbolName;
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
        auto li = std::get_if<int64_t>(&*lhs);
        auto ri = std::get_if<int64_t>(&*rhs);
        if (li && ri) {
            switch (binary->op) {
            case TokenKind::Plus:
                return *li + *ri;
            case TokenKind::Minus:
                return *li - *ri;
            case TokenKind::Star:
                return *li * *ri;
            case TokenKind::Slash:
                if (*ri) return *li / *ri;
                break;
            case TokenKind::Percent:
                if (*ri) return *li % *ri;
                break;
            case TokenKind::Ampersand:
                return *li & *ri;
            case TokenKind::BitOr:
                return *li | *ri;
            case TokenKind::BitXor:
                return *li ^ *ri;
            case TokenKind::ShiftLeft:
                return *li << *ri;
            case TokenKind::ShiftRight:
                return *li >> *ri;
            case TokenKind::EqEq:
                return *li == *ri;
            case TokenKind::Neq:
                return *li != *ri;
            case TokenKind::Lt:
                return *li < *ri;
            case TokenKind::LtEq:
                return *li <= *ri;
            case TokenKind::Gt:
                return *li > *ri;
            case TokenKind::GtEq:
                return *li >= *ri;
            default:
                break;
            }
        }
        auto lf = std::get_if<double>(&*lhs);
        auto rf = std::get_if<double>(&*rhs);
        if (lf && rf) {
            switch (binary->op) {
            case TokenKind::Plus:
                return *lf + *rf;
            case TokenKind::Minus:
                return *lf - *rf;
            case TokenKind::Star:
                return *lf * *rf;
            case TokenKind::Slash:
                if (*rf != 0) return *lf / *rf;
                break;
            case TokenKind::Percent:
                if (*rf != 0) return std::fmod(*lf, *rf);
                break;
            case TokenKind::EqEq:
                return *lf == *rf;
            case TokenKind::Neq:
                return *lf != *rf;
            case TokenKind::Lt:
                return *lf < *rf;
            case TokenKind::LtEq:
                return *lf <= *rf;
            case TokenKind::Gt:
                return *lf > *rf;
            case TokenKind::GtEq:
                return *lf >= *rf;
            default:
                break;
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
    }
    return std::nullopt;
}

std::optional<CompileTimeEvaluator::ConstValue>

CompileTimeEvaluator::evaluateConstFunction(FunctionDecl* function,
                                            const std::vector<ConstValue>& args) {
    if (!function || !function->isConstexpr || function->params.size() != args.size())
        return std::nullopt;
    if (++mContext.mConstEvaluationDepth > 128) {
        --mContext.mConstEvaluationDepth;
        return std::nullopt;
    }
    std::unordered_map<std::string, ConstValue> locals;
    for (size_t i = 0; i < args.size(); ++i)
        locals[function->params[i].name] = args[i];
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
            case TokenKind::Plus:
                return *li + *ri;
            case TokenKind::Minus:
                return *li - *ri;
            case TokenKind::Star:
                return *li * *ri;
            case TokenKind::Slash:
                if (*ri != 0) return *li / *ri;
                break;
            case TokenKind::Percent:
                if (*ri != 0) return *li % *ri;
                break;
            case TokenKind::EqEq:
                return *li == *ri;
            case TokenKind::Neq:
                return *li != *ri;
            case TokenKind::Lt:
                return *li < *ri;
            case TokenKind::LtEq:
                return *li <= *ri;
            case TokenKind::Gt:
                return *li > *ri;
            case TokenKind::GtEq:
                return *li >= *ri;
            default:
                break;
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
    auto* callee = call ? dynamic_cast<IdentifierExpr*>(call->callee.get()) : nullptr;
    if (!call || !callee || !call->args.empty()) return std::nullopt;

    const std::string conceptKey = mContext.sourceDeclarationKey(callee->name, false);
    if (mContext.mConcepts.count(conceptKey)) {
        TypeVec arguments;
        for (auto& argument : call->typeArgASTs)
            arguments.push_back(
                mContext.resolved(mContext.resolveTypeAST(argument.get(), bindings)));
        auto value = evaluateConstraint(conceptKey, arguments, active);
        return value ? std::optional<ConstValue>(*value) : std::nullopt;
    }

    auto resolveArgument = [&](size_t index) -> TypePtr {
        if (index >= call->typeArgASTs.size()) return TyUnknown;
        return mContext.resolved(mContext.resolveTypeAST(call->typeArgASTs[index].get(), bindings));
    };
    const bool binaryRelation = callee->name == "type_same" || callee->name == "type_same_shape" ||
                                callee->name == "type_abi_compatible";
    if (binaryRelation) {
        if (call->typeArgASTs.size() != 2) return std::nullopt;
        TypePtr lhs = resolveArgument(0);
        TypePtr rhs = resolveArgument(1);
        if (callee->name == "type_same") return luna::types::sameType(lhs, rhs);
        if (callee->name == "type_same_shape") return luna::types::sameShape(lhs, rhs);
        return luna::types::isAbiCompatible(lhs, rhs);
    }
    if (call->typeArgASTs.size() != 1) return std::nullopt;
    TypePtr type = resolveArgument(0);
    if (!type || type->kind == TypeKind::Unknown || type->kind == TypeKind::TypeParam ||
        type->kind == TypeKind::InferenceVar)
        return std::nullopt;
    if (callee->name == "type_is_struct") return type->kind == TypeKind::Struct;
    if (callee->name == "type_is_enum") return type->kind == TypeKind::Enum;
    if (callee->name == "type_is_nominal") return !type->nominalId.empty();
    if (callee->name == "type_is_structural")
        return type->identityMode == luna::types::IdentityMode::Structural;
    if (callee->name == "type_is_meta") return type->domain == luna::types::TypeDomain::Meta;
    if (callee->name == "type_is_reference") return type->kind == TypeKind::Reference;
    if (callee->name == "type_field_count") {
        if (type->kind != TypeKind::Struct && type->kind != TypeKind::Record) return std::nullopt;
        return static_cast<int64_t>(type->fields.size());
    }
    if (callee->name == "type_variant_count") {
        if (type->kind != TypeKind::Enum) return std::nullopt;
        return static_cast<int64_t>(type->variants.size());
    }
    if (callee->name == "type_id") return luna::types::typeId(type).value;
    if (callee->name == "type_shape") return luna::types::shapeId(type).value;
    if (callee->name == "type_size") return static_cast<int64_t>(luna::layout::valueSize(type));
    if (callee->name == "type_alignment")
        return static_cast<int64_t>(luna::layout::valueAlignment(type));
    return std::nullopt;
}

std::optional<bool> CompileTimeEvaluator::evaluateConstraint(const std::string& name,
                                                             const TypeVec& arguments,
                                                             std::vector<std::string>& active) {
    auto found = mContext.mConcepts.find(name);
    if (found == mContext.mConcepts.end()) {
        const auto key = mContext.sourceDeclarationKey(name, false);
        found = mContext.mConcepts.find(key);
    }
    if (found == mContext.mConcepts.end() || found->second->typeParams.size() != arguments.size())
        return std::nullopt;
    if (std::find(active.begin(), active.end(), found->first) != active.end()) return std::nullopt;
    active.push_back(found->first);
    std::unordered_map<std::string, TypePtr> bindings;
    for (size_t index = 0; index < arguments.size(); ++index)
        bindings[found->second->typeParams[index]] = arguments[index];
    auto value = evaluateConstraintExpr(found->second->predicate.get(), bindings, active);
    active.pop_back();
    if (!value) return std::nullopt;
    if (auto* boolean = std::get_if<bool>(&*value)) return *boolean;
    return std::nullopt;
}
