#include "Parser.h"
#include "../diagnostics/Diagnostic.h"

Parser::Parser(std::vector<Token> tokens, std::string sourceName, std::string source)
    : mTokens(std::move(tokens)), mSourceName(std::move(sourceName)), mSource(std::move(source)) {}

std::unique_ptr<Program> Parser::parse() {
    auto program = std::make_unique<Program>();
    if (check(TokenKind::Package)) {
        parsePackageHeader(program.get());
        program->isPackage = true;
    }
    if (check(TokenKind::Module)) parseModuleHeader(program.get());
    while (check(TokenKind::Using)) parseUsingHeader(program.get());
    while (!isAtEnd()) {
        const int declarationStart = mPos;
        if (auto decl = parseDeclaration()) {
            decl->modulePath = program->modulePath;
            program->declarations.push_back(std::move(decl));
        } else {
            // A malformed top-level declaration must not hide every later
            // error in the file. Advance to a statement terminator or the
            // next declaration introducer, then continue collecting errors.
            synchronizeDeclaration();
            if (mPos == declarationStart && !isAtEnd()) advance();
        }
    }
    return program;
}

bool Parser::parsePackageHeader(Program* program) {
    consume(TokenKind::Package, "Expected 'package'");
    if (!parsePackageId(program->packageName)) {
        addError("expected a package name, found " + diagnostic::quotedToken(peek().lexeme),
                 "write `package com.example.my_package;`");
        return false;
    }
    consume(TokenKind::SemiColon, "Expected ';' after package name");
    return true;
}

bool Parser::parseModuleHeader(Program* program) {
    const Token start = consume(TokenKind::Module, "Expected 'module'");
    if (!parseModulePath(program->modulePath)) {
        addError("expected a module path, found " + diagnostic::quotedToken(peek().lexeme),
                 "write `module io;` or `module io::format;`");
        return false;
    }
    consume(TokenKind::SemiColon, "Expected ';' after module path");
    (void)start;
    return true;
}

bool Parser::parseUsingHeader(Program* program) {
    const Token start = consume(TokenKind::Using, "Expected 'using'");
    Program::PackageUse use;
    use.sourcePath = mSourceName;
    use.line = start.line;
    use.col = start.col;
    if (!parsePackageId(use.packageId)) {
        addError("expected a Package ID after `using`",
                 "write `using org.example.library as library;`");
        return false;
    }
    if (!match(TokenKind::As)) {
        addError("package using declaration requires a local alias",
                 "append `as name`, for example `using org.luna.std as std;`");
        return false;
    }
    if (!match(TokenKind::Identifier)) {
        addError("expected an identifier after `as` in using declaration");
        return false;
    }
    use.alias = mTokens[mPos - 1].lexeme;
    consume(TokenKind::SemiColon, "Expected ';' after using declaration");
    program->packageUses.push_back(std::move(use));
    return true;
}

bool Parser::parsePackageId(std::string& result) {
    if (!match(TokenKind::Identifier)) return false;
    result = mTokens[mPos - 1].lexeme;
    while (match(TokenKind::Dot)) {
        if (!match(TokenKind::Identifier)) {
            addError("expected a Package ID component after '.'");
            return false;
        }
        result += "." + mTokens[mPos - 1].lexeme;
    }
    return true;
}

bool Parser::parseModulePath(std::string& result) {
    if (!match(TokenKind::Identifier)) return false;
    result = mTokens[mPos - 1].lexeme;
    while (match(TokenKind::ColonColon)) {
        if (!match(TokenKind::Identifier)) {
            addError("expected a module component after '::'");
            return false;
        }
        result += "::" + mTokens[mPos - 1].lexeme;
    }
    return true;
}

bool Parser::parseQualifiedName(std::string& result) {
    if (!match(TokenKind::Identifier)) return false;
    result = mTokens[mPos - 1].lexeme;
    parseQualifiedNameTail(result);
    return true;
}

void Parser::parseQualifiedNameTail(std::string& result) {
    while (check(TokenKind::ColonColon) &&
           peekAhead(1).kind == TokenKind::Identifier) {
        advance();
        advance();
        result += "::" + mTokens[mPos - 1].lexeme;
    }
}

std::unique_ptr<Decl> Parser::parseDeclaration() {
    const Token start = peek();
    bool isExported = false;
    bool isConstexpr = false;
    bool isExtern = false;
    bool isKernel = false;
    bool isNominal = false;
    RetentionKind retention = RetentionKind::CompileTime;
    std::vector<Decl::MetadataAttachment> metadata;
    bool consumedModifier = true;
    while (consumedModifier) {
        consumedModifier = false;
        if (match(TokenKind::Export)) { isExported = true; consumedModifier = true; }
        else if (match(TokenKind::Constexpr)) { isConstexpr = true; consumedModifier = true; }
        else if (match(TokenKind::Extern)) { isExtern = true; consumedModifier = true; }
        else if (match(TokenKind::Kernel)) { isKernel = true; consumedModifier = true; }
        else if (match(TokenKind::Nominal)) { isNominal = true; consumedModifier = true; }
        else if (match(TokenKind::Runtime)) {
            consumedModifier = true;
            if (check(TokenKind::At))
                metadata.push_back(parseMetadataAttachment(RetentionKind::Runtime));
            else retention = RetentionKind::Runtime;
        } else if (match(TokenKind::Dynamic)) {
            consumedModifier = true;
            if (check(TokenKind::At))
                metadata.push_back(parseMetadataAttachment(RetentionKind::Dynamic));
            else retention = RetentionKind::Dynamic;
        } else if (check(TokenKind::At)) {
            consumedModifier = true;
            metadata.push_back(parseMetadataAttachment(RetentionKind::CompileTime));
        }
    }
    std::string abi;
    if (check(TokenKind::StringLiteral) && (isExtern || isExported))
        abi = advance().lexeme;
    if (isExtern && abi.empty()) abi = "C";
    std::unique_ptr<Decl> decl;
    if (match(TokenKind::Fn))
        decl = parseFunctionDecl(false, isExtern, abi, isConstexpr, isKernel);
    else if (match(TokenKind::Interceptor)) decl = parseFragmentDecl(FragmentKind::Interceptor);
    else if (match(TokenKind::Context)) decl = parseFragmentDecl(FragmentKind::Context);
    else if (match(TokenKind::Fragment)) {
        addError("`fragment` is ambiguous and is no longer accepted",
                 "use `interceptor name { ... }`, `context name { ... }`, or `context many name { ... }`");
        return nullptr;
    }
    else if (match(TokenKind::Struct)) decl = parseStructDecl();
    else if (match(TokenKind::Enum)) decl = parseEnumDecl();
    else if (match(TokenKind::Trait)) decl = parseTraitDecl();
    else if (match(TokenKind::Impl)) decl = parseImplDecl();
    else if (match(TokenKind::Meta)) decl = parseMetaDecl();
    else if (match(TokenKind::Constraint)) decl = parseConstraintDecl();
    else {
        if (isExported || isExtern || isConstexpr || isKernel || isNominal) {
            addError("expected a declaration after `export`, found " +
                     diagnostic::quotedToken(peek().lexeme),
                     "only `fn`, `interceptor`, `context`, `struct`, `enum`, "
                     "`trait`, `meta`, and `constraint` can be exported");
        } else {
            addError("expected a declaration, found " + diagnostic::quotedToken(peek().lexeme),
                     "start a declaration with `fn`, `interceptor`, `context`, "
                     "`struct`, `enum`, `trait`, `impl`, `meta`, or `constraint`");
        }
        advance(); // skip unexpected token
        return nullptr;
    }
    if (decl) {
        if (auto* structure = dynamic_cast<StructDecl*>(decl.get())) {
            structure->isNominal = isNominal;
        } else if (auto* enumeration = dynamic_cast<EnumDecl*>(decl.get())) {
            enumeration->isNominal = isNominal;
        } else if (isNominal) {
            addError("`nominal` can only modify a struct or enum declaration",
                     "remove `nominal` or apply it to `struct`/`enum`");
        }
        // A runtime-visible attachment needs a descriptor to live on. Keep
        // that implication local to the attached declaration so one schema
        // does not make every use of it pay a runtime cost.
        for (const auto& attachment : metadata) {
            if (attachment.retention == RetentionKind::Dynamic)
                retention = RetentionKind::Dynamic;
            else if (attachment.retention == RetentionKind::Runtime &&
                     retention == RetentionKind::CompileTime)
                retention = RetentionKind::Runtime;
        }
        decl->isExported = isExported;
        decl->retention = retention;
        decl->metadata = std::move(metadata);
        decl->sourcePath = mSourceName;
        decl->line = start.line;
        decl->col = start.col;
    }
    return decl;
}

