> Document category: implementation note
> Applies to: Luna 0.3.0 development
> Status: Implemented Experimental
> Normative status: non-normative
> 语言：中文权威版（英文版为占位）

# src/parser/ —— 目录逐文件指南

本指南合并了 src/parser/ 下全部源码文件的五段式导读，方便你一次读完整个目录。

---
title: parser/AST.h
lang: zh-CN
source: src/parser/AST.h
---

# src/parser/AST.h

抽象语法树（AST）节点定义：语句、表达式、类型、声明等全部数据结构。

## 这个文件做什么

为语法分析器产出的一整棵 AST 定义数据类型，同时被后续语义分析（sema）读取/填充。几乎所有解析器返回类型都在这里。

根节点是 `Program`（含 package 声明、module 路径、`declarations` 数组）。

## 关键结构体·类·枚举

- 基础：`ASTNode`（基类，带 `sourcePath`/`line`/`col`）。

- 类型节点：`TypeAST` 及派生 `NamedTypeAST`/`RefTypeAST`/`LinearTypeAST`/`AffineTypeAST`/`FunctionTypeAST`/`RecordTypeAST`。

- 语句：`Stmt` 及派生 `BlockStmt`/`LetStmt`/`ReturnStmt`/`ExprStmt`/`IfStmt`/`MatchStmt`/`WhileStmt`/`ForStmt`/`FreeStmt`/`SlotDeclStmt`/`SlotInvokeStmt`/`ResumeStmt`/`AbortStmt`/`AwaitStmt`/`ApplyStmt`。另有 `CleanupObligation`/`MatchArm`。

- 表达式：`Expr` 及派生 `IntLiteralExpr`/`FloatLiteralExpr`/`StringLiteralExpr`/`BoolLiteralExpr`/`IdentifierExpr`/`BinaryExpr`/`UnaryExpr`/`CallExpr`/`LaunchExpr`/`VariantConstructExpr`/`FieldAccessExpr`/`IndexExpr`/`ArrayLiteralExpr`/`RecordLiteralExpr`/`HeapAllocExpr`/`TryExpr`/`MoveExpr`/`BorrowExpr`/`DerefExpr`/`AddrOfExpr`/`BlockExpr`/`IfExpr`/`LambdaExpr`/`AssignExpr`/`SelectExpr` 等。

- 模式：`Pattern`/`IdentPattern`。

- 声明：`Decl`（基类，含导出/包/元数据等）及 `FunctionDecl`/`FragmentDecl`/`StructDecl`/`EnumDecl`/`TraitDecl`/`ImplDecl`/`MetaDecl`/`ConstraintDecl`；`Param`、`WhereClause`、`TraitRef`、`Program`。

- 枚举：`FragmentKind`（Interceptor/Context）、`FragmentCardinality`（Once/Many）、`RetentionKind`（CompileTime/Runtime/Dynamic）、别名 `MetadataConstValue`。

## 关键函数·方法

几乎都是数据节点（构造少量带参数）。这些类型主要被 `Parser.cpp` 构造、被 sema/ownership/backend 遍历填充（如 `resolvedType`/`inferredType` 字段）。

## 与周边文件或阶段关系

parser 产出、sema（`sem/`）填充类型/符号、ownership 填所有权信息、codegen 消费。`LetStmt`/`ForStmt`/`ForStmt` 等携带所有权合约与迭代器协议 witnesses，供后端用。

## 延伸阅读

`Parser.cpp`、`core/Ownership.h`、`core/TypeSystem.h`、`core/TypeSystem.h`、`lexer/Token.h` 与相关枚举（`IteratorOp` 等）。

## 注意

本文件即一个大型公共类型头文件，是前后端之间的协议层。



---

---
title: parser/Parser.cpp
lang: zh-CN
source: src/parser/Parser.cpp
---

# src/parser/Parser.cpp

递归下降语法分析器实现：token 流 → `Program`，含错误恢复。

## 这个文件做什么

实现 `Parser.h` 里声明的全部解析函数。整体是经典递归下降 + 优先级爬升（precedence climbing）的组合，另外为嵌套深度设了上限。

顶层 `parse()`：先可解析 `package`/`module`/多个 `using` 头，再循环 `parseDeclaration()`，失败则 `synchronizeDeclaration()` 跳到下一个声明符/分号以继续收集错误。

## 关键结构体·枚举

- 常量 `kMaxParseNestingDepth = 256`（括号深度上限）。

- 用到 `astNested`：`Program`、`Decl`、`Stmt`/`Expr`/`TypeAST`、`Param`、`WhereClause`、`MatchArm` 等（见 `AST.h`）。



## 关键结构体·类·枚举

解析器本身无新定义数据类型，全部依赖 `AST.h` 中的节点类型：`Program`、`Decl`、`Stmt`、`Expr`、`TypeAST`、`Param`、`WhereClause`、`MatchArm` 等。

