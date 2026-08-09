#pragma once

#include "SemanticContextAccess.h"

class BodyAnalyzer final : public BodyAnalysis {
public:
    explicit BodyAnalyzer(BodyContextAccess context)
        : mContext(std::move(context)) {}

    void analyzeFunction(FunctionDecl* decl) override;
    void analyzeStruct(StructDecl* decl) override;
    void analyzeEnum(EnumDecl* decl) override;
    void analyzeTrait(TraitDecl* decl) override;
    void analyzeImpl(ImplDecl* decl) override;
    TypePtr analyzeStmt(Stmt* stmt, TypePtr expectedReturn) override;
    TypePtr analyzeBlock(
        BlockStmt* block, TypePtr expectedReturn) override;
    TypePtr analyzeExpr(Expr* expr) override;
    TypePtr analyzeCall(CallExpr* call) override;
    TypePtr analyzeMemberCall(
        CallExpr* call, FieldAccessExpr* member) override;
    TypePtr analyzeIteratorCall(
        CallExpr* call, FieldAccessExpr* member) override;
    TypePtr analyzeLaunch(LaunchExpr* launch) override;
    TypePtr analyzeSelect(SelectExpr* selection) override;
    bool statementAlwaysReturns(const Stmt* stmt) const override;
    bool blockAlwaysReturns(const BlockStmt* block) const override;

private:
    using ConstValue = BodyContextAccess::ConstValue;
    using FromConversion = BodyContextAccess::FromConversion;
    using FromIteratorImplementation =
        BodyContextAccess::FromIteratorImplementation;

    BodyContextAccess mContext;

    luna::ownership::Usage inherentUsageForInitializer(
        Expr* initializer, const TypePtr& type);
    luna::ownership::Usage finalizeBindingUsage(
        const std::string& name, const TypePtr& type, Expr* initializer,
        luna::ownership::Usage requested, bool isExplicit,
        int line, int column);
};
