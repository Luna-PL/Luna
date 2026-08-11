#pragma once

#include "MoonIR.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace moon {

// Construction-only bridge from the typed structured block produced by the
// frontend lowerer to the single canonical CFG body. The structured input is
// consumed and never becomes part of a sealed Module.
class ControlFlowBuilder {
public:
    std::unique_ptr<ControlFlowGraph> build(
        std::unique_ptr<BlockStmt> root,
        const std::vector<Param>& parameters,
        RegionKind rootKind,
        const Module& module);

    const std::vector<std::string>& errors() const { return mErrors; }

private:
    struct OpenBlock {
        BlockId block;
        std::vector<CleanupId> cleanups;
    };

    struct BuiltBlock {
        RegionId region;
        ScopeId scope;
        BlockId entry;
        std::optional<OpenBlock> exit;
    };

    struct IteratorRecipeStep {
        IteratorOp op = IteratorOp::None;
        std::unique_ptr<Expr> argument;
        TypeRef inputType;
        TypeRef outputType;
    };

    struct IteratorRecipePlan {
        IteratorMode mode = IteratorMode::Copy;
        std::unique_ptr<Expr> source;
        TypeRef sourceType;
        std::unique_ptr<Expr> rangeStart;
        std::unique_ptr<Expr> rangeEnd;
        TypeRef itemType;
        std::vector<IteratorRecipeStep> steps;
    };

    RegionId addRegion(RegionId parent, RegionKind kind,
                       const SourceLocation& location);
    ScopeId addScope(ScopeId parent, RegionId region,
                     const SourceLocation& location);
    BlockId addBlock(RegionId region, ScopeId scope,
                     const SourceLocation& location);
    LocalId addLocal(ScopeId scope, LocalKind kind,
                     const std::string& name, const TypeRef& type,
                     luna::ownership::Usage usage,
                     std::optional<luna::ownership::Relation> relation =
                         std::nullopt);
    CleanupId addCleanup(LocalId local, const TypeRef& type,
                         luna::ownership::CleanupAction action);

    BuiltBlock lowerNestedBlock(std::unique_ptr<BlockStmt> block,
                                RegionId parentRegion,
                                ScopeId parentScope,
                                RegionKind kind = RegionKind::Lexical);
    std::optional<OpenBlock> lowerSequence(
        std::vector<std::unique_ptr<Stmt>>& statements,
        OpenBlock current, RegionId region, ScopeId scope);
    std::optional<OpenBlock> lowerStatement(
        std::unique_ptr<Stmt> statement,
        OpenBlock current, RegionId region, ScopeId scope);
    std::optional<OpenBlock> lowerIf(
        std::unique_ptr<IfStmt> statement,
        OpenBlock current, RegionId region, ScopeId scope);
    std::optional<OpenBlock> lowerWhile(
        std::unique_ptr<WhileStmt> statement,
        OpenBlock current, RegionId region, ScopeId scope);
    std::optional<OpenBlock> lowerFor(
        std::unique_ptr<ForStmt> statement,
        OpenBlock current, RegionId region, ScopeId scope);
    std::optional<OpenBlock> lowerIteratorRecipeFor(
        std::unique_ptr<ForStmt> statement,
        OpenBlock current, RegionId region, ScopeId scope);
    std::optional<OpenBlock> lowerMatch(
        std::unique_ptr<MatchStmt> statement,
        OpenBlock current, RegionId region, ScopeId scope);

    bool bindExpr(Expr* expression);
    bool parseIteratorRecipe(
        std::unique_ptr<Expr> expression,
        IteratorRecipePlan& plan,
        const SourceLocation& location);
    LocalId lookupLocal(const std::string& name) const;
    void connectJump(const OpenBlock& source, BlockId target);
    std::vector<CleanupId> lowerCleanupObligations(
        const std::vector<CleanupObligation>& obligations,
        ScopeId sourceScope);
    std::vector<CleanupId> canonicalCleanupOrder(
        const std::vector<CleanupId>& active, ScopeId sourceScope,
        std::optional<ScopeId> targetScope) const;
    void canonicalizeCleanupTable();
    void error(const SourceLocation& location, const std::string& message);

    const Module* mModule = nullptr;
    ControlFlowGraph* mGraph = nullptr;
    std::vector<std::unordered_map<std::string, LocalId>> mBindings;
    std::unordered_map<uint32_t, CleanupId> mCleanupByLocal;
    std::vector<std::string> mErrors;
    bool mBindingIteratorRecipe = false;
};

} // namespace moon