Decl::MetadataAttachment Parser::parseMetadataAttachment(RetentionKind retention) {
    Decl::MetadataAttachment attachment;
    attachment.retention = retention;
    consume(TokenKind::At, "Expected '@' before metadata attachment");
    if (!match(TokenKind::Identifier)) {
        addError("expected a metadata schema name after `@`",
                 "declare it with `meta name { ... }` and attach it as `@name(...)`");
        return attachment;
    }
    attachment.schemaName = mTokens[mPos - 1].lexeme;
    consume(TokenKind::LParen, "Expected '(' after metadata schema name");
    if (!check(TokenKind::RParen)) attachment.arguments = parseArgs();
    consume(TokenKind::RParen, "Expected ')' after metadata arguments");
    return attachment;
}

std::unique_ptr<MetaDecl> Parser::parseMetaDecl() {
    auto declaration = std::make_unique<MetaDecl>();
    if (!match(TokenKind::Identifier)) {
        addError("expected a metadata type name after `meta`");
        return nullptr;
    }
    declaration->name = mTokens[mPos - 1].lexeme;
    consume(TokenKind::LBrace, "Expected '{' for metadata schema body");
    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        if (!match(TokenKind::Identifier)) {
            addError("expected a metadata field name");
            synchronizeStatement();
            continue;
        }
        MetaDecl::Field field;
        field.name = mTokens[mPos - 1].lexeme;
        consume(TokenKind::Colon, "Expected ':' after metadata field name");
        field.type = parseType();
        consume(TokenKind::SemiColon, "Expected ';' after metadata field type");
        declaration->fields.push_back(std::move(field));
    }
    consume(TokenKind::RBrace, "Expected '}' after metadata schema body");
    return declaration;
}

std::unique_ptr<ConstraintDecl> Parser::parseConstraintDecl() {
    auto declaration = std::make_unique<ConstraintDecl>();
    if (!match(TokenKind::Identifier)) {
        addError("expected a constraint name after `constraint`");
        return nullptr;
    }
    declaration->name = mTokens[mPos - 1].lexeme;
    declaration->typeParams = parseTypeParamList();
    if (declaration->typeParams.empty())
        addError("constraint '" + declaration->name +
                 "' requires at least one type parameter");
    consume(TokenKind::Eq, "Expected '=' after constraint parameters");
    declaration->predicate = parseExpr();
    consume(TokenKind::SemiColon, "Expected ';' after constraint predicate");
    return declaration;
}

std::unique_ptr<FragmentDecl> Parser::parseFragmentDecl(FragmentKind kind) {
    auto decl = std::make_unique<FragmentDecl>();
    decl->kind = kind;
    if (kind == FragmentKind::Context && match(TokenKind::Many))
        decl->cardinality = FragmentCardinality::Many;
    else if (kind == FragmentKind::Interceptor && check(TokenKind::Many))
        addError("interceptor is always single-pass and cannot be `many`");
    if (!match(TokenKind::Identifier)) {
        addError("expected a fragment name, found " + diagnostic::quotedToken(peek().lexeme),
                 "write `fragment name { ... }` or `fragment name(value) { ... }`");
        return nullptr;
    }
    decl->name = mTokens[mPos - 1].lexeme;
    if (match(TokenKind::LParen)) {
        decl->params = parseParams();
        consume(TokenKind::RParen, "Expected ')' after fragment parameters");
    }
    decl->body = parseBlock();
    return decl;
}

// ─── Function ──────────────────────────────────────────────────────

std::unique_ptr<FunctionDecl> Parser::parseFunctionDecl(bool isTraitMethod,
                                                         bool isExtern,
                                                         std::string abi,
                                                         bool isConstexpr,
                                                         bool isKernel) {
    auto decl = std::make_unique<FunctionDecl>();
    decl->isExtern = isExtern;
    decl->isConstexpr = isConstexpr;
    decl->isKernel = isKernel;
    decl->abi = std::move(abi);

    if (match(TokenKind::Identifier)) {
        decl->name = mTokens[mPos - 1].lexeme;
    } else {
        // Trait methods may have operator-style names; for simplicity just require IDENT
        addError("expected a function name, found " + diagnostic::quotedToken(peek().lexeme));
        return nullptr;
    }

    decl->typeParams = parseTypeParamList();
    consume(TokenKind::LParen, "Expected '(' after function name");
    decl->params = parseParams();
    consume(TokenKind::RParen, "Expected ')' after parameters");

    if (match(TokenKind::Arrow)) {
        decl->returnType = parseType();
        decl->returnsLinear = dynamic_cast<LinearTypeAST*>(decl->returnType.get()) != nullptr;
        decl->returnUsage = decl->returnsLinear
            ? luna::ownership::Usage::Linear
            : (dynamic_cast<AffineTypeAST*>(decl->returnType.get())
                ? luna::ownership::Usage::Affine
                : luna::ownership::Usage::Copy);
    }

    if (isExtern && match(TokenKind::As)) {
        if (match(TokenKind::StringLiteral)) {
            decl->linkName = mTokens[mPos - 1].lexeme;
        } else {
            addError("Expected string symbol name after 'as'");
        }
    }

    decl->whereClauses = parseWhereClause();

    if (isTraitMethod) {
        consume(TokenKind::SemiColon, "Expected ';' after trait method signature");
    } else if (isExtern) {
        consume(TokenKind::SemiColon, "Expected ';' after extern function declaration");
    } else {
        decl->body = parseBlock();
    }

    return decl;
}

// ─── Struct ────────────────────────────────────────────────────────

std::unique_ptr<StructDecl> Parser::parseStructDecl() {
    auto decl = std::make_unique<StructDecl>();
    if (!match(TokenKind::Identifier)) {
        addError("Expected struct name");
        return nullptr;
    }
    decl->name = mTokens[mPos - 1].lexeme;
    decl->typeParams = parseTypeParamList();
    consume(TokenKind::LBrace, "Expected '{' for struct body");

    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        if (!match(TokenKind::Identifier)) {
            addError("Expected field name in struct");
            break;
        }
        Param field;
        field.name = mTokens[mPos - 1].lexeme;
        consume(TokenKind::Colon, "Expected ':' after field name");
        field.type = parseType();
        consume(TokenKind::SemiColon, "Expected ';' after field type");
        decl->fields.push_back(std::move(field));
    }
    consume(TokenKind::RBrace, "Expected '}' after struct body");
    return decl;
}

std::unique_ptr<EnumDecl> Parser::parseEnumDecl() {
    auto decl = std::make_unique<EnumDecl>();
    if (!match(TokenKind::Identifier)) {
        addError("Expected enum name");
        return nullptr;
    }
    decl->name = mTokens[mPos - 1].lexeme;
    decl->typeParams = parseTypeParamList();
    consume(TokenKind::LBrace, "Expected '{' for enum body");

    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        if (!match(TokenKind::Identifier)) {
            addError("Expected enum variant name");
            advance();
            continue;
        }
        EnumDecl::Variant variant;
        variant.name = mTokens[mPos - 1].lexeme;
        if (match(TokenKind::LParen)) {
            if (!check(TokenKind::RParen)) {
                do {
                    variant.fields.push_back(parseType());
                } while (match(TokenKind::Comma));
            }
            consume(TokenKind::RParen, "Expected ')' after enum variant fields");
        }
        // Both semicolon and comma are accepted between variants. The
        // semicolon form matches struct declarations and is unambiguous.
        if (!match(TokenKind::SemiColon)) match(TokenKind::Comma);
        decl->variants.push_back(std::move(variant));
    }
    consume(TokenKind::RBrace, "Expected '}' after enum body");
    return decl;
}

