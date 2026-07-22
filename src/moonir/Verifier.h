#pragma once

#include "MoonIR.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace moon {

class Verifier {
public:
    bool verify(const Module& module);
    const std::vector<std::string>& errors() const { return mErrors; }

private:
    void verifyDeclaration(const Decl& declaration, const Module& module);
    void verifyFunction(const FunctionDecl& function, const Module& module);
    void verifyBlock(const BlockStmt* block, const Module& module,
                     const std::string& owner);
    void verifyStmt(const Stmt* stmt, const Module& module,
                    const std::string& owner);
    void verifyExpr(const Expr* expr, const Module& module,
                    const std::string& owner);
    void verifyType(const TypePtr& type, const SourceLocation& location,
                    const std::string& context, bool allowTypeParameter = false);
    void error(const SourceLocation& location, const std::string& message);

    std::vector<std::string> mErrors;
    std::unordered_set<std::string> mVerifiedTypeIds;
};

} // namespace moon