## 关键函数·方法

- 声明解析：`parseDeclaration`（初始处理 `export`/`extern`/`kernel`/`constexpr`/`runtime`/`dynamic`/`@metadata` 修饰与 ABI 字符串）、`parseFunctionDecl`、`parseStructDecl`、`parseEnumDecl`、`parseTraitDecl`、`parseImplDecl`、`parseFragmentDecl`、`parseMetaDecl`、`parseConstraintDecl`。

- 语句：`parseStatement`（分发 `linear/affine/copy/const let`、`let`、`return`、`if`、`match`、`while`、`for`、`free`、`slot`、`resume`、`abort`、`await`、`apply`、block，以及命名 slot 调用 `isNamedSlotInvocationStart`）。

- 表达式：优先级链 `parseExpr→parseAssignment→parseOr→parseAnd→parseBitOr→parseBitXor→parseBitAnd→parseEquality→parseComparison→parseShift→parseAddSub→parseMulDiv→parseUnary→parsePostfix→parsePrimary`；含 `parseRecordLiteral`、`parseSelectExpr`、`parseLaunchExpr`、`parseLambda`、`parseIfExpr`。

- 类型：`parseType`（处理 `linear/affine/& 引用/Self/auto/内建类型关键字/命名类型+泛型参数/解析 array<T,N> 常量长度`）、`parseRecordType`、`parseFunctionType`、`parseParams`、`parseArgs`、`parseTypeParamList`、`parseWhereClause`、`parseTraitRef`。

- 工具：`peek`/`peekAhead`/`advance`/`check`/`match`/`consume`（报错并返回 Error token）/`isAtEnd`/`sourceLineAt`/`addError`（调用 `diagnostic::format`）/`synchronizeDeclaration`/`synchronizeStatement`。

- 状态：`mPos` 是游标，`mUsageDefault` 记录当前块内所有权默认用法（供 `let`/参数继承）。

## 与周边文件·阶段的关系

阶段：lex → parse → …。消费 `lexer/Token.h` 的 token；产出 `AST.h` 的节点；错误经 `diagnostics/Diagnostic.h`。

部分复用 `core/Ownership.h`（`luna::ownership::Usage/Relation`）来给 let/参数打所有权标记。

## 延伸阅读

`Parser.h`、`AST.h`、`Lexer.cpp`、`Token.h`、`Diagnostic.h`、`core/Ownership.h`。



---

---
title: parser/Parser.h
lang: zh-CN
source: src/parser/Parser.h
---

# src/parser/Parser.h

语法分析器接口：`Parser` 类把 token 流变成 `Program`（AST 根），并收集语法错误。

## 这个文件做什么

声明递归下降解析器 `Parser`。`parse()` 返回 `std::unique_ptr<Program>`。源码/源名可选传入用于错误定位。实现体在 `Parser.cpp`。

## 关键结构体·类·枚举

- `class Parser`：主要公开：`parse()`、`errors()`。

- 私有成员：`mTokens`、`mPos`、`mNestingDepth`、`mSourceName`、`mSource`、`mErrors`、`mStopBeforeBlockBrace`、`mUsageDefault`。

- 私有方法极多，涵盖声明、语句、表达式（优先级爬升）、类型、模式、where 子句等。

## 关键函数·方法

- `parse()`：解析整个 `Program`。

- 声明：`parseDeclaration`、`parseFunctionDecl`、`parseStructDecl`、`parseEnumDecl`、`parseFragmentDecl`、`parseTraitDecl`、`parseImplDecl`、`parseMetaDecl`、`parseConstraintDecl`。

- 语句：`parseStatement`、`parseBlock`、`parseLetStmt`、`parseReturn/If/Match/While/For/Free/Slot/Resume/Abort/Await/Apply` 等。

- 表达式：优先爬升链 `parseExpr→parseAssignment→parseOr→…→parseUnary→parsePostfix→parsePrimary`，另有 `parseIfExpr/parseLambda/parseLaunchExpr/parseSelectExpr/parseRecordLiteral`。

- 类型：`parseType`、`parseRecordType`、`parseFunctionType`、`parseParams`、`parseArgs`、`parseTypeParamList`、`parseWhereClause`、`parseTraitRef`。

- 工具：`peek`/`peekAhead`/`advance`/`check`/`match`/`consume`/`isAtEnd`/`addError`/`synchronizeDeclaration`/`synchronizeStatement`/`sourceLineAt`。

## 与周边文件·阶段关系

消费 `lexer` 的 token；产出 `AST.h` 定义的节点；错误用 `diagnostics/Diagnostic.h`。阶段：lex → parse → …。

## 延伸阅读

`Parser.cpp`、`AST.h`（节点定义）、`Lexer.cpp`、`Token.h`、`Diagnostic.h`、`core/Ownership.h`（`luna::ownership::Usage`）。



---