// ─── Trait ─────────────────────────────────────────────────────────

std::unique_ptr<TraitDecl> Parser::parseTraitDecl() {
    auto decl = std::make_unique<TraitDecl>();
    if (!match(TokenKind::Identifier)) {
        addError("Expected trait name");
        return nullptr;
    }
    decl->name = mTokens[mPos - 1].lexeme;
    decl->typeParams = parseTypeParamList();
    consume(TokenKind::LBrace, "Expected '{' for trait body");

    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        if (match(TokenKind::Fn)) {
            auto method = parseFunctionDecl(true);
            if (method) {
                TraitDecl::MethodSig sig;
                sig.name = method->name;
                sig.returnType = std::move(method->returnType);
                for (auto& p : method->params)
                    sig.params.push_back(std::move(p));
                decl->methods.push_back(std::move(sig));
            }
        } else {
            addError("Expected 'fn' in trait body");
            advance();
        }
    }
    consume(TokenKind::RBrace, "Expected '}' after trait body");
    return decl;
}

// ─── Impl ──────────────────────────────────────────────────────────

std::unique_ptr<ImplDecl> Parser::parseImplDecl() {
    auto decl = std::make_unique<ImplDecl>();
    decl->typeParams = parseTypeParamList();

    if (!match(TokenKind::Identifier)) {
        addError("Expected trait name in impl");
        return nullptr;
    }
    decl->trait = parseTraitRef(mTokens[mPos - 1]);

    if (!match(TokenKind::For)) {
        addError("Expected 'for' after trait name in impl");
        return nullptr;
    }

    decl->targetType = parseType();
    consume(TokenKind::LBrace, "Expected '{' in impl");

    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        if (match(TokenKind::Fn)) {
            auto method = parseFunctionDecl(false);
            if (method) decl->methods.push_back(std::move(method));
        } else {
            addError("Expected 'fn' in impl body");
            advance();
        }
    }
    consume(TokenKind::RBrace, "Expected '}' after impl body");
    return decl;
}

// ─── Block ─────────────────────────────────────────────────────────

std::unique_ptr<BlockStmt> Parser::parseBlock() {
    auto block = std::make_unique<BlockStmt>();
    consume(TokenKind::LBrace, "Expected '{'");
    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        if (auto stmt = parseStatement()) {
            block->stmts.push_back(std::move(stmt));
        }
    }
    consume(TokenKind::RBrace, "Expected '}'");
    return block;
}

// ─── Statements ────────────────────────────────────────────────────

std::unique_ptr<Stmt> Parser::parseStatement() {
    const Token start = peek();
    const auto stamp = [&](std::unique_ptr<Stmt> stmt) {
        if (stmt && stmt->line <= 0) {
            stmt->sourcePath = mSourceName;
            stmt->line = start.line;
            stmt->col = start.col;
        }
        return stmt;
    };
    if (match(TokenKind::Linear)) {
        if (match(TokenKind::Let))
            return stamp(parseLetStmt(luna::ownership::Usage::Linear));
        addError("Expected 'let' after 'linear'");
        return nullptr;
    }
    if (match(TokenKind::Affine)) {
        if (match(TokenKind::Let))
            return stamp(parseLetStmt(luna::ownership::Usage::Affine));
        addError("Expected 'let' after 'affine'");
        return nullptr;
    }
    if (match(TokenKind::Const)) {
        if (match(TokenKind::Let))
            return stamp(parseLetStmt(luna::ownership::Usage::Copy, true));
        addError("expected `let` after `const`",
                 "write `const let name = compile_time_expression;`");
        synchronizeStatement();
        return nullptr;
    }
    if (match(TokenKind::Let))
        return stamp(parseLetStmt(luna::ownership::Usage::Copy, match(TokenKind::Const)));
    if (match(TokenKind::Free)) return stamp(parseFreeStmt());
    if (match(TokenKind::Slot)) return stamp(parseSlotStmt());
    if (match(TokenKind::Resume)) return stamp(parseResumeStmt());
    if (match(TokenKind::Abort)) return stamp(parseAbortStmt());
    if (match(TokenKind::Await)) return stamp(parseAwaitStmt());
    if (match(TokenKind::Apply)) return stamp(parseApplyStmt());
    if (match(TokenKind::Dynamic)) {
        if (match(TokenKind::Slot)) return stamp(parseSlotStmt(true));
        if (match(TokenKind::Apply)) return stamp(parseApplyStmt(true));
        addError("`dynamic` must introduce `slot` or `apply`",
                 "write `dynamic slot name(value: Type);` or `dynamic apply name(fragment, ...) { ... }`");
        synchronizeStatement();
        return nullptr;
    }
    if (match(TokenKind::Return)) return stamp(parseReturnStmt());
    if (match(TokenKind::If)) return stamp(parseIfStmt());
    if (match(TokenKind::While)) return stamp(parseWhileStmt());
    if (match(TokenKind::For)) return stamp(parseForStmt());
    if (match(TokenKind::LBrace)) return stamp(parseBlock());
    if (isNamedSlotInvocationStart()) return stamp(parseNamedSlotInvokeStmt());
    if (check(TokenKind::Identifier) && peekAhead(1).kind == TokenKind::Eq) {
        // Assignment statement (lhs = expr ;) — handled in parseExprStmt via parseExpr
    }
    return stamp(parseExprStmt());
}

