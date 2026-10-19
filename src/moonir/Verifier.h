#pragma once

#include "diagnostics/Diagnostic.h"

#include "MoonIR.h"

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace moon {

enum class VerificationPolicy {
    Published,
    // Native library preparation only. A caller must still rederive the
    // sealed Ref/unit source facts; this never authorizes publication.
    NativeRefUnitPreparation,
};

class Verifier {
public:
    explicit Verifier(
        VerificationPolicy policy = VerificationPolicy::Published)
        : mPolicy(policy) {}
    bool verify(const Module& module);
    bool verify(const ControlFlowGraph& graph, const Module& module);
    const std::vector<diagnostic::Diagnostic>& errors() const { return mErrors; }
    // Available after a successful CFG verification for Jump and RuntimeSlot
    // blocks. These rows remain active after the block's operations and Slot
    // operand, before an outgoing edge performs its own cleanups.
    const std::vector<CleanupId>* activeCleanupsBeforeEdge(BlockId block) const;

private:
    bool nativeRefUnitCandidateShape(
        const FunctionDecl& function, const Module& module) const;
    void verifyDeclaration(const Decl& declaration, const Module& module);
    void verifyFunction(const FunctionDecl& function, const Module& module,
                        bool inheritsTypeParameters = false);
    void verifyBlock(const BlockStmt* block, const Module& module,
                     const std::string& owner);
    void verifyStmt(const Stmt* stmt, const Module& module,
                    const std::string& owner);
    void verifyExpr(const Expr* expr, const Module& module,
                    const std::string& owner);
    void verifyCleanupAction(
        luna::ownership::CleanupAction action,
        const luna::types::TypeId& typeId,
        const SourceLocation& location,
        const std::string& context,
        const Module& module);
    const DeclarationRecord* verifyDeclarationRef(
        const DeclarationRef& reference,
        const SourceLocation& location,
        const std::string& context,
        const Module& module,
        DeclarationKind expectedKind);
    void verifyType(const TypeRef& type, const SourceLocation& location,
                    const std::string& context, const Module& module,
                    bool allowTypeParameter = false);
    // CFG structural invariants: canonical index tables, root anchors, and
    // parent-chain acyclicity. Split out from verify(CFG) for readability.
    void verifyCanonicalTables(const ControlFlowGraph& graph);
    // Region structure and block ownership verification.
    void verifyRegions(const ControlFlowGraph& graph, const Module& module);
    void verifyControlFlowBlocks(
        const ControlFlowGraph& graph, const Module& module,
        const std::unordered_set<uint32_t>& guardedCursorIds,
        std::vector<std::vector<BlockId>>& successors);
    // Path-sensitive cleanup and move-only local state over canonical CFG
    // edges. Kept separate from structural block/terminator verification.
    void verifyOwnershipDataflow(
        const ControlFlowGraph& graph, const Module& module, bool hasEntry);
    void error(const SourceLocation& location, const std::string& message);

    std::vector<diagnostic::Diagnostic> mErrors;
    std::vector<std::optional<std::vector<CleanupId>>> mActiveCleanupsBeforeEdge;
    std::unordered_set<std::string> mVerifiedTypeIds;
    std::unordered_set<std::string> mActiveTypeIds;
    bool mAllowTypeParameters = false;
    VerificationPolicy mPolicy = VerificationPolicy::Published;
};

} // namespace moon
