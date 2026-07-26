#include "Lexer.h"
#include "../diagnostics/Diagnostic.h"
#include <cctype>
#include <stdexcept>

Lexer::Lexer(std::string source, std::string sourceName)
    : mSource(std::move(source)), mSourceName(std::move(sourceName)) {}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    while (!isAtEnd()) {
        mStartLine = mLine;
        mStartCol = mCol;
        skipWhitespace();
        if (isAtEnd()) break;
        mStartLine = mLine;
        mStartCol = mCol;

        char c = peek();
        if (std::isalpha(c) || c == '_') {
            tokens.push_back(readIdentifierOrKeyword());
        } else if (std::isdigit(c)) {
            tokens.push_back(readNumber());
        } else if (c == '"') {
            tokens.push_back(readString());
        } else {
            Token tok = readOperator();
            if (tok.kind != TokenKind::EndOfFile) {
                tokens.push_back(tok);
            }
            if (tok.kind == TokenKind::Error) break;
        }
    }
    tokens.emplace_back(TokenKind::EndOfFile, "", mLine, mCol);
    return tokens;
}

void Lexer::skipWhitespace() {
    while (!isAtEnd()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r') {
            advance();
        } else if (c == '\n') {
            advance();
            mLine++;
            mCol = 1;
        } else if (c == '/' && mPos + 1 < (int)mSource.size() && mSource[mPos + 1] == '/') {
            skipComment();
        } else {
            break;
        }
    }
}

void Lexer::skipComment() {
    advance(); advance(); // skip //
    while (!isAtEnd() && peek() != '\n') advance();
}

Token Lexer::readIdentifierOrKeyword() {
    std::string lexeme;
    while (!isAtEnd() && (std::isalnum(peek()) || peek() == '_')) {
        lexeme += advance();
    }
    auto it = KEYWORDS.find(lexeme);
    TokenKind kind = (it != KEYWORDS.end()) ? it->second : TokenKind::Identifier;
    return Token(kind, lexeme, mStartLine, mStartCol);
}

Token Lexer::readNumber() {
    std::string lexeme;
    bool isFloat = false;
    while (!isAtEnd() && std::isdigit(peek())) lexeme += advance();
    if (!isAtEnd() && peek() == '.' && mPos + 1 < (int)mSource.size() && std::isdigit(mSource[mPos + 1])) {
        isFloat = true;
        lexeme += advance(); // '.'
        while (!isAtEnd() && std::isdigit(peek())) lexeme += advance();
    }
    return Token(isFloat ? TokenKind::FloatLiteral : TokenKind::IntLiteral,
                 lexeme, mStartLine, mStartCol);
}

Token Lexer::readString() {
    advance(); // skip opening "
    std::string lexeme;
    while (!isAtEnd() && peek() != '"') {
        if (peek() == '\\') {
            advance();
            if (isAtEnd()) break;
            char esc = advance();
            switch (esc) {
                case 'n': lexeme += '\n'; break;
                case 't': lexeme += '\t'; break;
                case '\\': lexeme += '\\'; break;
                case '"': lexeme += '"'; break;
                default: lexeme += esc; break;
            }
        } else {
            lexeme += advance();
        }
    }
    if (!isAtEnd()) advance(); // skip closing "
    else addError("Unterminated string literal");
    return Token(TokenKind::StringLiteral, lexeme, mStartLine, mStartCol);
}

