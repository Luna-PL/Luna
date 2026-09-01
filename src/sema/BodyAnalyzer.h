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

    // Active while analyzing a lambda body. Free-variable references from
    // scopes outside the lambda's own scope are recorded as captures in
    // first-reference order; the lambda analysis then builds the Closure type
    // from them (C016 CL003/CL009).
    struct CaptureFrame {
        size_t lambdaScopeDepth = 0;
        std::vector<std::string> captures;
    };
    std::vector<CaptureFrame> mCaptureFrames;
    // Inactive arms of a compile-time Option match are still type-checked,
    // but an absent Some payload intentionally has no catalog identity.
    bool mInCompileTimeInactiveBranch = false;

    luna::ownership::Usage inherentUsageForInitializer(
        Expr* initializer, const TypePtr& type);
    luna::ownership::Usage finalizeBindingUsage(
        const std::string& name, const TypePtr& type, Expr* initializer,
        luna::ownership::Usage requested, bool isExplicit,
        int line, int column);

    // Large analyzeStmt/analyzeExpr branches split out for readability;
    // behavior unchanged.
    TypePtr analyzeLetStmt(LetStmt* stmt, TypePtr expectedReturn);
    TypePtr analyzeMatchStmt(MatchStmt* stmt, TypePtr expectedReturn);
    TypePtr analyzeForStmt(ForStmt* stmt, TypePtr expectedReturn);
    TypePtr analyzeLambdaExpr(LambdaExpr* expr);
    TypePtr analyzeVariantConstructExpr(VariantConstructExpr* expr);
    TypePtr analyzeRecordLiteralExpr(RecordLiteralExpr* expr);
    TypePtr analyzeTryExpr(TryExpr* expr);
    // Intrinsic/builtin call handling split out from analyzeCall.
    // Returns nullptr if the call is not a recognized intrinsic.
    TypePtr analyzeIntrinsicCall(CallExpr* call, IdentifierExpr* id);
};
