#pragma once

#include "Token.h"
#include "diagnostics/Diagnostic.h"
#include <vector>
#include <string>

class Lexer {
public:
    explicit Lexer(std::string source, std::string sourceName = "<input>");

    std::vector<Token> tokenize();
    const std::vector<diagnostic::Diagnostic>& errors() const { return mErrors; }

private:
    void skipWhitespace();
    void skipComment();
    Token readIdentifierOrKeyword();
    Token readNumber();
    Token readString();
    Token readOperator();
    char peek() const;
    char advance();
    bool isAtEnd() const;
    bool match(char c);
    void addError(const std::string& msg);
    Token makeError(const std::string& lexeme);
    std::string sourceLineAt(int line) const;

    std::string mSource;
    std::string mSourceName;
    int mPos = 0;
    int mLine = 1;
    int mCol = 1;
    int mStartLine = 1;
    int mStartCol = 1;
    std::vector<diagnostic::Diagnostic> mErrors;
};
