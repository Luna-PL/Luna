#include "../diagnostics/Diagnostic.h"
#include "Parser.h"

#include <charconv>
#include <cmath>
#include <locale>
#include <sstream>
#include <unordered_set>

namespace {

constexpr int kMaxParseNestingDepth = 256;

bool parseFiniteF64(const std::string& text, double& value) {
    std::istringstream input(text);
    input.imbue(std::locale::classic());
    if (!(input >> value) || !std::isfinite(value)) return false;
    return input.peek() == std::char_traits<char>::eof();
}

} // namespace

std::unique_ptr<Expr> Parser::parseExpr() { return parseAssignment(); }

std::unique_ptr<Expr> Parser::parseExprBeforeBlock() {
    const bool saved = mStopBeforeBlockBrace;
    mStopBeforeBlockBrace = true;
    auto expression = parseExpr();
    mStopBeforeBlockBrace = saved;
    return expression;
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
        expr->lhs = std::move(lhs);
        expr->op = TokenKind::BitOr;
        expr->rhs = parseBitXor();
        lhs = std::move(expr);
    }
    return lhs;
}

std::unique_ptr<Expr> Parser::parseBitXor() {
    auto lhs = parseBitAnd();
    while (match(TokenKind::BitXor)) {
        auto expr = std::make_unique<BinaryExpr>();
        expr->lhs = std::move(lhs);
        expr->op = TokenKind::BitXor;
        expr->rhs = parseBitAnd();
        lhs = std::move(expr);
    }
    return lhs;
}

std::unique_ptr<Expr> Parser::parseBitAnd() {
    auto lhs = parseEquality();
    while (match(TokenKind::Ampersand)) {
        auto expr = std::make_unique<BinaryExpr>();
        expr->lhs = std::move(lhs);
        expr->op = TokenKind::Ampersand;
        expr->rhs = parseEquality();
        lhs = std::move(expr);
    }
    return lhs;
}

std::unique_ptr<Expr> Parser::parseEquality() {
    auto lhs = parseComparison();
    while (check(TokenKind::EqEq) || check(TokenKind::Neq)) {
        auto expr = std::make_unique<BinaryExpr>();
        expr->lhs = std::move(lhs);
        expr->op = advance().kind;
        expr->rhs = parseComparison();
        lhs = std::move(expr);
    }
    return lhs;
}

