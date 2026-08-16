#pragma once

#include "diagnostics/Diagnostic.h"

#include "MoonIR.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace moon {

class Verifier {
public:
    bool verify(const Module& module);
    bool verify(const ControlFlowGraph& graph, const Module& module);
    const std::vector<diagnostic::Diagnostic>& errors() const { return mErrors; }

private:
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
    void error(const SourceLocation& location, const std::string& message);

    std::vector<diagnostic::Diagnostic> mErrors;
    std::unordered_set<std::string> mVerifiedTypeIds;
    std::unordered_set<std::string> mActiveTypeIds;
    bool mAllowTypeParameters = false;
};

} // namespace moon
