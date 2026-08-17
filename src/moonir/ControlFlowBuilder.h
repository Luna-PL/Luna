#pragma once

#include "MoonIR.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace moon {

// Construction-only bridge from the typed structured block produced by the
// frontend lowerer to the single canonical CFG body. The structured input is
// consumed and never becomes part of a sealed Module.
class ControlFlowBuilder {
public:
    // Non-consuming construction is used by the atomic module sealer: all
    // candidate graphs are built and verified before any source body is
    // removed from its declaration.
    std::unique_ptr<ControlFlowGraph> build(
        const BlockStmt& root,
        const std::vector<Param>& parameters,
        RegionKind rootKind,
        const Module& module);

    std::unique_ptr<ControlFlowGraph> build(
        std::unique_ptr<BlockStmt> root,
        const std::vector<Param>& parameters,
        RegionKind rootKind,
        const Module& module);

    // Capturing lambdas set this before build(): the lambda CFG then declares
    // the synthetic environment parameter local and rewrites capture reads to
    // EnvLoad operations (C016 CL009).
    void setCaptureEnvironment(
        const std::vector<std::string>& captures,
        const TypeRef& closureType,
        const std::string& envParamName) {
        mCaptureNames = captures;
        mCaptureClosureType = closureType;
        mCaptureEnvParamName = envParamName;
        mCaptureEnvLocal = {};
    }

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

    struct FragmentContext {
        BlockId exit;
        FragmentKind kind = FragmentKind::Interceptor;
        const BlockStmt* continuation = nullptr;
        // Binding scopes at or below this depth belong to the fragment. A
        // context continuation retains their lifetime but must resolve names
        // only in the invoking lexical environment.
        size_t outerBindingDepth = 0;
    };

    struct IteratorRecipeStep {
        IteratorOp op = IteratorOp::None;
        std::unique_ptr<Expr> argument;
        LocalId argumentLocal;
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
        bool materialized = false;
        bool materializedOwnsSource = false;
        LocalId materializedSource;
        LocalId materializedIndex;
        LocalId materializedLimit;
    };

    struct MaterializedIteratorStep {
        IteratorOp op = IteratorOp::None;
        LocalId argument;
        TypeRef inputType;
        TypeRef outputType;
    };

    struct MaterializedIteratorRecipe {
        IteratorMode mode = IteratorMode::Copy;
        LocalId source;
        TypeRef sourceType;
        LocalId index;
        LocalId limit;
        TypeRef itemType;
        bool ownsSource = false;
        std::vector<MaterializedIteratorStep> steps;
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
                         std::nullopt,
                     bool inferTypeCleanup = true);
    CleanupId addCleanup(LocalId local, const TypeRef& type,
                         luna::ownership::CleanupAction action,
                         CleanupKind kind = CleanupKind::Value);

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
    std::optional<OpenBlock> normalizeControlFlowExpression(
        std::unique_ptr<Expr>& expression, OpenBlock current,
        RegionId region, ScopeId scope, bool discardUnitResult);
    std::optional<OpenBlock> lowerBlockExpression(
        std::unique_ptr<BlockExpr> expression, OpenBlock current,
        RegionId region, ScopeId scope, std::unique_ptr<Expr>& replacement);
    std::optional<OpenBlock> lowerIfExpression(
        std::unique_ptr<IfExpr> expression, OpenBlock current,
        RegionId region, ScopeId scope, std::unique_ptr<Expr>& replacement);
    std::optional<OpenBlock> lowerTryExpression(
        std::unique_ptr<TryExpr> expression, OpenBlock current,
        RegionId region, ScopeId scope, std::unique_ptr<Expr>& replacement);
    std::optional<OpenBlock> lowerShortCircuitExpression(
        std::unique_ptr<BinaryExpr> expression, OpenBlock current,
        RegionId region, ScopeId scope, std::unique_ptr<Expr>& replacement);
    std::optional<OpenBlock> lowerRecordAllocation(
        std::unique_ptr<RecordLiteralExpr> expression, OpenBlock current,
        RegionId region, ScopeId scope, std::unique_ptr<Expr>& replacement);
    std::optional<OpenBlock> lowerHeapAllocation(
        std::unique_ptr<HeapAllocExpr> expression, OpenBlock current,
        RegionId region, ScopeId scope, std::unique_ptr<Expr>& replacement);
    std::optional<OpenBlock> lowerAllocationElements(
        const SourceLocation& location, const TypeRef& resultType,
        const TypeRef& allocatedType, HeapStorageKind storage,
        std::vector<InitAllocationExpr::Element> elements,
        OpenBlock current, RegionId region, ScopeId scope,
        std::unique_ptr<Expr>& replacement);
    std::optional<OpenBlock> normalizeOrderedOperands(
        const std::vector<std::unique_ptr<Expr>*>& operands,
        OpenBlock current, RegionId region, ScopeId scope);
    bool containsIteratorTerminal(const Expr* expression) const;
    bool containsPendingControlFlow(const Expr* expression) const;
    bool containsPotentialEarlyExit(const Expr* expression) const;
    bool hoistOrderedOperand(std::unique_ptr<Expr>& expression,
                             OpenBlock& current, ScopeId scope,
                             bool allowLinear);
    std::optional<OpenBlock> lowerIteratorTerminal(
        std::unique_ptr<CallExpr> terminal, OpenBlock current,
        RegionId region, ScopeId scope, bool discardUnitResult,
        std::unique_ptr<Expr>& replacement);
    std::optional<OpenBlock> lowerMatch(
        std::unique_ptr<MatchStmt> statement,
        OpenBlock current, RegionId region, ScopeId scope);
    std::optional<OpenBlock> lowerApply(
        std::unique_ptr<ApplyStmt> statement,
        OpenBlock current, RegionId region, ScopeId scope);
    std::optional<OpenBlock> lowerSlotInvoke(
        std::unique_ptr<SlotInvokeStmt> statement,
        OpenBlock current, RegionId region, ScopeId scope);
    std::optional<OpenBlock> lowerDynamicSlotInvoke(
        std::unique_ptr<SlotInvokeStmt> statement, OpenBlock current,
        RegionId region, ScopeId scope,
        std::vector<DeclarationRef> candidates);
    std::optional<OpenBlock> lowerResume(
        std::unique_ptr<ResumeStmt> statement,
        OpenBlock current, RegionId region, ScopeId scope);
    const FragmentDecl* resolveFragment(
        const DeclarationRef& reference) const;