Token Lexer::readOperator() {
    char c = advance();
    switch (c) {
        case '(': return Token(TokenKind::LParen, "(", mStartLine, mStartCol);
        case ')': return Token(TokenKind::RParen, ")", mStartLine, mStartCol);
        case '{': return Token(TokenKind::LBrace, "{", mStartLine, mStartCol);
        case '}': return Token(TokenKind::RBrace, "}", mStartLine, mStartCol);
        case '[': return Token(TokenKind::LBracket, "[", mStartLine, mStartCol);
        case ']': return Token(TokenKind::RBracket, "]", mStartLine, mStartCol);
        case ',': return Token(TokenKind::Comma, ",", mStartLine, mStartCol);
        case ';': return Token(TokenKind::SemiColon, ";", mStartLine, mStartCol);
        case ':': return match(':') ? Token(TokenKind::ColonColon, "::", mStartLine, mStartCol)
                                    : Token(TokenKind::Colon, ":", mStartLine, mStartCol);
        case '.': return match('.') ? Token(TokenKind::DotDot, "..", mStartLine, mStartCol)
                                    : Token(TokenKind::Dot, ".", mStartLine, mStartCol);
        case '+': return match('=') ? Token(TokenKind::PlusEq, "+=", mStartLine, mStartCol)
                                    : Token(TokenKind::Plus, "+", mStartLine, mStartCol);
        case '-': return match('>') ? Token(TokenKind::Arrow, "->", mStartLine, mStartCol)
                                    : (match('=') ? Token(TokenKind::MinusEq, "-=", mStartLine, mStartCol)
                                                   : Token(TokenKind::Minus, "-", mStartLine, mStartCol));
        case '*': return match('=') ? Token(TokenKind::StarEq, "*=", mStartLine, mStartCol)
                                    : Token(TokenKind::Star, "*", mStartLine, mStartCol);
        case '/': return match('=') ? Token(TokenKind::SlashEq, "/=", mStartLine, mStartCol)
                                    : Token(TokenKind::Slash, "/", mStartLine, mStartCol);
        case '%': return match('=') ? Token(TokenKind::PercentEq, "%=", mStartLine, mStartCol)
                                    : Token(TokenKind::Percent, "%", mStartLine, mStartCol);
        case '!': return match('=') ? Token(TokenKind::Neq, "!=", mStartLine, mStartCol)
                                    : Token(TokenKind::Not, "!", mStartLine, mStartCol);
        case '=': return match('=') ? Token(TokenKind::EqEq, "==", mStartLine, mStartCol)
                                    : Token(TokenKind::Eq, "=", mStartLine, mStartCol);
        case '&': return match('&') ? Token(TokenKind::AndAnd, "&&", mStartLine, mStartCol)
                                    : (match('=') ? Token(TokenKind::AndEq, "&=", mStartLine, mStartCol)
                                                   : Token(TokenKind::Ampersand, "&", mStartLine, mStartCol));
        case '@': return Token(TokenKind::At, "@", mStartLine, mStartCol);
        case '?': return Token(TokenKind::Question, "?", mStartLine, mStartCol);
        case '|': return match('|') ? Token(TokenKind::OrOr, "||", mStartLine, mStartCol)
                                    : (match('=') ? Token(TokenKind::OrEq, "|=", mStartLine, mStartCol)
                                                   : Token(TokenKind::BitOr, "|", mStartLine, mStartCol));
        case '^': return match('=') ? Token(TokenKind::XorEq, "^=", mStartLine, mStartCol)
                                    : Token(TokenKind::BitXor, "^", mStartLine, mStartCol);
        case '~': return Token(TokenKind::Tilde, "~", mStartLine, mStartCol);
        case '<':
            if (match('<')) return match('=') ? Token(TokenKind::ShiftLeftEq, "<<=", mStartLine, mStartCol)
                                              : Token(TokenKind::ShiftLeft, "<<", mStartLine, mStartCol);
            return match('=') ? Token(TokenKind::LtEq, "<=", mStartLine, mStartCol)
                              : Token(TokenKind::Lt, "<", mStartLine, mStartCol);
        case '>':
            if (match('>')) return match('=') ? Token(TokenKind::ShiftRightEq, ">>=", mStartLine, mStartCol)
                                              : Token(TokenKind::ShiftRight, ">>", mStartLine, mStartCol);
            return match('=') ? Token(TokenKind::GtEq, ">=", mStartLine, mStartCol)
                              : Token(TokenKind::Gt, ">", mStartLine, mStartCol);
        default:
            addError("Unexpected character: '" + std::string(1, c) + "'");
            return Token(TokenKind::Error, std::string(1, c), mStartLine, mStartCol);
    }
}

char Lexer::peek() const {
    return isAtEnd() ? '\0' : mSource[mPos];
}

char Lexer::advance() {
    mCol++;
    return mSource[mPos++];
}

bool Lexer::isAtEnd() const {
    return mPos >= (int)mSource.size();
}

bool Lexer::match(char c) {
    if (isAtEnd() || peek() != c) return false;
    advance();
    return true;
}

void Lexer::addError(const std::string& msg) {
    mErrors.push_back(diagnostic::format(
        "lex", msg, mSourceName, mStartLine, mStartCol,
        "check the highlighted character and surrounding token", sourceLineAt(mStartLine)));
}

std::string Lexer::sourceLineAt(int line) const {
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

Token Lexer::makeError(const std::string& lexeme) {
    addError("Unexpected character '" + lexeme + "'");
    return Token(TokenKind::Error, lexeme, mStartLine, mStartCol);
}
