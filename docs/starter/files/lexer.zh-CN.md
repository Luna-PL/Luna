> Document category: implementation note
> Applies to: Luna 0.3.0 development
> Status: Implemented Experimental
> Normative status: non-normative
> 语言：中文权威版（英文版为占位）

# src/lexer/ —— 目录逐文件指南

本指南合并了 src/lexer/ 下全部源码文件的五段式导读，方便你一次读完整个目录。

---
title: lexer/Lexer.cpp
lang: zh-CN
source: src/lexer/Lexer.cpp
---

# src/lexer/Lexer.cpp

手写词法扫描器：把源码字符流切成标识符/关键字、数字、字符串、运算符等 token。

## 这个文件做什么

`tokenize()` 是主入口：每轮记录起始位置，跳过空白和 `//` 注释，再按首字符分派。

字母/下划线 → `readIdentifierOrKeyword`：收集后查 `KEYWORDS` 表定关键字与否。

数字 → `readNumber`：整数，遇 `.`+数字转 `FloatLiteral`。

引号 → `readString`：转义处理，未闭合报错。

其它 → `readOperator`：`match(c)` 最长匹配，覆盖 `::`、`->`、`==`、`&&`、`<<=` 等。

末尾补 `TokenKind::EndOfFile`。

C++ 类比：手写递归下降/状态机分词器（按字符类别 switch），错误统一进 `Diagnostic` 列表。

## 关键结构体·类·枚举

类 `Lexer` 成员：`mSource`、`mSourceName`、`mPos`、`mLine`、`mCol`、`mStartLine`、`mStartCol`、`mErrors`。

无局部 struct/enum；`Token`/`TokenKind` 来自 `Token.h`。

## 关键函数·方法

`tokenize()`：公开入口，返回 `vector<Token>`。

私有读取器：`skipWhitespace`、`skipComment`、`readIdentifierOrKeyword`、`readNumber`、`readString`、`readOperator`。

底层字符操作：`peek`、`advance`、`isAtEnd`、`match`；诊断：`addError`、`makeError`、`sourceLineAt`。

## 与周边文件·阶段的关系

阶段：lexing → parsing → …。产物 `vector<Token>` 交给 `parser/Parser`。

错误经 `diagnostic::format`（`../diagnostics/Diagnostic.h`）记录。

## 延伸阅读

`Lexer.h`（接口）、`Token.h`（token 种类）、`Parser.h/.cpp`（下游）、`Diagnostic.h`。



---

---
title: lexer/Lexer.h
lang: zh-CN
source: src/lexer/Lexer.h
---

# src/lexer/Lexer.h

词法分析器接口声明：`Lexer` 类把源码切成 token 并收集错误。

## 这个文件做什么

公开 `Lexer` 的最小接口：构造（源码+源名）、`tokenize()`、`errors()`。实现体放在 `Lexer.cpp`。

## 关键结构体·类·枚举

- `class Lexer`：入口方法 `tokenize()`（返回 `vector<Token>`）、错误查询 `errors()`。

- 私有成员：`mSource`、`mSourceName`、行/列/起始行列、`mErrors`。

- 私有辅助声明：`skipWhitespace`/`skipComment`/`readIdentifierOrKeyword`/`readNumber`/`readString`/`readOperator`/`peek`/`advance`/`isAtEnd`/`match`/`addError`/`makeError`/`sourceLineAt`。

## 关键函数·方法

- `explicit Lexer(std::string source, std::string sourceName)`；

- `std::vector<Token> tokenize()`；

- `const std::vector<diagnostic::Diagnostic>& errors()`。

## 与周边文件·阶段关系

是编译流水线最前端的接口（lex 阶段）。token 由 `Parser` 消费；错误类型来自 `diagnostics/Diagnostic.h`。

## 延伸阅读

`Lexer.cpp`、`Token.h`、`Parser.h`、`Diagnostic.h`。



---

---
title: lexer/Token.h
lang: zh-CN
source: src/lexer/Token.h
---

# src/lexer/Token.h

定义 Luna 的所有 token 种类、token 结构体、关键字映射表与可读名称。

## 这个文件做什么

`TokenKind` 枚举列出全部词法类别：关键字、内建类型关键字（`i32` 等）、字面量、运算/赋值符、比较/位运算、括号分隔符、EOF/Error。

`Token` 结构体携带：`kind`、`lexeme`(原文)、`line`、`col`，供 parser 用。

`KEYWORDS`：字符串→`TokenKind` 的 `unordered_map`，lexer 用。

`tokenKindName(kind)`：返回可读字符串（如 `fn`、`+`、`EOF`）。

## 关键结构体·类·枚举

- `enum class TokenKind { ... }`：规模大，内含各关键字与运算符。

- `struct Token { TokenKind kind; std::string lexeme; int line; int col; }`。

- `const std::unordered_map<std::string, TokenKind> KEYWORDS`。

- `inline std::string tokenKindName(TokenKind k)`。

## 关键函数·方法

- `tokenKindName(...)`：转可读名。

- `Token` 构造函数（聚合初始化）。

## 与周边文件·阶段关系

`Lexer.cpp/.h` 产出 `Token`；`Parser` 消费。关键字表同时决定语言保留字集合。

## 延伸阅读

`Lexer.cpp/.h`、`Parser.cpp/.h`。



---
