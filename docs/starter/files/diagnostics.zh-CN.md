> Document category: implementation note
> Applies to: Luna 0.3.0 development
> Status: Implemented Experimental
> Normative status: non-normative
> 语言：中文权威版（英文版为占位）

# src/diagnostics/ —— 目录逐文件指南

本指南合并了 src/diagnostics/ 下全部源码文件的五段式导读，方便你一次读完整个目录。

---
title: diagnostics/Diagnostic.h
lang: zh-CN
source: src/diagnostics/Diagnostic.h
---

# src/diagnostics/Diagnostic.h

全编译器共享的、零外部依赖的诊断记录与渲染器：人类可读 + JSON 序列化。

## 这个文件做什么

定义 `namespace diagnostic` 下的 `Diagnostic` 结构体及一整套工具：稳定错误码 `errorCode`、人类阅读渲染 `render`、文件源行/字节定位助手、JSON 输出 `toJson`、路径规整 `normalizedPath`。所有编译阶段（lex/parse/package/semantic/ownership/codegen/driver…）用它统一报告。

## 关键结构体·类·枚举

- `struct Diagnostic`：`severity`、`phase`、`code`、`message`、`file`、`line/col`、`hint`、`sourceLine`、可选 `startByte/endByte`、`endLine/endCol`。

- 命名空间函数 + 一个重载 `operator<<(ostream&, const Diagnostic&)`。

## 关键函数·方法

- `errorCode(phase, message)`：按 phase + 关键字匹配给稳定机器码（如 `PAR0001`、`SEM0001`、`OWN0001`、`GEN9999`）。

- `format(phase, message, file, line, col, hint, sourceLine)`：构造 `Diagnostic` 并自动做字节定位。

- `render(...)`：拼出人类可读文本（`error[phase/code]: …` 加 `--> file:line:col`、源码行高亮与 `help:`）。

- `byteOffsetFromFile`/`sourceLineFromFile`/`highlightedByteLength`：按行/列在文件中定位字节与源码摘录（伪文件如 `<repl>` 无磁盘查找）。

- `toJson(...)` + `jsonEscape(...)` + `normalizedPath(...)`：机器可读 JSON 流（`luna.diagnostic` 协议）与路径规整。

- `quotedToken(lexeme)`：空 → `end of file`，否则带引号。

## 与周边文件·阶段的关系

**全项目共享**。被 `Lexer`、`Parser`、语义/所有权/代码生成各阶段与 `driver` 使用。`Driver.cpp` 依据 `message_format` 选 `render` 或 `toJson` 序列化。

## 延伸阅读

各阶段用 `phase` 名：`Driver.cpp`（`analyze`/JSON 协议）、`Lexer.cpp`（`lex`）、`Parser.cpp`（`parse`）、`AotLinker.cpp`。



---