std::unique_ptr<Expr> Parser::parseComparison() {
    auto lhs = parseShift();
    while (check(TokenKind::Lt) || check(TokenKind::LtEq) || check(TokenKind::Gt) ||
           check(TokenKind::GtEq)) {
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
        expr->lhs = std::move(lhs);
        expr->op = advance().kind;
        expr->rhs = parseAddSub();
        lhs = std::move(expr);
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
            addError("Dynamic expressions were removed in Luna 0.3",
                     "use compile-time `select` or a typed host binding");
            return std::make_unique<IntLiteralExpr>(0);
        }
        addError("`dynamic select` was removed in Luna 0.3",
                 "use compile-time `select`; runtime replacement is an EV004 typed host binding");
        return parseSelectExpr();
    }
    if (match(TokenKind::Minus) || match(TokenKind::Not) || match(TokenKind::Tilde)) {
        auto expr = std::make_unique<UnaryExpr>();
        expr->op = mTokens[mPos - 1].kind;
        expr->operand = parseUnary();
        return expr;
    }
    if (match(TokenKind::Star)) {
        auto expr = std::make_unique<DerefExpr>();
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

    const auto isQualifiedValueName = [&]() {
        return check(TokenKind::Identifier) || check(TokenKind::New);
    };
    const auto consumeQualifiedValueName = [&]() {
        if (!isQualifiedValueName()) return false;
        advance();
        return true;
    };

    while (true) {
        if (!mStopBeforeBlockBrace && check(TokenKind::LBrace)) {
            auto* target = dynamic_cast<IdentifierExpr*>(expr.get());
            if (!target) break;
            auto targetType = std::make_unique<NamedTypeAST>(target->name);
            targetType->sourcePath = target->sourcePath;
            targetType->line = target->line;
            targetType->col = target->col;
            expr = parseRecordLiteral(std::move(targetType));
        } else if (match(TokenKind::LParen)) {
            auto call = std::make_unique<CallExpr>();
            call->callee = std::move(expr);
            if (!check(TokenKind::RParen)) { call->args = parseArgs(); }
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
                     "declare a `meta` schema and write `select target with selector(...)` or "
                     "`@selector(...) target`");
            break;
        } else if (match(TokenKind::Dot)) {
            auto access = std::make_unique<FieldAccessExpr>();
            access->object = std::move(expr);
            if (!match(TokenKind::Identifier)) {
                addError("Expected field name after '.'");
                break;
            }
            const auto& fieldToken = mTokens[mPos - 1];
            access->sourcePath = mSourceName;
            access->line = fieldToken.line;
            access->col = fieldToken.col;
            access->field = fieldToken.lexeme;
            expr = std::move(access);
        } else if (match(TokenKind::LBracket)) {
            auto first = parseExpr();
            if (match(TokenKind::DotDot)) {
                auto call = std::make_unique<CallExpr>();
                auto callee = std::make_unique<IdentifierExpr>("slice");
                callee->sourcePath = mSourceName;
                call->callee = std::move(callee);
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
            const auto* ownerLocation = dynamic_cast<IdentifierExpr*>(expr.get());
            if (ownerLocation) ownerName = ownerLocation->name;
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
            if (!hadTypeArgs && isQualifiedValueName()) {
                std::vector<Token> members;
                do {
                    if (!consumeQualifiedValueName()) break;
                    members.push_back(mTokens[mPos - 1]);
                    if (!(check(TokenKind::ColonColon) &&
                          (peekAhead(1).kind == TokenKind::Identifier ||
                           peekAhead(1).kind == TokenKind::New)))
                        break;
                    advance();
                } while (true);
                const bool isVariantConstructor =
                    !members.empty() && !members.back().lexeme.empty() &&
                    members.back().lexeme.front() >= 'A' && members.back().lexeme.front() <= 'Z' &&
                    check(TokenKind::LParen);
                if (isVariantConstructor) {
                    auto variant = std::make_unique<VariantConstructExpr>();
                    variant->typeName = ownerName;
                    for (size_t index = 0; index + 1 < members.size(); ++index)
                        variant->typeName += "::" + members[index].lexeme;
                    variant->typeSourcePath = ownerLocation->sourcePath;
                    variant->typeLine = ownerLocation->line;
                    variant->typeCol = ownerLocation->col;
                    variant->variantName = members.back().lexeme;
                    variant->sourcePath = mSourceName;
                    variant->line = members.back().line;
                    variant->col = members.back().col;
                    consume(TokenKind::LParen, "Expected '(' after enum variant");
                    if (!check(TokenKind::RParen)) variant->args = parseArgs();
                    consume(TokenKind::RParen, "Expected ')' after enum variant arguments");
                    expr = std::move(variant);
                    continue;
                } else {
                    if (auto* owner = dynamic_cast<IdentifierExpr*>(expr.get())) {
                        for (const auto& member : members)
                            owner->name += "::" + member.lexeme;
                    } else {
                        addError("qualified paths require an identifier owner");
                    }
                    continue;
                }
            }

            bool isVariant =
                match(TokenKind::ColonColon) || (!hadTypeArgs && check(TokenKind::Identifier));
            if (isVariant) {
                auto variant = std::make_unique<VariantConstructExpr>();
                variant->typeName = ownerName;
                if (ownerLocation) {
                    variant->typeSourcePath = ownerLocation->sourcePath;
                    variant->typeLine = ownerLocation->line;
                    variant->typeCol = ownerLocation->col;
                }
                variant->typeArgs = std::move(typeArgs);
                if (!match(TokenKind::Identifier)) {
                    addError("Expected enum variant name after '::'");
                    expr = std::move(variant);
                    continue;
                }
                const auto& variantToken = mTokens[mPos - 1];
                variant->variantName = variantToken.lexeme;
                variant->sourcePath = mSourceName;
                variant->line = variantToken.line;
                variant->col = variantToken.col;
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
    if (match(TokenKind::Select)) return parseSelectExpr();
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
        uint64_t parsed = 0;
        const auto result = std::from_chars(
            token.lexeme.data(), token.lexeme.data() + token.lexeme.size(),
            parsed);
        if (result.ec == std::errc::result_out_of_range ||
            result.ptr != token.lexeme.data() + token.lexeme.size()) {
            addErrorAt(
                token,
                "integer literal is outside the supported unsigned 64-bit range",
                "use a value from 0 through 18446744073709551615");
            parsed = 0;
        }
        auto node = std::make_unique<IntLiteralExpr>(parsed);
        node->sourcePath = mSourceName;
        node->line = token.line;
        node->col = token.col;
        return node;
    }
    if (match(TokenKind::FloatLiteral)) {
        const Token& token = mTokens[mPos - 1];
        double parsed = 0.0;
        if (!parseFiniteF64(token.lexeme, parsed)) {
            addErrorAt(
                token,
                "floating-point literal is outside the finite f64 range",
                "use a finite IEEE-754 f64 value");
            parsed = 0.0;
        }
        auto node = std::make_unique<FloatLiteralExpr>(parsed);
        node->sourcePath = mSourceName;
        node->line = token.line;
        node->col = token.col;
        return node;
    }
    if (match(TokenKind::StringLiteral)) {
        const Token& token = mTokens[mPos - 1];
        auto node = std::make_unique<StringLiteralExpr>(token.lexeme);
        node->sourcePath = mSourceName;
        node->line = token.line;
        node->col = token.col;
        return node;
    }
    if (match(TokenKind::True)) {
        const Token& token = mTokens[mPos - 1];
        auto node = std::make_unique<BoolLiteralExpr>(true);
        node->sourcePath = mSourceName;
        node->line = token.line;
        node->col = token.col;
        return node;
    }
    if (match(TokenKind::False)) {
        const Token& token = mTokens[mPos - 1];
        auto node = std::make_unique<BoolLiteralExpr>(false);
        node->sourcePath = mSourceName;
        node->line = token.line;
        node->col = token.col;
        return node;
    }
    if (check(TokenKind::LBrace)) return parseRecordLiteral();
    if (match(TokenKind::Identifier)) {
        const Token& token = mTokens[mPos - 1];
        auto node = std::make_unique<IdentifierExpr>(token.lexeme);
        node->sourcePath = mSourceName;
        node->line = token.line;
        node->col = token.col;
        return node;
    }
    if (match(TokenKind::If)) {
        auto iexpr = std::make_unique<IfExpr>();
        iexpr->cond = parseExprBeforeBlock();
        iexpr->thenExpr = std::make_unique<BlockExpr>(parseBlock());
        if (match(TokenKind::Else)) {
            if (check(TokenKind::If)) {
                auto innerIf = std::make_unique<IfExpr>();
                advance(); // consume 'if'
                innerIf->cond = parseExprBeforeBlock();
                innerIf->thenExpr = std::make_unique<BlockExpr>(parseBlock());
                if (match(TokenKind::Else)) {
                    if (check(TokenKind::If)) {
                        // nested if-else-if: simplify to else block
                        innerIf->elseExpr = std::make_unique<BlockExpr>(parseBlock());
                    } else {
                        innerIf->elseExpr = std::make_unique<BlockExpr>(parseBlock());
                    }
                } else {
                    innerIf->elseExpr = std::make_unique<BlockExpr>(std::make_unique<BlockStmt>());
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
    const bool isHeapAllocation = match(TokenKind::New);
    if (isHeapAllocation) {
        auto type = parseType();
        consume(TokenKind::LParen, "Expected '(' after type in new expression");
        auto alloc = std::make_unique<HeapAllocExpr>();
        alloc->storage = HeapStorageKind::Unique;
        alloc->allocatedTypeAST = std::move(type);
        auto init = std::make_unique<CallExpr>();
        // Build a synthetic call: new T(args) → call T(args)
        auto* allocatedNamed = dynamic_cast<NamedTypeAST*>(alloc->allocatedTypeAST.get());
        auto calleeType =
            std::make_unique<IdentifierExpr>(allocatedNamed ? allocatedNamed->name : "?");
        init->callee = std::move(calleeType);
        if (!check(TokenKind::RParen)) { init->args = parseArgs(); }
        consume(TokenKind::RParen, "Expected ')' after new arguments");
        alloc->initializer = std::move(init);
        return alloc;
    }
    if (match(TokenKind::LParen)) {
        if (++mNestingDepth > kMaxParseNestingDepth) {
            addError("expression nesting exceeds the parser depth limit");
            --mNestingDepth;
            return std::make_unique<IntLiteralExpr>(0);
        }
        auto expr = parseExpr();
        --mNestingDepth;
        consume(TokenKind::RParen, "Expected ')' after grouped expression");
        return expr;
    }
    if (match(TokenKind::LBracket)) {
        const Token start = mTokens[mPos - 1];
        auto array = std::make_unique<ArrayLiteralExpr>();
        array->sourcePath = mSourceName;
        array->line = start.line;
        array->col = start.col;
        if (!check(TokenKind::RBracket)) {
            do {
                array->elements.push_back(parseExpr());
            } while (match(TokenKind::Comma));
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
    advance();                                  // skip bad token to prevent infinite loop
    return std::make_unique<IntLiteralExpr>(0); // error recovery
}

std::unique_ptr<RecordLiteralExpr> Parser::parseRecordLiteral(std::unique_ptr<TypeAST> targetType) {
    const Token start = consume(TokenKind::LBrace, "Expected '{' for record literal");
    auto record = std::make_unique<RecordLiteralExpr>();
    record->sourcePath = mSourceName;
    record->line = start.line;
    record->col = start.col;
    record->targetType = std::move(targetType);
    std::unordered_set<std::string> names;
    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        if (!match(TokenKind::Identifier)) {
            addError("Expected field name in record literal");
            break;
        }
        const Token fieldToken = mTokens[mPos - 1];
        RecordLiteralExpr::Field field;
        field.name = fieldToken.lexeme;
        field.sourcePath = mSourceName;
        field.line = fieldToken.line;
        field.col = fieldToken.col;
        if (!names.insert(field.name).second)
            addError("duplicate record field '" + field.name + "'");
        consume(TokenKind::Colon, "Expected ':' after record field name");
        field.value = parseExpr();
        record->fields.push_back(std::move(field));
        if (!match(TokenKind::Comma)) break;
    }
    consume(TokenKind::RBrace, "Expected '}' after record literal");
    return record;
}

std::unique_ptr<SelectExpr> Parser::parseSelectExpr(bool selectAlreadyConsumed) {
    (void)selectAlreadyConsumed;
    const Token start = mTokens[mPos - 1];
    auto selection = std::make_unique<SelectExpr>();
    selection->sourcePath = mSourceName;
    selection->line = start.line;
    selection->col = start.col;
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
    launch->sourcePath = mSourceName;
    launch->line = start.line;
    launch->col = start.col;
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

    if (match(TokenKind::Arrow)) { lambda->returnType = parseType(); }

    const auto savedDefault = mUsageDefault;
    mUsageDefault = luna::ownership::Usage::Copy;
    lambda->body = parseBlock();
    mUsageDefault = savedDefault;
    return lambda;
}

std::unique_ptr<Expr> Parser::parseIfExpr() {
    auto expr = std::make_unique<IfExpr>();
    expr->cond = parseExprBeforeBlock();
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
