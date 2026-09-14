#include "Parser.h"
#include "../diagnostics/Diagnostic.h"

#include <unordered_set>

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
    if (check(TokenKind::LBrace)) return parseRecordType();
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
    // Named type (predefined type, user type, or type parameter)
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

std::unique_ptr<RecordTypeAST> Parser::parseRecordType() {
    const Token start = consume(TokenKind::LBrace,
                                "Expected '{' for record type");
    auto record = std::make_unique<RecordTypeAST>();
    record->sourcePath = mSourceName;
    record->line = start.line;
    record->col = start.col;
    std::unordered_set<std::string> names;
    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        if (!match(TokenKind::Identifier)) {
            addError("Expected field name in record type");
            break;
        }
        const Token fieldToken = mTokens[mPos - 1];
        RecordTypeAST::Field field;
        field.name = fieldToken.lexeme;
        field.sourcePath = mSourceName;
        field.line = fieldToken.line;
        field.col = fieldToken.col;
        if (!names.insert(field.name).second)
            addError("duplicate record field '" + field.name + "'");
        consume(TokenKind::Colon, "Expected ':' after record field name");
        field.type = parseType();
        record->fields.push_back(std::move(field));
        if (!match(TokenKind::Comma)) break;
    }
    consume(TokenKind::RBrace, "Expected '}' after record type");
    return record;
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
        if (dynamic_cast<LinearTypeAST*>(p.type.get()) != nullptr) {
            p.isLinear = true;
            p.usage = luna::ownership::Usage::Linear;
            p.hasExplicitUsage = true;
        } else if (dynamic_cast<AffineTypeAST*>(p.type.get()) != nullptr) {
            p.usage = luna::ownership::Usage::Affine;
            p.hasExplicitUsage = true;
        }
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

std::vector<std::string> Parser::parseTypeParamList(
    std::vector<WhereClause>* constrainedParameters) {
    std::vector<std::string> params;
    if (match(TokenKind::Lt)) {
        do {
            if (!match(TokenKind::Identifier)) {
                addError("Expected type parameter name");
                break;
            }
            std::string first = mTokens[mPos - 1].lexeme;
            parseQualifiedNameTail(first);
            if (check(TokenKind::Identifier)) {
                const Token parameter = advance();
                params.push_back(parameter.lexeme);
                if (!constrainedParameters) {
                    addError("constrained type parameters are only valid on functions");
                    continue;
                }
                WhereClause clause;
                clause.kind = WhereClause::Kind::Constraint;
                clause.constraintName = std::move(first);
                auto argument = std::make_unique<NamedTypeAST>(parameter.lexeme);
                argument->sourcePath = mSourceName;
                argument->line = parameter.line;
                argument->col = parameter.col;
                clause.constraintTypeArgs.push_back(std::move(argument));
                constrainedParameters->push_back(std::move(clause));
            } else {
                if (first.find("::") != std::string::npos)
                    addError("A type parameter name cannot be qualified");
                params.push_back(std::move(first));
            }
        } while (match(TokenKind::Comma));
        consume(TokenKind::Gt, "Expected '>' after type parameters");
    }
    return params;
}

std::vector<WhereClause> Parser::parseWhereClause() {
    std::vector<WhereClause> clauses;
    if (match(TokenKind::Where)) {
        do {
            WhereClause clause;
            if (check(TokenKind::Identifier) &&
                peekAhead(1).kind == TokenKind::Colon &&
                peekAhead(2).kind == TokenKind::Identifier) {
                const Token first = advance();
                advance(); // ':'
                clause.kind = WhereClause::Kind::TraitBound;
                clause.typeParam = first.lexeme;
                if (!match(TokenKind::Identifier)) {
                    addError("Expected trait name in where clause");
                    break;
                }
                clause.trait = parseTraitRef(mTokens[mPos - 1]);
            } else {
                int cursor = mPos;
                bool namedConstraint = cursor < static_cast<int>(mTokens.size()) &&
                    mTokens[cursor].kind == TokenKind::Identifier;
                if (namedConstraint) {
                    ++cursor;
                    while (cursor + 1 < static_cast<int>(mTokens.size()) &&
                           mTokens[cursor].kind == TokenKind::ColonColon &&
                           mTokens[cursor + 1].kind == TokenKind::Identifier)
                        cursor += 2;
                    namedConstraint = cursor < static_cast<int>(mTokens.size()) &&
                        mTokens[cursor].kind == TokenKind::Lt;
                }
                if (!namedConstraint) {
                    clause.kind = WhereClause::Kind::ConstraintExpression;
                    clause.constraintExpression = parseExpr();
                    clauses.push_back(std::move(clause));
                    continue;
                }
                clause.kind = WhereClause::Kind::Constraint;
                advance();
                clause.constraintName = mTokens[mPos - 1].lexeme;
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
    if (match(TokenKind::Lt)) {
        if (!check(TokenKind::Gt)) {
            do {
                trait.typeArgs.push_back(parseType());
            } while (match(TokenKind::Comma));
        }
        consume(TokenKind::Gt, "Expected '>' after trait type arguments");
    }
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
    if (!check(TokenKind::Identifier)) return false;
    int open = mPos + 1;
    while (open + 1 < static_cast<int>(mTokens.size()) &&
           mTokens[open].kind == TokenKind::ColonColon &&
           mTokens[open + 1].kind == TokenKind::Identifier)
        open += 2;
    if (open >= static_cast<int>(mTokens.size()) ||
        mTokens[open].kind != TokenKind::LParen) return false;
    int depth = 0;
    for (int i = open; i < static_cast<int>(mTokens.size()); ++i) {
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
