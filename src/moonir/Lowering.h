#pragma once

#include "diagnostics/Diagnostic.h"

#include "MoonIR.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct Program;
class SymbolTable;
struct ASTNode;
struct TypeAST;
struct Expr;
struct Stmt;
struct BlockStmt;
struct Decl;
struct FunctionDecl;
struct Param;
enum class RetentionKind;

namespace moon {

class LunaLowerer {
public:
    std::unique_ptr<Module> lower(const Program& program,
                                  const SymbolTable& symbols,
                                  bool reserveKernelRuntime = false);
    const std::vector<diagnostic::Diagnostic>& errors() const { return mErrors; }

private:
    SourceLocation locationOf(const ASTNode* node) const;
    TypePtr lowerType(const TypeAST* type) const;
    TypeRef typeRef(const TypePtr& type) const;
    TypeRefVec typeRefs(const TypeVec& types) const;
    TypePtr parameterType(const ::Param& parameter) const;
    Param lowerParam(const ::Param& parameter) const;
    Operator lowerOperator(int tokenKind, const ASTNode* node);
    std::unique_ptr<moon::Expr> lowerExpr(const ::Expr* expression);
    std::unique_ptr<moon::Stmt> lowerStmt(const ::Stmt* statement);
    std::unique_ptr<moon::BlockStmt> lowerBlock(const ::BlockStmt* block);
    std::unique_ptr<moon::Decl> lowerDecl(const ::Decl* declaration);
    std::unique_ptr<moon::FunctionDecl> lowerFunction(
        const ::FunctionDecl* function);
    void lowerCommonDeclaration(const ::Decl* source, moon::Decl& target);
    Retention lowerRetention(RetentionKind retention) const;
    TypePtr inferredExprType(const ::Expr* expression) const;
    void addDeclarationRecord(const moon::Decl& declaration,
                              DeclarationKind kind, TypePtr type);
    void deferDeclarationRef(DeclarationRef& target,
                             std::string lookup,
                             const ASTNode* source,
                             std::string context,
                             bool lookupById = false,
                             luna::identity::SymbolId expectedSymbol = {},
                             luna::identity::ContractId expectedContract = {});
    void resolveDeclarationReferences();
    void buildModuleInterfaces();
    void error(const ASTNode* node, const std::string& message);

    struct PendingDeclarationRef {
        DeclarationRef* target = nullptr;
        std::string lookup;
        const ASTNode* source = nullptr;
        std::string context;
        bool lookupById = false;
        luna::identity::SymbolId expectedSymbol;
        luna::identity::ContractId expectedContract;
    };

    struct CompileTimeDeclarationBinding {
        std::string declarationId;
        std::string symbolName;
        luna::identity::SymbolId symbolId;
        luna::identity::ContractId contractId;
        TypePtr type;
    };

    const Program* mProgram = nullptr;
    const SymbolTable* mSymbols = nullptr;
    Module* mModule = nullptr;
    bool mReserveKernelRuntime = false;
    std::unordered_set<std::string> mRequiredKernelSymbols;
    std::vector<PendingDeclarationRef> mPendingDeclarationRefs;
    std::unordered_map<std::string, CompileTimeDeclarationBinding>
        mCompileTimeDeclarationBindings;
    std::vector<diagnostic::Diagnostic> mErrors;
};

} // namespace moon
