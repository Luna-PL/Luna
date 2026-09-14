#include "CompileTimeEvaluator.h"

#include "SemanticAnalysisSupport.h"
#include "../core/TypeLayout.h"
#include "../core/TypeRelations.h"
#include "../parser/AST.h"
#include "../selector/Selector.h"
#include <cmath>
#include <unordered_set>
#include <utility>

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
            return SelectorDeclarationValue{
                functionDeclarationIdentity(
                    mContext.mProgram, selected)};
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
            const auto* candidate = mContext.mActiveSelectorSet
                ? mContext.mActiveSelectorSet->findDeclaration(
                    declaration->declarationId) : nullptr;
            if (!candidate || !candidate->type ||
                candidate->kind !=
                    luna::selector::CatalogSymbolKind::Function)
                return std::nullopt;
            return SelectorValue(
                luna::types::typeId(candidate->type).value);
        }
        if ((callee->name == "metadata" ||
             callee->name == "declaration_has_metadata") &&
            arguments.size() == 1 && call->typeArgASTs.size() == 1) {
            auto* declaration =
                std::get_if<SelectorDeclarationValue>(&arguments[0]);
            if (!declaration || !mContext.mActiveSelectorSet) return std::nullopt;
            TypePtr schema = mContext.resolved(
                mContext.resolveTypeAST(call->typeArgASTs.front().get(), {}));
            const auto* candidate =
                mContext.mActiveSelectorSet->findDeclaration(
                    declaration->declarationId);
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
            if (!view || !wanted || !mContext.mActiveSelectorSet) return std::nullopt;
            std::vector<luna::identity::SymbolId> viewSymbols;
            viewSymbols.reserve(view->declarationIds.size());
            for (const auto& id : view->declarationIds) {
                viewSymbols.push_back(
                    luna::identity::symbolIdFromCanonical(id));
            }
            auto terminal = mContext.mActiveSelectorSet
                ->select(viewSymbols)
                .filterMetadata(wanted->schemaId, wanted->fields)
                .one();
            if (!terminal.oneSucceeded()) {
                mSelectorEvaluationFailure = terminal.message;
                return SelectorDeclarationValue{};
            }
            return SelectorDeclarationValue{
                terminal.selected->declarationId};
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
    FunctionDecl* function, const luna::selector::SymbolSet& symbols,
    const std::vector<ConstValue>& arguments, std::string& failure) {
    failure.clear();
    mSelectorEvaluationFailure.clear();
    if (!function || function->params.size() != arguments.size() + 1) {
        failure = "selector invocation does not match its declaration";
        return std::nullopt;
    }
    std::unordered_map<std::string, SelectorValue> locals;
    SelectorDeclarationViewValue input;
    for (const auto* candidate : symbols.orderedSymbols())
        input.declarationIds.push_back(candidate->declarationId);
    locals[function->params.front().name] = std::move(input);
    for (size_t index = 0; index < arguments.size(); ++index) {
        locals[function->params[index + 1].name] =
            std::visit([](const auto& item) -> SelectorValue {
                return item;
            }, arguments[index]);
    }
    const auto* previousSet = mContext.mActiveSelectorSet;
    const std::string previousPackage = mContext.mCurrentPackageId;
    const std::string previousModule = mContext.mCurrentModulePath;
    mContext.setDeclarationContext(function);
    mContext.mActiveSelectorSet = &symbols;
    if (++mContext.mConstEvaluationDepth > 128) {
        --mContext.mConstEvaluationDepth;
        mContext.mActiveSelectorSet = previousSet;
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
    mContext.mActiveSelectorSet = previousSet;
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
        failure = mSelectorEvaluationFailure.empty()
            ? "selector returned no legal declaration"
            : mSelectorEvaluationFailure;
        return std::nullopt;
    }
    return declaration->declarationId;
}
