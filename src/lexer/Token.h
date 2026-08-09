#pragma once

#include <string>
#include <unordered_map>

enum class TokenKind {
    // Keywords
    Fn, Let, Const, Constexpr, New, Move, Borrow, Copy, Affine, Linear, Mut, Free, Extern, Auto, Return,
    Fragment, Interceptor, Context, Many, Slot, Resume, Abort, Apply, Default,
    Meta, Constraint, Select, With, Runtime, Dynamic, Kernel, Launch, Await,
    Trait, Impl, Where, Struct, Enum, Package, Module, Using, As, Export, If, Else, Match, While, For,
    True, False, Self,
    // Built-in types (parsed as keywords for type annotations)
    TyI32, TyI64, TyF32, TyF64, TyBool, TyString,
    // Identifiers & literals
    Identifier, IntLiteral, FloatLiteral, StringLiteral,
    // Operators
    Plus, Minus, Star, Slash, Percent,
    Eq, PlusEq, MinusEq, StarEq, SlashEq, PercentEq,
    AndEq, OrEq, XorEq, ShiftLeftEq, ShiftRightEq,
    EqEq, Neq, Lt, LtEq, Gt, GtEq, ShiftLeft, ShiftRight,
    AndAnd, OrOr, BitOr, BitXor, Not, Tilde,
    Arrow, FatArrow, Colon, SemiColon, ColonColon, Dot, DotDot, Ampersand, At, Question,
    // Delimiters
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Less, Greater,
    // Special
    EndOfFile, Error
};

struct Token {
    TokenKind kind;
    std::string lexeme;
    int line;
    int col;

    Token(TokenKind k, std::string l, int ln, int c)
        : kind(k), lexeme(std::move(l)), line(ln), col(c) {}
};

const std::unordered_map<std::string, TokenKind> KEYWORDS = {
    {"fn",     TokenKind::Fn},     {"let",    TokenKind::Let},
    {"const",  TokenKind::Const},  {"constexpr", TokenKind::Constexpr},
    {"fragment", TokenKind::Fragment}, {"slot", TokenKind::Slot},
    {"interceptor", TokenKind::Interceptor}, {"context", TokenKind::Context},
    {"many", TokenKind::Many},
    {"resume", TokenKind::Resume}, {"apply", TokenKind::Apply},
    {"abort", TokenKind::Abort},
    {"default", TokenKind::Default},
    {"meta", TokenKind::Meta}, {"constraint", TokenKind::Constraint},
    {"select", TokenKind::Select},
    {"with", TokenKind::With}, {"runtime", TokenKind::Runtime},
    {"dynamic", TokenKind::Dynamic},
    {"kernel", TokenKind::Kernel}, {"launch", TokenKind::Launch},
    {"await", TokenKind::Await},
    {"new",    TokenKind::New},    {"move",   TokenKind::Move},
    {"borrow", TokenKind::Borrow}, {"copy", TokenKind::Copy},
    {"affine", TokenKind::Affine},
    {"linear", TokenKind::Linear},
    {"mut",    TokenKind::Mut},    {"free",   TokenKind::Free},
    {"extern", TokenKind::Extern}, {"auto",   TokenKind::Auto},
    {"return", TokenKind::Return}, {"trait",  TokenKind::Trait},
    {"impl",   TokenKind::Impl},   {"where",  TokenKind::Where},
    {"struct", TokenKind::Struct}, {"enum",   TokenKind::Enum},
    {"package", TokenKind::Package}, {"module", TokenKind::Module},
    {"using", TokenKind::Using}, {"as", TokenKind::As}, {"export", TokenKind::Export},
    {"if",     TokenKind::If},
    {"else",   TokenKind::Else},   {"match",  TokenKind::Match},
    {"while",  TokenKind::While},
    {"for",    TokenKind::For},    {"true",   TokenKind::True},
    {"false",  TokenKind::False},  {"Self",   TokenKind::Self},
    {"i32",    TokenKind::TyI32},  {"i64",    TokenKind::TyI64},
    {"f32",    TokenKind::TyF32},  {"f64",    TokenKind::TyF64},
    {"bool",   TokenKind::TyBool}, {"string", TokenKind::TyString},
};

