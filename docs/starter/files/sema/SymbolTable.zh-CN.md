# src/sema/SymbolTable.cpp — 符号表的实现

> 一句话定位：把 `SymbolTable.h` 声明的接口落地：作用域栈的进出、符号/类型/链接符号的读写与判重。

## 这个文件做什么

这里是 `SymbolTable` 的全部实现（约 70 行）。构造函数自动 `enterScope()` 建立包级根作用域；之后每个函数/块体进入时压一层、退出时弹一层，从而让词法作用域内声明的名字在块结束时自动失效。

C++ 类比：相当于在一段代码里用一个局部的 `std::unordered_map` 记录本块的名字，块结束 map 析构、名字消失——只是这里用手动 push/pop 控制。

## 关键结构体·类·枚举

本文件没有新增类型/枚举；所有数据结构（`SymbolKind`、`SymbolInfo`、`SymbolTable` 的三张表）都声明在 `SymbolTable.h`。

## 关键函数·方法

- 构造函数 `SymbolTable()`：调用 `enterScope()`，保证 `mScopes` 非空且 `mScopes[0]` 为根作用域。
- `enterScope()`/`exitScope()`：`emplace_back()` 压层；`exitScope` 仅在 `size()>1` 时 `pop_back()`，避免误删根作用域。
- `define(name, info)` / `defineAtRoot(name, info)`：先 `count` 判重再写入 `mScopes.back()` / `mScopes.front()`；重名返回 `false`。
- `defineLinkage(name, info)` / `lookupLinkage(name)`：读写 `mLinkageSymbols`（链接级符号，与作用域无关）。
- `lookup(name)`：从 `rbegin` 到 `rend` 遍历 `mScopes`，返回第一个命中项；找不到返回 `nullptr`。
- `lookupDepth(name)`：类似 `lookup` 但返回名字所在层深（0 起）或 `static_cast<size_t>(-1)`；闭包捕获分析依赖它区分「lambda 本地绑定 vs 外层自由变量」。
- `hasInCurrentScope(name)`：只查 `mScopes.back()`。
- `defineType`/`lookupType`：读写 `mTypeMap`（名→`TypePtr`）。
- `visibleSymbols()`：把每一层名字并入一个 `unordered_map` 快照（内层同名覆盖外层），供上层捕获分析取用。

## 与周边文件·阶段的关系

- `SymbolTable` 实例是 `SemanticContext` 的成员（`mSymTable`），由 `SemanticAnalyzer` 暴露给外部（`symTable()`）。
- 声明阶段：`DeclarationCollector` 用 `defineAtRoot`/`defineLinkage`/`defineType` 写入声明。
- 体分析阶段：`BodyAnalyzer` 在函数/块进入时 `enterScope`、退出时 `exitScope`，并用 `lookup` 绑定标识符。
- 所有权检查：`OwnershipChecker::check` 直接接收 `SymbolTable&` 以读取契约信息。

## 延伸阅读

- `SymbolTable.h`：数据结构与接口声明。
- `SemanticContext.cpp`：`SemanticContext::analyze` 中作用域/符号表的初始化与清洗。
- `DeclarationCollector.cpp` / `BodyAnalyzer.cpp`：符号表的主要写入与读取方。


---

---
kind: source-file-guide
module: sema
source: src/sema/SymbolTable.h
lang: zh-CN
audience: 学过 C/C++、想读 Luna 编译器前端的读者
---

# src/sema/SymbolTable.h — 作用域感知的「名字 → 符号」容器

> 一句话定位：`SymbolTable` 是语义分析的符号表：把名字（变量/函数/类型/片段等）绑定到对应符号，并记录其所在作用域深度，名字解析与闭包捕获分析都要靠它。

## 这个文件做什么

Luna 语义分析需要回答「`foo` 这个名字指什么」。`SymbolTable` 提供按词法作用域分层的名字→`SymbolInfo` 表：

- `SymbolInfo` 缓存每个符号在语义各阶段算出的全部信息（类型、所有权 contract、是否为泛型模板等）。
- `SymbolTable` 用作用域栈 + 全局类型表 + 链接符号表承载查找。

C++ 类比：相当于一本「作用域限定的符号字典」——类似定义在一个函数内局部、离开即失效的一张哈希表，但还惦记着「在哪层定义的」以支持词法作用域。

## 关键结构体·类·枚举

- `enum class SymbolKind`：`Variable, Function, Fragment, Slot, Struct, Trait, TypeParam, Metadata`（符号种类）。
- `struct SymbolInfo`：一个符号的完整语义摘要：
  - `kind`：符号种类；`type`/`paramTypes`/`returnType`：类型与签名。
  - `typeParams`：泛型参数名序列；`genericDecl`：泛型模板原始声明（供特化）。
  - `isLinear`/`usage`/`relation`：所有权契约（`luna::ownership::Usage/Relation`）。
  - `isConst`/`isExported`/`isExtern`/`returnsLinear`/`returnUsage`/`paramContracts` 等。
  - `compileTimeDeclarationId`：前端专用、MIR 前抹掉的 `declaration_ref` 值。
- `class SymbolTable`：
  - `mScopes`：作用域栈（`vector<unordered_map<string, SymbolInfo>>`），`mScopes[0]` 是包级根作用域。
  - `mTypeMap`：全局「类型/特征注册表」（名→`TypePtr`）。
  - `mLinkageSymbols`：链接级符号（`defineLinkage`/`lookupLinkage`）。

## 关键函数·方法

- `enterScope()`/`exitScope()`：压入或弹出一层作用域，天然实现「块结束、名字失效」。
- `define`/`defineAtRoot`：在当前最内层/根作用域定义一个符号；重名返回 `false`。
- `defineLinkage`/`lookupLinkage`：`mLinkageSymbols` 上的读写（针对已定链接名的声明）。
- `lookup(name)`、`lookupDepth(name)`、`lookupLinkage(name)`：从最内到外查普通/链接符号；`lookupDepth` 返回名字所在层深（从 0 计）或 `SIZE_MAX`，供闭包捕获分析判断是否为 lambda 体外层自由变量。
- `hasInCurrentScope`：仅看最内层；`visibleSymbols()`：把各层名字并成一个快照（上层捕获时用）。
- `defineType`/`lookupType`：操作 `mTypeMap`。

## 与周边文件·阶段的关系

- 是 `SemanticContext` 的成员（`mSymTable`），各分析器共享同一实例。
- 声明阶段（`DeclarationCollector`）往表里写；函数/块体阶段（`BodyAnalyzer`）进出 `enterScope` 绑定词法名字；类型经 `defineType` 注册。
- lambda 捕获分析区分「lambda 本地绑定 vs 外层自由变量」依赖 `lookupDepth`。
- OwnershipChecker 也读 `SymbolTable` 拿名字与契约。

## 延伸阅读

- `SymbolTable.cpp`（实现）、`SemanticContext.h`（运行期状态宿主）、`DeclarationCollector.h`（写入者）、`BodyAnalyzer.h`（读写主力）。


---

---
kind: source-file-guide
module: sema
source: src/sema/TraitChecker.cpp
lang: zh-CN
audience: 学过 C/C++、想读 trait 检查实现的读者
---
