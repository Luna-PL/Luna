#pragma once

#include "AST.h"
#include "../lexer/Token.h"
#include "diagnostics/Diagnostic.h"
#include <memory>
#include <string>
#include <vector>

class Parser {
public:
    explicit Parser(std::vector<Token> tokens, std::string sourceName = "<input>",
                    std::string source = "");

    std::unique_ptr<Program> parse();
    const std::vector<diagnostic::Diagnostic>& errors() const { return mErrors; }

private:
    std::unique_ptr<Decl> parseDeclaration();
    bool parsePackageHeader(Program* program);
    bool parseModuleHeader(Program* program);
    bool parseUsingHeader(Program* program);
    bool parsePackageId(std::string& result);
    bool parseModulePath(std::string& result);
    bool parseQualifiedName(std::string& result);
    void parseQualifiedNameTail(std::string& result);
    std::unique_ptr<FunctionDecl> parseFunctionDecl(bool isTraitMethod = false,
                                                    bool isExtern = false,
                                                    std::string abi = "",
                                                    bool isConstexpr = false,
                                                    bool isKernel = false);
    std::unique_ptr<StructDecl> parseStructDecl();
    std::unique_ptr<EnumDecl> parseEnumDecl();
    std::unique_ptr<FragmentDecl> parseFragmentDecl(FragmentKind kind);
    std::unique_ptr<TraitDecl> parseTraitDecl();
    std::unique_ptr<ImplDecl> parseImplDecl();
    std::unique_ptr<MetaDecl> parseMetaDecl();
    std::unique_ptr<ConstraintDecl> parseConstraintDecl();
    std::unique_ptr<BlockStmt> parseBlock();
    std::unique_ptr<Stmt> parseStatement();
    std::unique_ptr<Stmt> parseLetStmt(
        luna::ownership::Usage usage = luna::ownership::Usage::Copy,
        bool isConst = false, bool hasExplicitUsage = false);
    std::unique_ptr<Stmt> parseReturnStmt();
    std::unique_ptr<Stmt> parseIfStmt();
    std::unique_ptr<Stmt> parseMatchStmt();
    std::unique_ptr<Stmt> parseWhileStmt();
    std::unique_ptr<Stmt> parseForStmt();
    std::unique_ptr<Stmt> parseFreeStmt();
    std::unique_ptr<Stmt> parseSlotStmt(bool isDynamic = false);
    std::unique_ptr<Stmt> parseResumeStmt();
    std::unique_ptr<Stmt> parseAbortStmt();
    std::unique_ptr<Stmt> parseAwaitStmt();
    std::unique_ptr<Stmt> parseApplyStmt(bool isDynamic = false);
    std::unique_ptr<Stmt> parseNamedSlotInvokeStmt();
    std::unique_ptr<Stmt> parseExprStmt();

    std::unique_ptr<Expr> parseExpr();
    std::unique_ptr<Expr> parseAssignment();
    std::unique_ptr<Expr> parseOr();
    std::unique_ptr<Expr> parseAnd();
    std::unique_ptr<Expr> parseBitOr();
    std::unique_ptr<Expr> parseBitXor();
    std::unique_ptr<Expr> parseBitAnd();
    std::unique_ptr<Expr> parseEquality();
    std::unique_ptr<Expr> parseComparison();
    std::unique_ptr<Expr> parseShift();
    std::unique_ptr<Expr> parseAddSub();
    std::unique_ptr<Expr> parseMulDiv();
    std::unique_ptr<Expr> parseUnary();
    std::unique_ptr<Expr> parsePostfix();
    std::unique_ptr<Expr> parsePrimary();
    std::unique_ptr<Expr> parseExprBeforeBlock();
    std::unique_ptr<RecordLiteralExpr> parseRecordLiteral(
        std::unique_ptr<TypeAST> targetType = nullptr);
    std::unique_ptr<Expr> parseIfExpr();
    std::unique_ptr<Expr> parseLambda();
    std::unique_ptr<Expr> parseLaunchExpr();
    std::unique_ptr<SelectExpr> parseSelectExpr(bool isDynamic,
                                                bool selectAlreadyConsumed = true);

    std::unique_ptr<TypeAST> parseType();
    std::unique_ptr<RecordTypeAST> parseRecordType();
    std::unique_ptr<TypeAST> parseFunctionType();
    std::vector<Param> parseParams();
    std::vector<std::unique_ptr<Expr>> parseArgs();
    std::vector<std::string> parseTypeParamList(
        std::vector<WhereClause>* constrainedParameters = nullptr);
    std::vector<WhereClause> parseWhereClause();
    TraitRef parseTraitRef(const Token& nameToken);
    Decl::MetadataAttachment parseMetadataAttachment(RetentionKind retention);

    const Token& peek() const;
    const Token& peekAhead(int n) const;
    Token advance();
    bool check(TokenKind kind) const;
    bool match(TokenKind kind);
    Token consume(TokenKind kind, const std::string& errorMsg);
    bool isAtEnd() const;
    bool isNamedSlotInvocationStart() const;
    void addError(const std::string& msg, const std::string& hint = "");
    void synchronizeDeclaration();
    void synchronizeStatement();
    std::string sourceLineAt(int line) const;

    std::vector<Token> mTokens;
    int mPos = 0;
    int mNestingDepth = 0;
    std::string mSourceName;
    std::string mSource;
    std::vector<diagnostic::Diagnostic> mErrors;
    bool mStopBeforeBlockBrace = false;
    luna::ownership::Usage mUsageDefault = luna::ownership::Usage::Copy;
};