inline std::string tokenKindName(TokenKind k) {
    switch (k) {
        case TokenKind::Fn: return "fn";
        case TokenKind::Let: return "let";
        case TokenKind::Const: return "const";
        case TokenKind::Constexpr: return "constexpr";
        case TokenKind::Fragment: return "fragment";
        case TokenKind::Interceptor: return "interceptor";
        case TokenKind::Context: return "context";
        case TokenKind::Many: return "many";
        case TokenKind::Slot: return "slot";
        case TokenKind::Resume: return "resume";
        case TokenKind::Abort: return "abort";
        case TokenKind::Apply: return "apply";
        case TokenKind::Default: return "default";
        case TokenKind::Meta: return "meta";
        case TokenKind::Constraint: return "constraint";
        case TokenKind::Select: return "select";
        case TokenKind::With: return "with";
        case TokenKind::Runtime: return "runtime";
        case TokenKind::Dynamic: return "dynamic";
        case TokenKind::Kernel: return "kernel";
        case TokenKind::Launch: return "launch";
        case TokenKind::Await: return "await";
        case TokenKind::New: return "new";
        case TokenKind::Move: return "move";
        case TokenKind::Borrow: return "borrow";
        case TokenKind::Copy: return "copy";
        case TokenKind::Affine: return "affine";
        case TokenKind::Linear: return "linear";
        case TokenKind::Mut: return "mut";
        case TokenKind::Free: return "free";
        case TokenKind::Extern: return "extern";
        case TokenKind::Auto: return "auto";
        case TokenKind::Return: return "return";
        case TokenKind::Trait: return "trait";
        case TokenKind::Impl: return "impl";
        case TokenKind::Where: return "where";
        case TokenKind::Struct: return "struct";
        case TokenKind::Enum: return "enum";
        case TokenKind::Package: return "package";
        case TokenKind::Module: return "module";
        case TokenKind::Using: return "using";
        case TokenKind::As: return "as";
        case TokenKind::Export: return "export";
        case TokenKind::If: return "if";
        case TokenKind::Else: return "else";
        case TokenKind::Match: return "match";
        case TokenKind::While: return "while";
        case TokenKind::For: return "for";
        case TokenKind::True: return "true";
        case TokenKind::False: return "false";
        case TokenKind::Self: return "Self";
        case TokenKind::TyI32: return "i32";
        case TokenKind::TyI64: return "i64";
        case TokenKind::TyF32: return "f32";
        case TokenKind::TyF64: return "f64";
        case TokenKind::TyBool: return "bool";
        case TokenKind::TyString: return "string";
        case TokenKind::Identifier: return "identifier";
        case TokenKind::IntLiteral: return "int literal";
        case TokenKind::FloatLiteral: return "float literal";
        case TokenKind::StringLiteral: return "string literal";
        case TokenKind::Plus: return "+";
        case TokenKind::Minus: return "-";
        case TokenKind::Star: return "*";
        case TokenKind::Slash: return "/";
        case TokenKind::Percent: return "%";
        case TokenKind::Eq: return "=";
        case TokenKind::PlusEq: return "+=";
        case TokenKind::MinusEq: return "-=";
        case TokenKind::StarEq: return "*=";
        case TokenKind::SlashEq: return "/=";
        case TokenKind::PercentEq: return "%=";
        case TokenKind::AndEq: return "&=";
        case TokenKind::OrEq: return "|=";
        case TokenKind::XorEq: return "^=";
        case TokenKind::ShiftLeftEq: return "<<=";
        case TokenKind::ShiftRightEq: return ">>=";
        case TokenKind::EqEq: return "==";
        case TokenKind::Neq: return "!=";
        case TokenKind::Lt: return "<";
        case TokenKind::LtEq: return "<=";
        case TokenKind::Gt: return ">";
        case TokenKind::GtEq: return ">=";
        case TokenKind::ShiftLeft: return "<<";
        case TokenKind::ShiftRight: return ">>";
        case TokenKind::AndAnd: return "&&";
        case TokenKind::OrOr: return "||";
        case TokenKind::BitOr: return "|";
        case TokenKind::BitXor: return "^";
        case TokenKind::Not: return "!";
        case TokenKind::Tilde: return "~";
        case TokenKind::Arrow: return "->";
        case TokenKind::FatArrow: return "=>";
        case TokenKind::Colon: return ":";
        case TokenKind::SemiColon: return ";";
        case TokenKind::ColonColon: return "::";
        case TokenKind::Dot: return ".";
        case TokenKind::DotDot: return "..";
        case TokenKind::Ampersand: return "&";
        case TokenKind::At: return "@";
        case TokenKind::Question: return "?";
        case TokenKind::LParen: return "(";
        case TokenKind::RParen: return ")";
        case TokenKind::LBrace: return "{";
        case TokenKind::RBrace: return "}";
        case TokenKind::LBracket: return "[";
        case TokenKind::RBracket: return "]";
        case TokenKind::Comma: return ",";
        case TokenKind::Less: return "<";
        case TokenKind::Greater: return ">";
        case TokenKind::EndOfFile: return "EOF";
        case TokenKind::Error: return "<error>";
    }
    return "???";
}
