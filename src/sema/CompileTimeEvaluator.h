#pragma once

#include "SemanticContextAccess.h"

class CompileTimeEvaluator final : public CompileTimeAnalysis {
public:
    explicit CompileTimeEvaluator(CompileTimeContextAccess context)
        : mContext(std::move(context)) {}

    TypePtr analyzeReflectionCall(
        CallExpr* call, const std::string& name) override;
    TypePtr analyzeDeclarationReflectionCall(
        CallExpr* call, const std::string& name) override;
    void enterConstScope() override;
    void exitConstScope() override;
    void defineConst(
        const std::string& name, const SemanticConstValue& value) override;
    const SemanticConstValue* lookupConst(
        const std::string& name) const override;
    std::optional<SemanticConstValue> evaluateConstExpr(
        Expr* expr,
        const std::unordered_map<std::string, SemanticConstValue>& locals) override;
    std::optional<SemanticConstValue> evaluateConstFunction(
        FunctionDecl* function,
        const std::vector<SemanticConstValue>& arguments) override;
    bool evaluateConstBlock(
        BlockStmt* block,
        std::unordered_map<std::string, SemanticConstValue>& locals,
        std::optional<SemanticConstValue>& result) override;
    std::optional<SemanticConstValue> evaluateConstraintExpr(
        Expr* expr,
        const std::unordered_map<std::string, TypePtr>& bindings,
        std::vector<std::string>& active) override;
    std::optional<bool> evaluateConstraint(
        const std::string& name, const TypeVec& arguments,
        std::vector<std::string>& active) override;
    std::optional<SemanticSelectorValue> evaluateSelectorExpr(
        Expr* expr,
        std::unordered_map<std::string, SemanticSelectorValue>& locals) override;
    bool evaluateSelectorBlock(
        BlockStmt* block,
        std::unordered_map<std::string, SemanticSelectorValue>& locals,
        std::optional<SemanticSelectorValue>& result,
        bool& returned) override;
    std::optional<std::string> evaluateSelectorFunction(
        FunctionDecl* function,
        const luna::selector::SymbolSet& symbols,
        const std::vector<SemanticConstValue>& arguments,
        std::string& failure) override;

private:
    using ConstValue = SemanticConstValue;
    using SelectorDeclarationValue = SemanticSelectorDeclarationValue;
    using SelectorMetadataValue = SemanticSelectorMetadataValue;
    using SelectorDeclarationViewValue =
        SemanticSelectorDeclarationViewValue;
    using SelectorMetadataViewValue = SemanticSelectorMetadataViewValue;
    using SelectorValue = SemanticSelectorValue;

    CompileTimeContextAccess mContext;
    std::string mSelectorEvaluationFailure;
};