    bool bindExpr(Expr* expression);
    bool parseIteratorRecipe(
        std::unique_ptr<Expr> expression,
        IteratorRecipePlan& plan,
        const SourceLocation& location);
    bool validateIteratorRecipe(
        IteratorRecipePlan& plan, const TypeRef& expectedItem,
        const SourceLocation& location,
        bool allowAffineItems = false);
    bool bindIteratorRecipe(IteratorRecipePlan& plan);
    bool materializeIteratorRecipe(
        std::unique_ptr<LetStmt> declaration, OpenBlock current,
        ScopeId scope);
    LocalId lookupLocal(const std::string& name) const;
    const MaterializedIteratorRecipe* lookupMaterializedIterator(
        const std::string& name) const;
    void pushBindings();
    void popBindings();
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
    std::vector<std::string> mCaptureNames;
    TypeRef mCaptureClosureType;
    std::string mCaptureEnvParamName;
    LocalId mCaptureEnvLocal;
    std::vector<std::unordered_map<std::string, LocalId>> mBindings;
    std::vector<std::unordered_map<std::string, MaterializedIteratorRecipe>>
        mMaterializedIterators;
    std::vector<std::unordered_map<std::string, DeclarationRef>>
        mSlotDefaults;
    std::vector<std::unordered_map<std::string, DeclarationRef>>
        mStaticApplyScopes;
    // Dynamic apply scopes: slot name -> candidate fragment DeclarationRefs.
    // Populated by lowerApply when the apply carries a finite candidate set;
    // consumed by lowerSlotInvoke to resolve dynamic dispatch candidates.
    std::vector<std::unordered_map<std::string, std::vector<DeclarationRef>>>
        mDynamicApplyScopes;
    // Active only while a cloned static fragment body is being composed into
    // its invocation graph. A source `return` or `abort` exits that fragment,
    // not the enclosing Luna function.
    std::vector<FragmentContext> mFragmentContexts;
    std::unordered_map<uint32_t, CleanupId> mCleanupByLocal;
    // Cleanup-bearing synthetic operands are active from their generated let
    // until the parent expression consumes them. Structured early exits built
    // in that interval must carry these obligations even though the frontend
    // could not name compiler-generated locals.
    std::vector<CleanupId> mActiveExpressionCleanups;
    // Recipe-state names of guarded consuming for-loops currently being
    // lowered. The frontend records each as a cleanup obligation on return/
    // abort edges, but the obligation has no canonical local: every element is
    // moved out by a canonical MoveExpr whose cleanup is the per-element
    // guarded tail already attached to the exit edge through
    // mActiveExpressionCleanups. Such obligations are skipped in
    // lowerCleanupObligations.
    std::unordered_set<std::string> mGuardedConsumingRecipeNames;
    std::vector<std::string> mErrors;
    bool mBindingIteratorRecipe = false;
    uint64_t mTerminalCounter = 0;
    uint64_t mExpressionCounter = 0;
};

} // namespace moon