std::unique_ptr<Stmt> Parser::parseSlotStmt(bool isDynamic) {
    const Token start = mTokens[mPos - 1]; // `slot` was consumed by parseStatement
    FragmentKind acceptedKind;
    FragmentCardinality acceptedCardinality = FragmentCardinality::Once;
    if (match(TokenKind::Interceptor)) acceptedKind = FragmentKind::Interceptor;
    else if (match(TokenKind::Context)) {
        acceptedKind = FragmentKind::Context;
        if (match(TokenKind::Many)) acceptedCardinality = FragmentCardinality::Many;
    }
    else {
        addError("slot must declare its fragment contract",
                 "write `slot interceptor name { ... }` or `slot context name { ... }`");
        synchronizeStatement();
        return nullptr;
    }
    if (!match(TokenKind::Identifier)) {
        addError("expected a slot name, found " + diagnostic::quotedToken(peek().lexeme),
                 "write `slot name { ... }` or `slot name(value: Type);`");
        synchronizeStatement();
        return nullptr;
    }
    const std::string name = mTokens[mPos - 1].lexeme;
    std::vector<Param> params;
    bool hasInterface = false;
    if (match(TokenKind::LParen)) {
        hasInterface = true;
        params = parseParams();
        consume(TokenKind::RParen, "Expected ')' after slot interface");
    }

    std::string defaultFragment;
    if (match(TokenKind::Default)) {
        if (!match(TokenKind::Identifier)) {
            addError("expected a fragment name after `default`");
            synchronizeStatement();
            return nullptr;
        }
        defaultFragment = mTokens[mPos - 1].lexeme;
    }

    if (check(TokenKind::SemiColon)) {
        advance();
        if (!hasInterface) {
            addError("a separately declared slot requires an explicit interface",
                     "use `slot " + name + "(value: Type);`, or define an implicit slot with a continuation block");
            return nullptr;
        }
        auto stmt = std::make_unique<SlotDeclStmt>();
        stmt->name = name;
        stmt->acceptedKind = acceptedKind;
        stmt->acceptedCardinality = acceptedCardinality;
        stmt->isDynamic = isDynamic;
        stmt->params = std::move(params);
        stmt->defaultFragment = std::move(defaultFragment);
        stmt->sourcePath = mSourceName; stmt->line = start.line; stmt->col = start.col;
        return stmt;
    }

    if (!check(TokenKind::LBrace)) {
        addError("expected `;` or a continuation block after slot declaration");
        synchronizeStatement();
        return nullptr;
    }
    auto stmt = std::make_unique<SlotInvokeStmt>();
    stmt->name = name;
    stmt->acceptedKind = acceptedKind;
    stmt->acceptedCardinality = acceptedCardinality;
    stmt->isDynamic = isDynamic;
    stmt->isImplicitCapture = !hasInterface;
    stmt->interfaceParams = std::move(params);
    stmt->defaultFragment = std::move(defaultFragment);
    stmt->continuation = parseBlock();
    stmt->sourcePath = mSourceName; stmt->line = start.line; stmt->col = start.col;
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseResumeStmt() {
    const Token start = mTokens[mPos - 1]; // `resume` already consumed
    consume(TokenKind::LParen, "Expected '(' after `resume`");
    if (!check(TokenKind::RParen)) {
        addError("`resume` does not accept arguments",
                 "the slot continuation restores its original captured frame; write `resume()`");
        synchronizeStatement();
        return nullptr;
    }
    consume(TokenKind::RParen, "Expected ')' after `resume`");
    consume(TokenKind::SemiColon, "Expected ';' after `resume()`");
    auto stmt = std::make_unique<ResumeStmt>();
    stmt->sourcePath = mSourceName; stmt->line = start.line; stmt->col = start.col;
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseAbortStmt() {
    const Token start = mTokens[mPos - 1];
    consume(TokenKind::LParen, "Expected '(' after `abort`");
    consume(TokenKind::RParen, "Expected ')' after `abort`");
    consume(TokenKind::SemiColon, "Expected ';' after `abort()`");
    auto stmt = std::make_unique<AbortStmt>();
    stmt->sourcePath = mSourceName; stmt->line = start.line; stmt->col = start.col;
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseAwaitStmt() {
    const Token start = mTokens[mPos - 1]; // `await` already consumed
    auto stmt = std::make_unique<AwaitStmt>();
    stmt->event = parseExpr();
    consume(TokenKind::SemiColon, "Expected ';' after await expression");
    stmt->sourcePath = mSourceName; stmt->line = start.line; stmt->col = start.col;
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseApplyStmt(bool isDynamic) {
    const Token start = mTokens[mPos - 1]; // `apply` already consumed
    auto stmt = std::make_unique<ApplyStmt>();
    stmt->isDynamic = isDynamic;
    if (!match(TokenKind::Identifier)) {
        addError("expected a slot name after `apply`");
        synchronizeStatement();
        return nullptr;
    }
    stmt->slotName = mTokens[mPos - 1].lexeme;
    consume(TokenKind::LParen, "Expected '(' after slot name in `apply`");
    if (!parseQualifiedName(stmt->fragmentName)) {
        addError("expected a fragment name in `apply`");
        synchronizeStatement();
        return nullptr;
    }
    while (isDynamic && match(TokenKind::Comma)) {
        std::string alternative;
        if (!parseQualifiedName(alternative)) {
            addError("expected a fragment name after ',' in dynamic apply");
            synchronizeStatement();
            return nullptr;
        }
        stmt->alternativeFragmentNames.push_back(std::move(alternative));
    }
    consume(TokenKind::RParen, "Expected ')' after fragment name in `apply`");
    if (check(TokenKind::LBrace)) stmt->body = parseBlock();
    else consume(TokenKind::SemiColon, "Expected ';' or a block after `apply`");
    stmt->sourcePath = mSourceName; stmt->line = start.line; stmt->col = start.col;
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseNamedSlotInvokeStmt() {
    const Token start = peek();
    auto stmt = std::make_unique<SlotInvokeStmt>();
    stmt->name = advance().lexeme;
    consume(TokenKind::LParen, "Expected '(' after slot name");
    if (!check(TokenKind::RParen)) stmt->args = parseArgs();
    consume(TokenKind::RParen, "Expected ')' after slot arguments");
    stmt->continuation = parseBlock();
    stmt->sourcePath = mSourceName; stmt->line = start.line; stmt->col = start.col;
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseLetStmt(luna::ownership::Usage usage, bool isConst) {
    auto stmt = std::make_unique<LetStmt>();
    stmt->usage = usage;
    stmt->hasExplicitUsage = usage != luna::ownership::Usage::Copy;
    stmt->isLinear = usage == luna::ownership::Usage::Linear;
    stmt->isConst = isConst;
    if (match(TokenKind::Linear)) {
        stmt->usage = luna::ownership::Usage::Linear;
        stmt->hasExplicitUsage = true;
        stmt->isLinear = true;
    } else if (match(TokenKind::Affine)) {
        stmt->usage = luna::ownership::Usage::Affine;
        stmt->hasExplicitUsage = true;
    }
    if (!match(TokenKind::Identifier)) {
        addError("Expected variable name after 'let'");
        synchronizeStatement();
        return nullptr;
    }
    stmt->name = mTokens[mPos - 1].lexeme;

    if (match(TokenKind::Colon)) {
        stmt->typeAnnotation = parseType();
    }

    consume(TokenKind::Eq, "Expected '=' in let binding");
    stmt->initializer = parseExpr();
    consume(TokenKind::SemiColon, "Expected ';' after let binding");
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseFreeStmt() {
    auto stmt = std::make_unique<FreeStmt>();
    stmt->operand = parseExpr();
    consume(TokenKind::SemiColon, "Expected ';' after free");
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseReturnStmt() {
    auto stmt = std::make_unique<ReturnStmt>();
    if (!check(TokenKind::SemiColon)) {
        stmt->value = parseExpr();
    }
    consume(TokenKind::SemiColon, "Expected ';' after return");
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseIfStmt() {
    auto stmt = std::make_unique<IfStmt>();
    stmt->cond = parseExpr();
    stmt->thenBlock = parseBlock();
    if (match(TokenKind::Else)) {
        if (check(TokenKind::If)) {
            stmt->elseBranch = parseIfStmt();
        } else {
            stmt->elseBranch = parseBlock();
        }
    }
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseWhileStmt() {
    auto stmt = std::make_unique<WhileStmt>();
    stmt->cond = parseExpr();
    stmt->body = parseBlock();
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseForStmt() {
    auto stmt = std::make_unique<ForStmt>();
    if (!match(TokenKind::Identifier)) {
        addError("Expected loop variable in for");
        return nullptr;
    }
    stmt->varName = mTokens[mPos - 1].lexeme;
    if (!check(TokenKind::Identifier) || mTokens[mPos].lexeme != "in") {
        addError("Expected 'in' in for-loop");
        return nullptr;
    }
    advance(); // consume "in" (it's an identifier, not a keyword)
    stmt->iterable = parseExpr();
    stmt->body = parseBlock();
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseExprStmt() {
    auto stmt = std::make_unique<ExprStmt>();
    stmt->expr = parseExpr();
    consume(TokenKind::SemiColon, "Expected ';' after expression");
    return stmt;
}

// ─── Expressions — precedence climbing ─────────────────────────────

std::unique_ptr<Expr> Parser::parseExpr() {
    return parseAssignment();
}

std::unique_ptr<Expr> Parser::parseAssignment() {
    auto lhs = parseOr();
    if (check(TokenKind::Eq) || check(TokenKind::PlusEq) || check(TokenKind::MinusEq) ||
        check(TokenKind::StarEq) || check(TokenKind::SlashEq) || check(TokenKind::PercentEq) ||
        check(TokenKind::AndEq) || check(TokenKind::OrEq) || check(TokenKind::XorEq) ||
        check(TokenKind::ShiftLeftEq) || check(TokenKind::ShiftRightEq)) {
        auto expr = std::make_unique<AssignExpr>();
        expr->op = advance().kind;
        expr->lhs = std::move(lhs);
        expr->rhs = parseExpr();
        return expr;
    }
    return lhs;
}

std::unique_ptr<Expr> Parser::parseOr() {
    auto lhs = parseAnd();
    while (match(TokenKind::OrOr)) {
        auto expr = std::make_unique<BinaryExpr>();
        expr->lhs = std::move(lhs);
        expr->op = TokenKind::OrOr;
        expr->rhs = parseAnd();
        lhs = std::move(expr);
    }
    return lhs;
}

std::unique_ptr<Expr> Parser::parseAnd() {
    auto lhs = parseBitOr();
    while (match(TokenKind::AndAnd)) {
        auto expr = std::make_unique<BinaryExpr>();
        expr->lhs = std::move(lhs);
        expr->op = TokenKind::AndAnd;
        expr->rhs = parseBitOr();
        lhs = std::move(expr);
    }
    return lhs;
}

std::unique_ptr<Expr> Parser::parseBitOr() {
    auto lhs = parseBitXor();
    while (match(TokenKind::BitOr)) {
        auto expr = std::make_unique<BinaryExpr>();
        expr->lhs = std::move(lhs); expr->op = TokenKind::BitOr;
        expr->rhs = parseBitXor(); lhs = std::move(expr);
    }
    return lhs;
}

std::unique_ptr<Expr> Parser::parseBitXor() {
    auto lhs = parseBitAnd();
    while (match(TokenKind::BitXor)) {
        auto expr = std::make_unique<BinaryExpr>();
        expr->lhs = std::move(lhs); expr->op = TokenKind::BitXor;
        expr->rhs = parseBitAnd(); lhs = std::move(expr);
    }
    return lhs;
}

std::unique_ptr<Expr> Parser::parseBitAnd() {
    auto lhs = parseEquality();
    while (match(TokenKind::Ampersand)) {
        auto expr = std::make_unique<BinaryExpr>();
        expr->lhs = std::move(lhs); expr->op = TokenKind::Ampersand;
        expr->rhs = parseEquality(); lhs = std::move(expr);
    }
    return lhs;
}

std::unique_ptr<Expr> Parser::parseEquality() {
    auto lhs = parseComparison();
    while (check(TokenKind::EqEq) || check(TokenKind::Neq)) {
        auto expr = std::make_unique<BinaryExpr>();
        expr->lhs = std::move(lhs); expr->op = advance().kind;
        expr->rhs = parseComparison(); lhs = std::move(expr);
    }
    return lhs;
}

std::unique_ptr<Expr> Parser::parseComparison() {
    auto lhs = parseShift();
    while (check(TokenKind::Lt) || check(TokenKind::LtEq) ||
           check(TokenKind::Gt) || check(TokenKind::GtEq)) {
        auto op = advance().kind;
        auto expr = std::make_unique<BinaryExpr>();
        expr->lhs = std::move(lhs);
        expr->op = op;
        expr->rhs = parseShift();
        lhs = std::move(expr);
    }
    return lhs;
}

std::unique_ptr<Expr> Parser::parseShift() {
    auto lhs = parseAddSub();
    while (check(TokenKind::ShiftLeft) || check(TokenKind::ShiftRight)) {
        auto expr = std::make_unique<BinaryExpr>();
        expr->lhs = std::move(lhs); expr->op = advance().kind;
        expr->rhs = parseAddSub(); lhs = std::move(expr);
    }
    return lhs;
}

std::unique_ptr<Expr> Parser::parseAddSub() {
    auto lhs = parseMulDiv();
    while (match(TokenKind::Plus) || match(TokenKind::Minus)) {
        auto expr = std::make_unique<BinaryExpr>();
        expr->lhs = std::move(lhs);
        expr->op = mTokens[mPos - 1].kind;
        expr->rhs = parseMulDiv();
        lhs = std::move(expr);
    }
    return lhs;
}

std::unique_ptr<Expr> Parser::parseMulDiv() {
    auto lhs = parseUnary();
    while (match(TokenKind::Star) || match(TokenKind::Slash) || match(TokenKind::Percent)) {
        auto expr = std::make_unique<BinaryExpr>();
        expr->lhs = std::move(lhs);
        expr->op = mTokens[mPos - 1].kind;
        expr->rhs = parseUnary();
        lhs = std::move(expr);
    }
    return lhs;
}

std::unique_ptr<Expr> Parser::parseUnary() {
    if (match(TokenKind::Dynamic)) {
        if (!match(TokenKind::Select)) {
            addError("`dynamic` in expression position must introduce `select`",
                     "write `dynamic select target with selector(...)`");
            return std::make_unique<IntLiteralExpr>(0);
        }
        return parseSelectExpr(true);
    }
    if (match(TokenKind::Minus) || match(TokenKind::Not) || match(TokenKind::Tilde) ||
        match(TokenKind::Star)) {
        auto expr = std::make_unique<UnaryExpr>();
        expr->op = mTokens[mPos - 1].kind;
        expr->operand = parseUnary();
        return expr;
    }
    if (match(TokenKind::Ampersand)) {
        auto expr = std::make_unique<AddrOfExpr>();
        expr->isMutable = match(TokenKind::Mut);
        expr->operand = parseUnary();
        return expr;
    }
    if (match(TokenKind::Move)) {
        auto expr = std::make_unique<MoveExpr>();
        expr->operand = parseUnary();
        return expr;
    }
    if (match(TokenKind::Borrow)) {
        auto expr = std::make_unique<BorrowExpr>();
        expr->isMutable = match(TokenKind::Mut);
        expr->operand = parseUnary();
        return expr;
    }
    return parsePostfix();
}

std::unique_ptr<Expr> Parser::parsePostfix() {
    auto expr = parsePrimary();

    while (true) {
        if (match(TokenKind::LParen)) {
            auto call = std::make_unique<CallExpr>();
            call->callee = std::move(expr);
            if (!check(TokenKind::RParen)) {
                call->args = parseArgs();
            }
            consume(TokenKind::RParen, "Expected ')' after call arguments");
            expr = std::move(call);
        } else if (match(TokenKind::Question)) {
            auto propagation = std::make_unique<TryExpr>();
            propagation->sourcePath = mSourceName;
            propagation->line = mTokens[mPos - 1].line;
            propagation->col = mTokens[mPos - 1].col;
            propagation->operand = std::move(expr);
            expr = std::move(propagation);
        } else if (match(TokenKind::At)) {
            addError("postfix `@tag(...)` versioning has been removed",
                     "declare a `meta` schema and write `select target with selector(...)` or `@selector(...) target`");
            break;
        } else if (match(TokenKind::Dot)) {
            auto access = std::make_unique<FieldAccessExpr>();
            access->object = std::move(expr);
            if (!match(TokenKind::Identifier)) {
                addError("Expected field name after '.'");
                break;
            }
            access->field = mTokens[mPos - 1].lexeme;
            expr = std::move(access);
        } else if (match(TokenKind::LBracket)) {
            auto first = parseExpr();
            if (match(TokenKind::DotDot)) {
                auto call = std::make_unique<CallExpr>();
                auto callee = std::make_unique<IdentifierExpr>("slice");
                callee->sourcePath = mSourceName; call->callee = std::move(callee);
                auto borrow = std::make_unique<BorrowExpr>();
                borrow->operand = std::move(expr);
                call->args.push_back(std::move(borrow));
                call->args.push_back(std::move(first));
                call->args.push_back(parseExpr());
                consume(TokenKind::RBracket, "Expected ']' after slice range");
                expr = std::move(call);
                continue;
            }
            auto idx = std::make_unique<IndexExpr>();
            idx->object = std::move(expr);
            idx->index = std::move(first);
            consume(TokenKind::RBracket, "Expected ']'");
            expr = std::move(idx);
        } else if (match(TokenKind::ColonColon)) {
            // Generic call: id::<T>(args), or ADT constructor:
            // Option::<i32>::Some(1) / Option::None().
            std::string ownerName;
            if (auto* owner = dynamic_cast<IdentifierExpr*>(expr.get()))
                ownerName = owner->name;
            std::vector<std::unique_ptr<TypeAST>> typeArgs;
            bool hadTypeArgs = match(TokenKind::Lt);
            if (hadTypeArgs) {
                if (!check(TokenKind::Gt)) {
                    do {
                        typeArgs.push_back(parseType());
                    } while (match(TokenKind::Comma));
                }
                consume(TokenKind::Gt, "Expected '>' after type arguments");
            }

            // A non-generic qualified value path remains an IdentifierExpr.
            // This lets semantic analysis resolve `alias::module::symbol`
            // against package and module namespaces. Enum constructors keep
            // their established UpperCamelCase terminal spelling.
            if (!hadTypeArgs && check(TokenKind::Identifier)) {
                std::vector<std::string> members;
                do {
                    if (!match(TokenKind::Identifier)) break;
                    members.push_back(mTokens[mPos - 1].lexeme);
                    if (!(check(TokenKind::ColonColon) &&
                          peekAhead(1).kind == TokenKind::Identifier))
                        break;
                    advance();
                } while (true);
                const bool isVariantConstructor = !members.empty() &&
                    !members.back().empty() &&
                    members.back().front() >= 'A' && members.back().front() <= 'Z' &&
                    check(TokenKind::LParen);
                if (isVariantConstructor) {
                    auto variant = std::make_unique<VariantConstructExpr>();
                    variant->typeName = ownerName;
                    for (size_t index = 0; index + 1 < members.size(); ++index)
                        variant->typeName += "::" + members[index];
                    variant->variantName = members.back();
                    consume(TokenKind::LParen, "Expected '(' after enum variant");
                    if (!check(TokenKind::RParen)) variant->args = parseArgs();
                    consume(TokenKind::RParen, "Expected ')' after enum variant arguments");
                    expr = std::move(variant);
                    continue;
                } else {
                    if (auto* owner = dynamic_cast<IdentifierExpr*>(expr.get())) {
                        for (const auto& member : members)
                            owner->name += "::" + member;
                    } else {
                        addError("qualified paths require an identifier owner");
                    }
                    continue;
                }
            }

            bool isVariant = match(TokenKind::ColonColon) ||
                             (!hadTypeArgs && check(TokenKind::Identifier));
            if (isVariant) {
                auto variant = std::make_unique<VariantConstructExpr>();
                variant->typeName = ownerName;
                variant->typeArgs = std::move(typeArgs);
                if (!match(TokenKind::Identifier)) {
                    addError("Expected enum variant name after '::'");
                    expr = std::move(variant);
                    continue;
                }
                variant->variantName = mTokens[mPos - 1].lexeme;
                consume(TokenKind::LParen, "Expected '(' after enum variant");
                if (!check(TokenKind::RParen)) variant->args = parseArgs();
                consume(TokenKind::RParen, "Expected ')' after enum variant arguments");
                expr = std::move(variant);
            } else {
                auto call = std::make_unique<CallExpr>();
                call->callee = std::move(expr);
                call->typeArgASTs = std::move(typeArgs);
                if (match(TokenKind::LParen)) {
                    if (!check(TokenKind::RParen)) call->args = parseArgs();
                    consume(TokenKind::RParen, "Expected ')' after call arguments");
                }
                expr = std::move(call);
            }
        } else {
            break;
        }
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parsePrimary() {
    if (match(TokenKind::Select)) return parseSelectExpr(false);
    if (match(TokenKind::At)) {
        const Token start = mTokens[mPos - 1];
        auto selection = std::make_unique<SelectExpr>();
        selection->sourcePath = mSourceName;
        selection->line = start.line;
        selection->col = start.col;
        if (!parseQualifiedName(selection->selectorName)) {
            addError("expected a selector function name after `@`",
                     "write `@selector(arguments) target`");
            return selection;
        }
        consume(TokenKind::LParen, "Expected '(' after selector function name");
        if (!check(TokenKind::RParen)) selection->selectorArgs = parseArgs();
        consume(TokenKind::RParen, "Expected ')' after selector arguments");
        if (!parseQualifiedName(selection->targetName)) {
            addError("expected a declaration family after selector sugar",
                     "write `@selector(arguments) target`");
            return selection;
        }
        return selection;
    }
    if (match(TokenKind::IntLiteral)) {
        const Token& token = mTokens[mPos - 1];
        auto node = std::make_unique<IntLiteralExpr>(std::stoll(token.lexeme));
        node->sourcePath = mSourceName; node->line = token.line; node->col = token.col;
        return node;
    }
    if (match(TokenKind::FloatLiteral)) {
        const Token& token = mTokens[mPos - 1];
        auto node = std::make_unique<FloatLiteralExpr>(std::stod(token.lexeme));
        node->sourcePath = mSourceName; node->line = token.line; node->col = token.col;
        return node;
    }
    if (match(TokenKind::StringLiteral)) {
        const Token& token = mTokens[mPos - 1];
        auto node = std::make_unique<StringLiteralExpr>(token.lexeme);
        node->sourcePath = mSourceName; node->line = token.line; node->col = token.col;
        return node;
    }
    if (match(TokenKind::True)) {
        const Token& token = mTokens[mPos - 1];
        auto node = std::make_unique<BoolLiteralExpr>(true);
        node->sourcePath = mSourceName; node->line = token.line; node->col = token.col;
        return node;
    }
    if (match(TokenKind::False)) {
        const Token& token = mTokens[mPos - 1];
        auto node = std::make_unique<BoolLiteralExpr>(false);
        node->sourcePath = mSourceName; node->line = token.line; node->col = token.col;
        return node;
    }
    if (match(TokenKind::Identifier)) {
        const Token& token = mTokens[mPos - 1];
        auto node = std::make_unique<IdentifierExpr>(token.lexeme);
        node->sourcePath = mSourceName; node->line = token.line; node->col = token.col;
        return node;
    }
    if (match(TokenKind::If)) {
        auto iexpr = std::make_unique<IfExpr>();
        iexpr->cond = parseExpr();
        iexpr->thenExpr = std::make_unique<BlockExpr>(parseBlock());
        if (match(TokenKind::Else)) {
            if (check(TokenKind::If)) {
                auto innerIf = std::make_unique<IfExpr>();
                advance(); // consume 'if'
                innerIf->cond = parseExpr();
                innerIf->thenExpr = std::make_unique<BlockExpr>(parseBlock());
                if (match(TokenKind::Else)) {
                    if (check(TokenKind::If)) {
                        // nested if-else-if: simplify to else block
                        innerIf->elseExpr = std::make_unique<BlockExpr>(parseBlock());
                    } else {
                        innerIf->elseExpr = std::make_unique<BlockExpr>(parseBlock());
                    }
                } else {
                    innerIf->elseExpr = std::make_unique<BlockExpr>(
                        std::make_unique<BlockStmt>());
                }
                iexpr->elseExpr = std::move(innerIf);
            } else {
                iexpr->elseExpr = std::make_unique<BlockExpr>(parseBlock());
            }
        } else {
            addError("If-expression requires an else branch");
        }
        return iexpr;
    }
    HeapStorageKind heapStorage = HeapStorageKind::Unique;
    bool isHeapAllocation = false;
    if (match(TokenKind::Rc)) {
        heapStorage = HeapStorageKind::Rc;
        consume(TokenKind::New, "Expected 'new' after 'rc'");
        isHeapAllocation = true;
    } else if (match(TokenKind::Arc)) {
        heapStorage = HeapStorageKind::Arc;
        consume(TokenKind::New, "Expected 'new' after 'arc'");
        isHeapAllocation = true;
    } else if (match(TokenKind::New)) {
        isHeapAllocation = true;
    }
    if (isHeapAllocation) {
        auto type = parseType();
        consume(TokenKind::LParen, "Expected '(' after type in new expression");
        auto alloc = std::make_unique<HeapAllocExpr>();
        alloc->storage = heapStorage;
        alloc->allocatedTypeAST = std::move(type);
        auto init = std::make_unique<CallExpr>();
        // Build a synthetic call: new T(args) → call T(args)
        auto* allocatedNamed = dynamic_cast<NamedTypeAST*>(alloc->allocatedTypeAST.get());
        auto calleeType = std::make_unique<IdentifierExpr>(
            allocatedNamed ? allocatedNamed->name : "?");
        init->callee = std::move(calleeType);
        if (!check(TokenKind::RParen)) {
            init->args = parseArgs();
        }
        consume(TokenKind::RParen, "Expected ')' after new arguments");
        alloc->initializer = std::move(init);
        return alloc;
    }
    if (match(TokenKind::LParen)) {
        auto expr = parseExpr();
        consume(TokenKind::RParen, "Expected ')' after grouped expression");
        return expr;
    }
    if (match(TokenKind::LBracket)) {
        const Token start = mTokens[mPos - 1];
        auto array = std::make_unique<ArrayLiteralExpr>();
        array->sourcePath = mSourceName; array->line = start.line; array->col = start.col;
        if (!check(TokenKind::RBracket)) {
            do { array->elements.push_back(parseExpr()); } while (match(TokenKind::Comma));
        }
        consume(TokenKind::RBracket, "Expected ']' after array literal");
        return array;
    }

    if (match(TokenKind::Fn)) {
        // Lambda: fn(params) -> Type { body } or fn(params) { body }
        return parseLambda();
    }
    if (match(TokenKind::Launch)) return parseLaunchExpr();
    addError("expected an expression, found " + diagnostic::quotedToken(peek().lexeme));
    advance(); // skip bad token to prevent infinite loop
    return std::make_unique<IntLiteralExpr>(0); // error recovery
}

std::unique_ptr<SelectExpr> Parser::parseSelectExpr(bool isDynamic,
                                                    bool selectAlreadyConsumed) {
    (void)selectAlreadyConsumed;
    const Token start = mTokens[mPos - 1];
    auto selection = std::make_unique<SelectExpr>();
    selection->sourcePath = mSourceName;
    selection->line = start.line;
    selection->col = start.col;
    selection->isDynamic = isDynamic;
    if (!parseQualifiedName(selection->targetName)) {
        addError("expected a declaration family after `select`",
                 "write `select target with selector(arguments)`");
        return selection;
    }
    consume(TokenKind::With, "Expected 'with' after select target");
    if (!parseQualifiedName(selection->selectorName)) {
        addError("expected a selector function after `with`");
        return selection;
    }
    consume(TokenKind::LParen, "Expected '(' after selector function name");
    if (!check(TokenKind::RParen)) selection->selectorArgs = parseArgs();
    consume(TokenKind::RParen, "Expected ')' after selector arguments");
    return selection;
}

std::unique_ptr<Expr> Parser::parseLaunchExpr() {
    const Token start = mTokens[mPos - 1]; // `launch` already consumed
    auto launch = std::make_unique<LaunchExpr>();
    launch->sourcePath = mSourceName; launch->line = start.line; launch->col = start.col;
    if (!parseQualifiedName(launch->kernelName)) {
        addError("expected a kernel name after `launch`",
                 "write `launch kernel_name[threads: count](arguments)`");
        return launch;
    }
    consume(TokenKind::LBracket, "Expected '[' after kernel name in launch");
    if (!match(TokenKind::Identifier) || mTokens[mPos - 1].lexeme != "threads") {
        addError("expected `threads` launch option", "write `[threads: count]`");
    }
    consume(TokenKind::Colon, "Expected ':' after `threads`");
    launch->threads = parseExpr();
    consume(TokenKind::RBracket, "Expected ']' after launch options");
    consume(TokenKind::LParen, "Expected '(' before launch arguments");
    if (!check(TokenKind::RParen)) launch->args = parseArgs();
    consume(TokenKind::RParen, "Expected ')' after launch arguments");
    return launch;
}

std::unique_ptr<Expr> Parser::parseLambda() {
    // Already consumed 'fn' in parsePrimary
    consume(TokenKind::LParen, "Expected '(' after fn in lambda");
    auto params = parseParams();
    consume(TokenKind::RParen, "Expected ')' after lambda params");

    auto lambda = std::make_unique<LambdaExpr>();
    lambda->params = std::move(params);

    if (match(TokenKind::Arrow)) {
        lambda->returnType = parseType();
    }

    lambda->body = parseBlock();
    return lambda;
}

std::unique_ptr<Expr> Parser::parseIfExpr() {
    auto expr = std::make_unique<IfExpr>();
    expr->cond = parseExpr();
    expr->thenExpr = std::make_unique<BlockExpr>(parseBlock());
    consume(TokenKind::Else, "If-expression requires 'else'");
    if (check(TokenKind::If)) {
        expr->elseExpr = parseIfExpr();
    } else {
        expr->elseExpr = std::make_unique<BlockExpr>(parseBlock());
    }
    return expr;
}

// ─── Types ─────────────────────────────────────────────────────────

std::unique_ptr<TypeAST> Parser::parseType() {
    if (match(TokenKind::Linear)) {
        return std::make_unique<LinearTypeAST>(parseType());
    }
    if (match(TokenKind::Affine)) {
        return std::make_unique<AffineTypeAST>(parseType());
    }
    // & Type
    if (match(TokenKind::Ampersand)) {
        bool isMutable = match(TokenKind::Mut);
        auto ref = std::make_unique<RefTypeAST>(parseType(), isMutable);
        return ref;
    }
    // Closure type: (ParamType, ...) -> ReturnType. `fn` remains reserved
    // for lambda expressions and function declarations.
    if (check(TokenKind::LParen)) {
        return parseFunctionType();
    }
    // Self
    if (match(TokenKind::Self)) {
        return std::make_unique<NamedTypeAST>("Self");
    }
    if (match(TokenKind::Auto)) {
        return std::make_unique<NamedTypeAST>("auto");
    }
    if (check(TokenKind::Rc) || check(TokenKind::Arc)) {
        const Token wrapper = advance();
        auto named = std::make_unique<NamedTypeAST>(wrapper.lexeme);
        named->sourcePath = mSourceName;
        named->line = wrapper.line;
        named->col = wrapper.col;
        consume(TokenKind::Lt, "Expected '<' after shared ownership type");
        named->typeArgs.push_back(parseType());
        consume(TokenKind::Gt, "Expected '>' after shared ownership element type");
        return named;
    }
    // Built-in type keywords
    if (check(TokenKind::TyI32) || check(TokenKind::TyI64) ||
        check(TokenKind::TyF32) || check(TokenKind::TyF64) ||
        check(TokenKind::TyBool) || check(TokenKind::TyString)) {
        auto tok = advance();
        return std::make_unique<NamedTypeAST>(tok.lexeme);
    }
    // Named type (user type or type param)
    if (match(TokenKind::Identifier)) {
        const Token typeName = mTokens[mPos - 1];
        auto named = std::make_unique<NamedTypeAST>(typeName.lexeme);
        named->sourcePath = mSourceName;
        named->line = typeName.line;
        named->col = typeName.col;
        while (match(TokenKind::ColonColon)) {
            if (!match(TokenKind::Identifier)) {
                addError("Expected type name component after '::'");
                break;
            }
            named->name += "::" + mTokens[mPos - 1].lexeme;
        }
        if (match(TokenKind::Lt)) {
            named->typeArgs.push_back(parseType());
            if (named->name == "array" && match(TokenKind::Comma)) {
                if (match(TokenKind::IntLiteral))
                    named->arrayLength = static_cast<uint64_t>(std::stoull(mTokens[mPos - 1].lexeme));
                else
                    addError("array<T, N> requires a non-negative integer compile-time length",
                             "write `array<i32, 4>`, not a runtime expression");
            } else while (match(TokenKind::Comma)) {
                named->typeArgs.push_back(parseType());
            }
            consume(TokenKind::Gt, "Expected '>' after type arguments");
        }
        return named;
    }

    addError("expected a type, found " + diagnostic::quotedToken(peek().lexeme),
             "use a built-in type such as `i32`, or a declared type name");
    return std::make_unique<NamedTypeAST>("i32"); // error recovery
}

std::unique_ptr<TypeAST> Parser::parseFunctionType() {
    consume(TokenKind::LParen, "Expected '(' in closure type");
    auto ft = std::make_unique<FunctionTypeAST>();
    if (!check(TokenKind::RParen)) {
        do {
            ft->paramTypes.push_back(parseType());
        } while (match(TokenKind::Comma));
    }
    consume(TokenKind::RParen, "Expected ')' after closure parameter types");
    consume(TokenKind::Arrow, "Expected '->' in closure type");
    ft->returnType = parseType();
    return ft;
}

// ─── Helpers ───────────────────────────────────────────────────────

std::vector<Param> Parser::parseParams() {
    std::vector<Param> params;
    if (check(TokenKind::RParen)) return params;

    do {
        Param p;
        if (match(TokenKind::Linear)) {
            p.isLinear = true;
            p.usage = luna::ownership::Usage::Linear;
            p.hasExplicitUsage = true;
        } else if (match(TokenKind::Affine)) {
            p.usage = luna::ownership::Usage::Affine;
            p.hasExplicitUsage = true;
        }
        if (!match(TokenKind::Identifier)) {
            addError("Expected parameter name");
            break;
        }
        p.name = mTokens[mPos - 1].lexeme;
        // A parameter type is an optional constraint. If it is omitted, the
        // semantic analyzer creates an inference variable.
        if (match(TokenKind::Colon)) p.type = parseType();
        if (dynamic_cast<RefTypeAST*>(p.type.get())) {
            auto* reference = static_cast<RefTypeAST*>(p.type.get());
            p.relation = reference->isMutable
                ? luna::ownership::Relation::MutableBorrow
                : luna::ownership::Relation::SharedBorrow;
            p.usage = luna::ownership::Usage::Copy;
        }
        params.push_back(std::move(p));
    } while (match(TokenKind::Comma));

    return params;
}

std::vector<std::unique_ptr<Expr>> Parser::parseArgs() {
    std::vector<std::unique_ptr<Expr>> args;
    do {
        args.push_back(parseExpr());
    } while (match(TokenKind::Comma));
    return args;
}

std::vector<std::string> Parser::parseTypeParamList() {
    std::vector<std::string> params;
    if (match(TokenKind::Lt)) {
        do {
            if (!match(TokenKind::Identifier)) {
                addError("Expected type parameter name");
                break;
            }
            params.push_back(mTokens[mPos - 1].lexeme);
        } while (match(TokenKind::Comma));
        consume(TokenKind::Gt, "Expected '>' after type parameters");
    }
    return params;
}

std::vector<WhereClause> Parser::parseWhereClause() {
    std::vector<WhereClause> clauses;
    if (match(TokenKind::Where)) {
        do {
            if (!match(TokenKind::Identifier)) {
                addError("Expected type parameter name in where clause");
                break;
            }
            const Token first = mTokens[mPos - 1];
            WhereClause clause;
            if (match(TokenKind::Colon)) {
                clause.kind = WhereClause::Kind::TraitBound;
                clause.typeParam = first.lexeme;
                if (!match(TokenKind::Identifier)) {
                    addError("Expected trait name in where clause");
                    break;
                }
                clause.trait = parseTraitRef(mTokens[mPos - 1]);
            } else {
                clause.kind = WhereClause::Kind::Constraint;
                clause.constraintName = first.lexeme;
                parseQualifiedNameTail(clause.constraintName);
                consume(TokenKind::Lt,
                        "Expected '<' after constraint name in where clause");
                if (!check(TokenKind::Gt)) {
                    do {
                        clause.constraintTypeArgs.push_back(parseType());
                    } while (match(TokenKind::Comma));
                }
                consume(TokenKind::Gt,
                        "Expected '>' after constraint type arguments");
            }
            clauses.push_back(std::move(clause));
        } while (match(TokenKind::Comma));
    }
    return clauses;
}

TraitRef Parser::parseTraitRef(const Token& nameToken) {
    TraitRef trait;
    trait.name = nameToken.lexeme;
    parseQualifiedNameTail(trait.name);
    trait.sourcePath = mSourceName;
    trait.line = nameToken.line;
    trait.col = nameToken.col;
    return trait;
}

// ─── Token helpers ─────────────────────────────────────────────────

const Token& Parser::peek() const {
    static Token eof(TokenKind::EndOfFile, "", 0, 0);
    if (mPos >= (int)mTokens.size() || mTokens[mPos].kind == TokenKind::EndOfFile) {
        return eof;
    }
    return mTokens[mPos];
}

const Token& Parser::peekAhead(int n) const {
    int idx = mPos + n;
    if (idx >= (int)mTokens.size()) {
        static Token eof(TokenKind::EndOfFile, "", 0, 0);
        return eof;
    }
    return mTokens[idx];
}

Token Parser::advance() {
    return mTokens[mPos++];
}

bool Parser::check(TokenKind kind) const {
    return peek().kind == kind;
}

bool Parser::match(TokenKind kind) {
    if (check(kind)) {
        mPos++;
        return true;
    }
    return false;
}

Token Parser::consume(TokenKind kind, const std::string& errorMsg) {
    if (check(kind)) {
        return advance();
    }
    addError(errorMsg + ", found " + diagnostic::quotedToken(peek().lexeme));
    return Token(TokenKind::Error, "", peek().line, peek().col);
}

bool Parser::isAtEnd() const {
    return mPos >= (int)mTokens.size() || peek().kind == TokenKind::EndOfFile;
}

bool Parser::isNamedSlotInvocationStart() const {
    if (!check(TokenKind::Identifier) || peekAhead(1).kind != TokenKind::LParen) return false;
    int depth = 0;
    for (int i = mPos + 1; i < static_cast<int>(mTokens.size()); ++i) {
        const TokenKind kind = mTokens[i].kind;
        if (kind == TokenKind::LParen) ++depth;
        else if (kind == TokenKind::RParen && --depth == 0)
            return i + 1 < static_cast<int>(mTokens.size()) &&
                   mTokens[i + 1].kind == TokenKind::LBrace;
        else if (kind == TokenKind::EndOfFile) return false;
    }
    return false;
}

void Parser::addError(const std::string& msg, const std::string& hint) {
    std::string message = msg;
    if (message.rfind("Expected", 0) == 0) message[0] = 'e';
    std::string resolvedHint = hint;
    if (resolvedHint.empty()) {
        if (message.find("';'") != std::string::npos)
            resolvedHint = "terminate this statement with `;`";
        else if (message.find("')'") != std::string::npos)
            resolvedHint = "check that every `(` has a matching `)`";
        else if (message.find("'}'") != std::string::npos)
            resolvedHint = "check that every `{` has a matching `}`";
        else if (message.find("variable name") != std::string::npos)
            resolvedHint = "write an identifier after `let`, for example `let value = ...;`";
        else if (message.find("function name") != std::string::npos)
            resolvedHint = "write an identifier after `fn`";
    }
    mErrors.push_back(diagnostic::format("parse", message, mSourceName, peek().line, peek().col,
                                         resolvedHint, sourceLineAt(peek().line)));
}

void Parser::synchronizeDeclaration() {
    while (!isAtEnd()) {
        if (match(TokenKind::SemiColon)) return;
        switch (peek().kind) {
            case TokenKind::Export:
            case TokenKind::Constexpr:
            case TokenKind::Extern:
            case TokenKind::Kernel:
            case TokenKind::Fn:
            case TokenKind::Fragment:
            case TokenKind::Interceptor:
            case TokenKind::Context:
            case TokenKind::Struct:
            case TokenKind::Enum:
            case TokenKind::Nominal:
            case TokenKind::Trait:
            case TokenKind::Impl:
            case TokenKind::Meta:
            case TokenKind::Constraint:
            case TokenKind::Runtime:
            case TokenKind::Dynamic:
            case TokenKind::At:
                return;
            default:
                advance();
        }
    }
}

void Parser::synchronizeStatement() {
    while (!isAtEnd() && !check(TokenKind::SemiColon) && !check(TokenKind::RBrace))
        advance();
    if (check(TokenKind::SemiColon)) advance();
}

std::string Parser::sourceLineAt(int line) const {
    if (line <= 0) return "";
    int current = 1;
    size_t begin = 0;
    for (size_t i = 0; i <= mSource.size(); ++i) {
        if (i == mSource.size() || mSource[i] == '\n') {
            if (current == line) return mSource.substr(begin, i - begin);
            begin = i + 1;
            ++current;
        }
    }
    return "";
}
