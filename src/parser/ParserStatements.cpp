#include "../diagnostics/Diagnostic.h"
#include "Parser.h"

std::unique_ptr<BlockStmt> Parser::parseBlock() {
    auto block = std::make_unique<BlockStmt>();
    consume(TokenKind::LBrace, "Expected '{'");
    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        if (auto stmt = parseStatement()) { block->stmts.push_back(std::move(stmt)); }
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
        if (check(TokenKind::LBrace)) {
            const auto savedDefault = mUsageDefault;
            mUsageDefault = luna::ownership::Usage::Linear;
            auto block = parseBlock();
            mUsageDefault = savedDefault;
            return stamp(std::move(block));
        }
        if (match(TokenKind::Let))
            return stamp(parseLetStmt(luna::ownership::Usage::Linear, false, true));
        addError("expected `let` or `{` after `linear`",
                 "write `linear let name = value;` or `linear { ... }`");
        synchronizeStatement();
        return nullptr;
    }
    if (match(TokenKind::Affine)) {
        if (check(TokenKind::LBrace)) {
            const auto savedDefault = mUsageDefault;
            mUsageDefault = luna::ownership::Usage::Affine;
            auto block = parseBlock();
            mUsageDefault = savedDefault;
            return stamp(std::move(block));
        }
        if (match(TokenKind::Let))
            return stamp(parseLetStmt(luna::ownership::Usage::Affine, false, true));
        addError("expected `let` or `{` after `affine`",
                 "write `affine let name = value;` or `affine { ... }`");
        synchronizeStatement();
        return nullptr;
    }
    if (match(TokenKind::Copy)) {
        if (match(TokenKind::Let))
            return stamp(parseLetStmt(luna::ownership::Usage::Copy, false, true));
        addError("expected `let` after `copy`", "write `copy let name = value;`");
        synchronizeStatement();
        return nullptr;
    }
    if (match(TokenKind::Const)) {
        if (match(TokenKind::Let)) return stamp(parseLetStmt(luna::ownership::Usage::Copy, true));
        addError("expected `let` after `const`",
                 "write `const let name = compile_time_expression;`");
        synchronizeStatement();
        return nullptr;
    }
    if (match(TokenKind::Let))
        return stamp(parseLetStmt(luna::ownership::Usage::Copy, match(TokenKind::Const)));
    if (match(TokenKind::Free)) return stamp(parseFreeStmt());
    if (match(TokenKind::Slot)) {
        addError("slot declarations are module-level in Luna 0.3",
                 "move `slot interceptor/context name(...);` outside the function and invoke it as "
                 "`name(args) { ... }`");
        synchronizeStatement();
        return nullptr;
    }
    if (match(TokenKind::Resume)) return stamp(parseResumeStmt());
    if (match(TokenKind::Abort)) return stamp(parseAbortStmt());
    if (match(TokenKind::Await)) return stamp(parseAwaitStmt());
    if (match(TokenKind::Apply)) return stamp(parseApplyStmt());
    if (match(TokenKind::Dynamic)) {
        addError("`dynamic slot` and `dynamic apply` were removed in Luna 0.3",
                 "use a statically named fragment with ordinary lexical `apply`; runtime fragment "
                 "acquisition/application is not exposed in Luna 0.3");
        synchronizeStatement();
        return nullptr;
    }
    if (match(TokenKind::Return)) return stamp(parseReturnStmt());
    if (match(TokenKind::If)) return stamp(parseIfStmt());
    if (match(TokenKind::Match)) return stamp(parseMatchStmt());
    if (match(TokenKind::While)) return stamp(parseWhileStmt());
    if (match(TokenKind::For)) return stamp(parseForStmt());
    if (check(TokenKind::LBrace)) return stamp(parseBlock());
    if (isNamedSlotInvocationStart()) return stamp(parseNamedSlotInvokeStmt());
    if (check(TokenKind::Identifier) && peekAhead(1).kind == TokenKind::Eq) {
        // Assignment statement (lhs = expr ;) — handled in parseExprStmt via parseExpr
    }
    return stamp(parseExprStmt());
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
    stmt->sourcePath = mSourceName;
    stmt->line = start.line;
    stmt->col = start.col;
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseAbortStmt() {
    const Token start = mTokens[mPos - 1];
    consume(TokenKind::LParen, "Expected '(' after `abort`");
    consume(TokenKind::RParen, "Expected ')' after `abort`");
    consume(TokenKind::SemiColon, "Expected ';' after `abort()`");
    auto stmt = std::make_unique<AbortStmt>();
    stmt->sourcePath = mSourceName;
    stmt->line = start.line;
    stmt->col = start.col;
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseAwaitStmt() {
    const Token start = mTokens[mPos - 1]; // `await` already consumed
    auto stmt = std::make_unique<AwaitStmt>();
    stmt->event = parseExpr();
    consume(TokenKind::SemiColon, "Expected ';' after await expression");
    stmt->sourcePath = mSourceName;
    stmt->line = start.line;
    stmt->col = start.col;
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseApplyStmt() {
    const Token start = mTokens[mPos - 1]; // `apply` already consumed
    auto stmt = std::make_unique<ApplyStmt>();
    if (!parseQualifiedName(stmt->fragmentName)) {
        addError("expected a statically named fragment after `apply`");
        synchronizeStatement();
        return nullptr;
    }
    if (!check(TokenKind::LBrace)) {
        addError("lexical `apply` requires a body",
                 "write `apply fragment_name { ... }`; blockless apply was removed in Luna 0.3");
        synchronizeStatement();
        return nullptr;
    }
    stmt->body = parseBlock();
    stmt->sourcePath = mSourceName;
    stmt->line = start.line;
    stmt->col = start.col;
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseNamedSlotInvokeStmt() {
    const Token start = peek();
    auto stmt = std::make_unique<SlotInvokeStmt>();
    if (!parseQualifiedName(stmt->name)) {
        addError("expected a module-level slot name");
        return nullptr;
    }
    consume(TokenKind::LParen, "Expected '(' after slot name");
    if (!check(TokenKind::RParen)) stmt->args = parseArgs();
    consume(TokenKind::RParen, "Expected ')' after slot arguments");
    stmt->continuation = parseBlock();
    stmt->sourcePath = mSourceName;
    stmt->line = start.line;
    stmt->col = start.col;
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseLetStmt(luna::ownership::Usage usage, bool isConst,
                                           bool hasExplicitUsage) {
    auto stmt = std::make_unique<LetStmt>();
    stmt->usage = usage;
    stmt->hasExplicitUsage = hasExplicitUsage;
    stmt->isLinear = usage == luna::ownership::Usage::Linear;
    stmt->isConst = isConst;
    if (!hasExplicitUsage && mUsageDefault != luna::ownership::Usage::Copy) {
        stmt->hasInheritedUsage = true;
        stmt->inheritedUsage = mUsageDefault;
    }
    if (!match(TokenKind::Identifier)) {
        addError("Expected variable name after 'let'");
        synchronizeStatement();
        return nullptr;
    }
    stmt->name = mTokens[mPos - 1].lexeme;

    if (match(TokenKind::Colon)) { stmt->typeAnnotation = parseType(); }

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
    if (!check(TokenKind::SemiColon)) { stmt->value = parseExpr(); }
    consume(TokenKind::SemiColon, "Expected ';' after return");
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseIfStmt() {
    auto stmt = std::make_unique<IfStmt>();
    stmt->cond = parseExprBeforeBlock();
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

std::unique_ptr<Stmt> Parser::parseMatchStmt() {
    const Token start = mTokens[mPos - 1];
    auto statement = std::make_unique<MatchStmt>();
    statement->scrutinee = parseExprBeforeBlock();
    consume(TokenKind::LBrace, "Expected '{' after match value");

    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        if (!match(TokenKind::Identifier)) {
            addError("Expected an enum variant pattern in match arm");
            synchronizeStatement();
            break;
        }
        MatchArm arm;
        const Token first = mTokens[mPos - 1];
        std::vector<Token> path{first};
        while (match(TokenKind::ColonColon)) {
            if (!match(TokenKind::Identifier)) {
                addError("Expected a variant name after '::' in match pattern");
                break;
            }
            path.push_back(mTokens[mPos - 1]);
        }
        const auto& variantToken = path.back();
        arm.sourcePath = mSourceName;
        arm.line = variantToken.line;
        arm.col = variantToken.col;
        arm.variantName = variantToken.lexeme;
        if (path.size() > 1) {
            arm.qualifierLine = first.line;
            arm.qualifierCol = first.col;
            for (size_t index = 0; index + 1 < path.size(); ++index) {
                if (!arm.typeQualifier.empty()) arm.typeQualifier += "::";
                arm.typeQualifier += path[index].lexeme;
            }
        }
        if (match(TokenKind::LParen)) {
            if (!check(TokenKind::RParen)) {
                do {
                    if (!match(TokenKind::Identifier)) {
                        addError("Expected a payload binding in enum match pattern");
                        synchronizeStatement();
                        break;
                    }
                    arm.bindings.push_back(mTokens[mPos - 1].lexeme);
                    arm.bindingUsageDefaults.push_back(mUsageDefault);
                } while (match(TokenKind::Comma));
            }
            consume(TokenKind::RParen, "Expected ')' after enum match bindings");
        }
        consume(TokenKind::FatArrow, "Expected '=>' after enum match pattern");
        arm.body = parseBlock();
        match(TokenKind::Comma);
        statement->arms.push_back(std::move(arm));
    }
    consume(TokenKind::RBrace, "Expected '}' after match arms");
    if (statement->arms.empty()) addError("match requires at least one arm");
    statement->sourcePath = mSourceName;
    statement->line = start.line;
    statement->col = start.col;
    return statement;
}

std::unique_ptr<Stmt> Parser::parseWhileStmt() {
    auto stmt = std::make_unique<WhileStmt>();
    stmt->cond = parseExprBeforeBlock();
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
    if (mUsageDefault != luna::ownership::Usage::Copy) {
        stmt->hasInheritedUsage = true;
        stmt->inheritedUsage = mUsageDefault;
    }
    if (!check(TokenKind::Identifier) || mTokens[mPos].lexeme != "in") {
        addError("Expected 'in' in for-loop");
        return nullptr;
    }
    advance(); // consume "in" (it's an identifier, not a keyword)
    stmt->iterable = parseExprBeforeBlock();
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
