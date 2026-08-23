> Document category: implementation note
> Applies to: Luna 0.3.0 development
> Status: Implemented Experimental
> Normative status: non-normative
> 语言：中文权威版（英文版为占位）

# src/tooling/ —— 目录逐文件指南

本指南合并了 src/tooling/ 下全部源码文件的五段式导读，方便你一次读完整个目录。

---
title: AnalysisSnapshot.cpp
source: src/tooling/AnalysisSnapshot.cpp
language: zh-CN
audience: C++ tooling developers
---

# src/tooling/AnalysisSnapshot.cpp

实现 AnalysisSnapshot 的静态工厂方法与私有辅助函数，串联前端编译管线各阶段。

## 这个文件做什么

实现 `AnalysisSnapshot.h` 中声明的全部方法，包括四个静态工厂函数、析构/移动语义函数、私有 `analyzeProgram` 和 `fail` 辅助方法，以及匿名命名空间中的 `assignSourceOwnership` 工具函数。

## 关键结构体·类·枚举

（本文件未定义新的类或枚举，仅在匿名命名空间中定义了一个自由函数。）

## 关键函数·方法

| 函数/方法 | 说明 |
|-----------|------|
| `assignSourceOwnership(Program&)`（匿名 namespace） | 遍历 `Program::declarations`，为缺失 packageId 的声明补上 `program.packageName`；对 `ImplDecl` 将其 packageId 和 modulePath 传播到每个 method；为 `packageUses` 补 ownerPackageId |
| `analyzePath(inputPath)` | 创建 `PackageManager`，构造 `PackageRequest`，调用 `manager.load` 加载包；成功后调用 `analyzeProgram` |
| `analyzePathWithOverlay` | 委托给 `analyzePathWithOverlays`，参数为单个 overlay |
| `analyzePathWithOverlays` | 与 `analyzePath` 类似，但 `request.overlays` 携带虚拟文件覆盖 |
| `analyzeSource(source, documentId)` | 无需 `PackageManager`，直接 Lexer → Parser → `assignSourceOwnership` → `analyzeProgram` |
| `analyzeProgram()` | 私有方法：依次执行 `SemanticAnalyzer::analyze`、`TraitChecker::check`、`OwnershipChecker::check`，最后构建 `SymbolIndex` 和 `ReferenceIndex`；任一阶段失败则调用 `fail` |
| `fail(errors, stage)` | 私有方法：即使失败也尽可能构建部分索引，设置 `mSuccess = false` 并记录错误 |
| `symbolTable()` | const 访问器，通过 `mSemanticAnalyzer->symTable()` 返回 `SymbolTable` 指针 |

## 与周边文件·阶段的关系

- 调用 `Lexer`（`lexer/Lexer.h`）和 `Parser`（`parser/Parser.h`）处理单源模式
- 调用 `PackageManager`（`package/PackageManager.h`）处理包加载
- 调用 `SemanticAnalyzer`、`TraitChecker`、`OwnershipChecker` 执行完整前端分析管线
- 输出 `SymbolIndex` 和 `ReferenceIndex` 供 tooling 消费者使用

## 延伸阅读

- `AnalysisSnapshot.h` — 类声明与公有接口
- `SymbolIndex.cpp` — 符号索引构建算法细节
- `ReferenceIndex.cpp` — 引用索引构建算法细节


---

---
title: AnalysisSnapshot.h
source: src/tooling/AnalysisSnapshot.h
language: zh-CN
audience: C++ tooling developers
---

# src/tooling/AnalysisSnapshot.h

AnalysisSnapshot 是前端编译结果的只读快照，供 LSP、代码导航等 tooling 模块消费，不暴露编译器的可变中间状态。

## 这个文件做什么

声明 `AnalysisSnapshot` 类，它将一次完整的编译流程（加载、解析、语义分析、Trait 检查、所有权检查、符号索引构建、引用索引构建）的执行结果封装为一个不可变对象。调用者通过静态工厂方法获得快照后，只能读取结果，不能修改编译器内部状态。

## 关键结构体·类·枚举

- `class AnalysisSnapshot` — 核心类。移动构造/移动赋值可用，拷贝构造/拷贝赋值被删除。包含以下私有成员，大部分通过 const 访问器暴露：
  - `std::unique_ptr<Program> mProgram` — AST 根节点
  - `std::unique_ptr<SemanticAnalyzer> mSemanticAnalyzer` — 语义分析器（用于符号表与引用信息）
  - `PackageGraph mPackageGraph` — 包依赖图
  - `PackageManifest mPackageManifest` — 当前包清单
  - `std::string mPackageRootPath` — 包根目录
  - `SymbolIndex mSymbolIndex` — 符号索引
  - `ReferenceIndex mReferenceIndex` — 引用索引
  - `std::vector<diagnostic::Diagnostic> mErrors` — 错误诊断列表
  - `std::string mErrorStage` — 出错阶段标识
  - `bool mSuccess` — 编译是否成功

## 关键函数·方法

| 方法 | 说明 |
|------|------|
| `static analyzePath(inputPath)` | 从文件系统路径加载并分析整个包 |
| `static analyzePathWithOverlay(inputPath, documentPath, source)` | 加载包时用单个虚拟文件覆盖磁盘上的对应文件 |
| `static analyzePathWithOverlays(inputPath, overlays)` | 加载包时用多个虚拟文件覆盖 |
| `static analyzeSource(source, documentId)` | 仅分析单段源代码（无包依赖），直接走词法/语法分析 |
| `success() / program() / symbolTable() / packageGraph() / packageManifest() / packageRootPath() / symbolIndex() / referenceIndex() / errors() / errorStage()` | const 访问器，全部为内联实现 |

## 与周边文件·阶段的关系

- 调用 `PackageManager::load`（`package/PackageManager.h`）处理包加载阶段
- 调用 `SemanticAnalyzer::analyze`（`sema/SemanticAnalyzer.h`）执行语义分析
- 调用 `TraitChecker::check`（`sema/TraitChecker.h`）检查 Trait 约束
- 调用 `OwnershipChecker::check`（`sema/OwnershipChecker.h`）检查所有权语义
- 调用 `SymbolIndex::build`（`tooling/SymbolIndex.h`）和 `ReferenceIndex::build`（`tooling/ReferenceIndex.h`）构建索引
- 读取 `SymbolTable` 获取符号定义信息

## 延伸阅读

- `ReferenceIndex.h` / `ReferenceIndex.cpp` — 引用索引的数据结构与构建
- `SymbolIndex.h` / `SymbolIndex.cpp` — 符号索引的数据结构与构建
- `SourceManager.h` / `SourceManager.cpp` — LSP 文档管理（不直接依赖 AnalysisSnapshot）


---

---
title: ReferenceIndex.cpp
source: src/tooling/ReferenceIndex.cpp
language: zh-CN
audience: C++ tooling developers
---

# src/tooling/ReferenceIndex.cpp

实现 `ReferenceIndex::build` 工厂方法和 `inDocument` 查询方法。

## 这个文件做什么

将 `SemanticAnalyzer` 收集的原始引用（按 linkageName 定位）通过 `SymbolIndex` 的 linkageName → id 映射解析为目标符号 ID，并对结果排序去重。

## 关键结构体·类·枚举

（本文件未定义新的类或枚举。）

## 关键函数·方法

| 函数/方法 | 说明 |
|-----------|------|
| `ReferenceIndex::build(semanticAnalyzer, symbols)` | 1) 遍历 `symbols.declarations()` 建立 `linkageName → symbol.id` 的 `unordered_map`；2) 遍历 `semanticAnalyzer.declarationReferences()`，对每个引用查找目标，若找到则构造 `IndexedReference`；3) 按 (path, line, column, targetId) 排序；4) 去重（同一位置同一目标的重复引用）；5) 返回结果 |
| `ReferenceIndex::inDocument(path)` | 线性扫描 `mReferences`，筛选 `source.path == path` 的条目，返回指针向量 |

## 与周边文件·阶段的关系

- 读取 `SemanticAnalyzer::declarationReferences()`（`sema/SemanticAnalyzer.h`）
- 引用 `SymbolIndex::declarations()` 获取 linkageName 映射
- 输出被 `AnalysisSnapshot::analyzeProgram()` 消费并存入 `AnalysisSnapshot::mReferenceIndex`

## 延伸阅读

- `ReferenceIndex.h` — 数据结构声明
- `SymbolIndex.cpp` — 符号索引构建中 linkageName 的生成规则
- `AnalysisSnapshot.cpp` — 调用 `ReferenceIndex::build` 的上下文


---

---
title: ReferenceIndex.h
source: src/tooling/ReferenceIndex.h
language: zh-CN
audience: C++ tooling developers
---

# src/tooling/ReferenceIndex.h

将语义分析阶段收集的声明引用（declaration references）解析为可遍历的符号引用索引，支持按文档查询。

## 这个文件做什么

声明 `IndexedReference` 结构体和 `ReferenceIndex` 类，用于存储从语义分析器收集的符号引用位置，并按文档路径做索引。

## 关键结构体·类·枚举

- `struct IndexedReference` — 一条已解析的引用记录：
  - `std::string targetId` — 目标符号的 ID（与 `SymbolIndex` 中的 `IndexedSymbol::id` 对应）
  - `SymbolSourceLocation source` — 引用出现的位置（路径、行、列、字节长度）
- `class ReferenceIndex` — 引用索引容器：
  - 私有成员 `std::vector<IndexedReference> mReferences` — 已排序去重的引用列表
  - 对外暴露 const 访问器和按文档查询接口

## 关键函数·方法

| 方法 | 说明 |
|------|------|
| `static build(semanticAnalyzer, symbols)` | 工厂函数：遍历 `SemanticAnalyzer::declarationReferences()`，通过 `symbols` 的 linkageName → id 映射解析目标，构造并排序去重后返回 |
| `references()` | 返回 `const std::vector<IndexedReference>&`，内联实现 |
| `inDocument(path)` | 筛选出 `source.path == path` 的所有引用，返回指针向量 |

## 与周边文件·阶段的关系

- 依赖 `SymbolIndex`（`tooling/SymbolIndex.h`）提供 linkageName 到 symbol id 的映射
- 读取 `SemanticAnalyzer::declarationReferences()`（`sema/SemanticAnalyzer.h`）的原始引用数据
- 输出由 `AnalysisSnapshot` 消费，供 LSP 的"查找引用"等功能使用

## 延伸阅读

- `ReferenceIndex.cpp` — `build` 和 `inDocument` 的具体实现
- `SymbolIndex.h` — 符号 ID 的生成规则与 `IndexedSymbol` 结构
- `SemanticAnalyzer` — 原始引用数据的来源


---

---
title: SourceManager.cpp
source: src/tooling/SourceManager.cpp
language: zh-CN
audience: C++ tooling developers
---

# src/tooling/SourceManager.cpp

实现 `SourceManager.h` 中声明的 UTF-8 ↔ UTF-16 位置双向转换算法和文档生命周期管理。

## 这个文件做什么

实现 `SourceDocument` 和 `SourceManager` 的全部公有方法，以及匿名命名空间中的 UTF-8 解码辅助函数 `decodeUtf8` 和 `utf16Width`。

## 关键结构体·类·枚举

- `struct DecodedScalar`（匿名 namespace）— 解码结果：`uint32_t value`（Unicode 标量值）和 `size_t bytes`（编码字节数）

## 关键函数·方法

| 函数/方法 | 说明 |
|-----------|------|
| `decodeUtf8(text, offset, limit)`（匿名 namespace） | 从 `offset` 解码一个 UTF-8 字符：1 字节 ASCII（0x00–0x7F）、2 字节（0xC2–0xDF）、3 字节（0xE0–0xEF，含超长/代理对拒绝）、4 字节（0xF0–0xF4，含超长拒绝）；非法序列返回 U+FFFD（1 字节） |
| `utf16Width(scalar)`（匿名 namespace） | BMP 字符返回 1，增补平面字符（> 0xFFFF）返回 2（UTF-16 代理对） |
| `SourceDocument::SourceDocument` | 构造函数：扫描 `mText` 中所有 \n 构建 `mLineStarts` 向量 |
| `SourceDocument::lineContentEnd(line)` | 私有辅助：计算行内容结束位置（\n 或 \r\n 前） |
| `SourceDocument::byteOffset(position)` | 从行首开始逐字符解码，累加 UTF-16 宽度直到匹配 `position.character`，返回 UTF-8 字节偏移 |
| `SourceDocument::utf16Position(byteOffset)` | 二分查找行号，然后逐字符解码累加 UTF-16 宽度 |
| `SourceDocument::lineText(line)` | 返回 `string_view` 指向该行内容 |
| `SourceManager::open / update / close / find` | 委托到 `unordered_map` 的 emplace / 查找 / 删除操作 |

## 与周边文件·阶段的关系

- 仅依赖 `SourceManager.h` 声明
- 不依赖其他 tooling 或编译器模块
- 位置转换的 UTF-8 解码逻辑独立于编译器的词法分析器，因为 tooling 需要在源码文本上直接操作，而非 token 流

## 延伸阅读

- `SourceManager.h` — 类声明与公有接口
- `Utf16Position` 与 `SymbolSourceLocation`（`SymbolIndex.h`）的对比：后者使用 UTF-8 字节偏移，前者使用 UTF-16 代码单元
- RFC 3629 — UTF-8 标准
- Unicode 标准中关于 UTF-16 代理对的定义


---

---
title: SourceManager.h
source: src/tooling/SourceManager.h
language: zh-CN
audience: C++ tooling developers
---

# src/tooling/SourceManager.h

LSP 与 tooling 层的源码文档管理器，管理已打开文档的文本内容与 UTF-8 ↔ UTF-16 位置转换。

## 这个文件做什么

声明 `Utf16Position` 结构体、`SourceDocument` 类和 `SourceManager` 类，提供 LSP 所需的零基 UTF-16 代码单元位置与编译器内部 UTF-8 字节偏移之间的双向转换，以及文档打开/关闭/更新生命周期管理。

## 关键结构体·类·枚举

- `struct Utf16Position` — LSP 位置类型（`line`、`character`，均为 `uint32_t`），支持 `operator==`
- `class SourceDocument` — 单个文档的不可变快照：
  - `mId` / `mText` / `mVersion` — 文档标识、文本内容、版本号
  - `mLineStarts` — 每行起始字节偏移的索引，构造函数中通过扫描 \n 构建
- `class SourceManager` — 文档集合管理器：
  - 私有成员 `std::unordered_map<std::string, SourceDocument> mDocuments`，以文档 ID 为键

## 关键函数·方法

| 方法 | 说明 |
|------|------|
| `SourceDocument(id, text, version)` | 构造函数，扫描文本构建 `mLineStarts` |
| `SourceDocument::byteOffset(position)` | UTF-16 → UTF-8 字节偏移（对多字节字符按 UTF-16 代理对宽度计算） |
| `SourceDocument::utf16Position(byteOffset)` | UTF-8 字节偏移 → UTF-16 位置 |
| `SourceDocument::lineText(line)` | 返回指定行的文本（不含行终止符） |
| `SourceManager::open(id, text, version)` | 打开新文档，若已存在则返回 false |
| `SourceManager::update(id, text, version)` | 更新文档内容（版本号必须递增），否则返回 false |
| `SourceManager::close(id)` | 关闭文档，返回是否成功删除 |
| `SourceManager::find(id)` | 按 ID 查找文档，返回 `const SourceDocument*` 或 nullptr |

## 与周边文件·阶段的关系

- 不依赖其他 tooling 模块（`SymbolIndex` / `ReferenceIndex` / `AnalysisSnapshot`）
- 被 LSP 服务器实现使用，用于维护已打开文档的文本状态
- 位置转换逻辑 (`decodeUtf8`) 在 `SourceManager.cpp` 中实现，严格遵循 UTF-8 规范

## 延伸阅读

- `SourceManager.cpp` — 位置转换与文档管理的具体实现
- LSP 规范中关于 Position 的 UTF-16 定义
- `Utf16Position` 与编译器内部源位置（`SymbolSourceLocation`）的区别


---

---
title: SymbolIndex.cpp
source: src/tooling/SymbolIndex.cpp
language: zh-CN
audience: C++ tooling developers
---

# src/tooling/SymbolIndex.cpp

实现 `SymbolIndex::build` 工厂函数，将 AST 中的各种声明类型转换为统一的 `IndexedSymbol` 记录，并构建多重索引。

## 这个文件做什么

实现 `SymbolIndex.h` 中声明的全部方法，包括 `build`、`add`、`finalize`、`findById`、`findByName`、`inDocument`，以及匿名命名空间中的大量辅助函数，用于生成符号 ID、限定名、签名等。

## 关键结构体·类·枚举

（本文件未定义新的类或枚举，仅匿名命名空间中的自由函数。）

## 关键函数·方法

### 匿名 namespace 辅助函数

| 函数 | 说明 |
|------|------|
| `typeName(type)` | 将 `TypePtr` 转为字符串，空指针返回 `"?"` |
| `usagePrefix(usage)` | 将所有权用法枚举转为前缀字符串：`Copy` → 空字符串、`Affine` → `"affine "`、`Linear` → `"linear "` |
| `typeParameters(params)` | 将类型参数列表格式化为 `"<T, U>"` |
| `parameterList(params)` | 将函数参数列表格式化为 `"(name: type, ...)"`，含 usage 前缀 |
| `functionSignature(function)` | 生成完整函数签名：`fn name<params>(params) -> usage type` |
| `qualifiedName(decl, name)` | 生成 `packageId::modulePath::name` 格式的限定名 |
| `appendIdentityComponent(id, component)` | 向版本化 ID 追加 `:len:component` 段 |
| `symbolId(symbol)` | 组装 `"luna.symbol.v1"` + 各身份组件 |
| `selectionOf(decl, name)` | 从声明中提取定义位置（优先使用 nameLine/nameCol） |
| `commonSymbol(decl, name, kind, signature, external)` | 构造 `IndexedSymbol` 并填充通用字段 |
| `childSymbol(parent, parentName, name, kind, signature, selection, linkageComponent)` | 构造子符号（字段、枚举变体），继承父级包/模块路径，在 linkageName 中追加组件 |

### SymbolIndex 成员方法

| 方法 | 说明 |
|------|------|
| `SymbolIndex::build(program)` | 遍历 `program.declarations`，按 `dynamic_cast` 分派到 `FunctionDecl`（Function/Kernel）、`FragmentDecl`（Fragment）、`StructDecl`（Struct + Field 子符号）、`EnumDecl`（Enum + EnumVariant 子符号）、`TraitDecl`（Trait）、`MetaDecl`（Metadata）、`ConstraintDecl`（Constraint）、`ImplDecl`（Method），最后调用 `finalize` |
| `add(symbol)` | 将符号追加到 `mDeclarations` 向量 |
| `finalize()` | 按 (path, line, column, id) 排序；对重复 ID 的符号追加位置组件使其唯一；构建 `mById` / `mByName` / `mByDocument` 索引 |
| `findById(id)` | 哈希表 O(1) 查找 |
| `findByName(name)` | 返回匹配符号的指针向量，按 id 排序 |
| `inDocument(path)` | 返回文档中所有符号，按行/列排序 |
| `indexedSymbolKindName(kind)` | 自由函数：枚举 → 字符串映射 |

## 与周边文件·阶段的关系

- 读取 `parser/AST.h` 中的 AST 节点类型，使用 `dynamic_cast` 进行运行时类型识别
- 引用 `Program`、`FunctionDecl`、`FragmentDecl`、`StructDecl`、`EnumDecl`、`TraitDecl`、`MetaDecl`、`ConstraintDecl`、`ImplDecl` 等 AST 类型
- 输出被 `AnalysisSnapshot` 和 `ReferenceIndex` 消费
- 类型签名生成依赖 `TypePtr::toString()` 和 `luna::ownership::Usage` 枚举

## 延伸阅读

- `SymbolIndex.h` — 数据结构声明与 `IndexedSymbolKind` 枚举定义
- `ReferenceIndex.cpp` — 利用 `linkageName` 和 `id` 进行引用解析
- `AnalysisSnapshot.cpp` — 调用 `SymbolIndex::build` 的时机
- `parser/AST.h` — AST 声明节点的完整定义


---

---
title: SymbolIndex.h
source: src/tooling/SymbolIndex.h
language: zh-CN
audience: C++ tooling developers
---

# src/tooling/SymbolIndex.h

将编译器的 AST 声明（Decl）归一化为统一的符号索引，支持按 ID、名称、文档查询。

## 这个文件做什么

声明符号索引所需的枚举、结构体和 `SymbolIndex` 类，将 AST 中的函数、结构体、枚举、Trait、Fragment、元数据、约束、方法、字段、枚举变体等声明类型统一为 `IndexedSymbol` 记录，并提供多种查询索引。

## 关键结构体·类·枚举

- `enum class IndexedSymbolKind` — 符号种类枚举：`Function` / `Kernel` / `Method` / `Fragment` / `Struct` / `Enum` / `Trait` / `Metadata` / `Constraint` / `Field` / `EnumVariant`，共 11 种
- `struct SymbolSourceLocation` — 源码位置：`path`（字符串路径）、`line` / `column`（int）、`byteLength`（size_t）
- `struct IndexedSymbol` — 一条符号索引记录：
  - `id` — 版本化标识符（格式：`luna.symbol.v1:len:component:len:component:...`）
  - `name` / `qualifiedName` — 短名称和限定名称
  - `packageId` / `modulePath` — 包和模块路径
  - `linkageName` — 链接名称（用于引用索引的交叉解析）
  - `signature` — 人类可读的类型签名
  - `kind` — `IndexedSymbolKind`
  - `selection` — 定义位置（`SymbolSourceLocation`）
  - `exported` / `external` — 导出/外部标记
- `class SymbolIndex` — 符号索引容器：
  - 私有成员：`mDeclarations`（向量）、`mById`（map）、`mByName`（multimap）、`mByDocument`（multimap）

## 关键函数·方法

| 方法 | 说明 |
|------|------|
| `static build(program)` | 工厂函数：遍历 `Program::declarations`，按具体类型分派调用 `add` |
| `declarations()` | 返回 `const std::vector<IndexedSymbol>&`，内联 |
| `findById(id)` | 通过 `mById` 哈希表 O(1) 查找，返回指针或 nullptr |
| `findByName(name)` | 通过 `mByName` 查找，返回匹配的指针向量，按 id 排序 |
| `inDocument(path)` | 通过 `mByDocument` 查找，返回指针向量，按行/列排序 |
| `indexedSymbolKindName(kind)` | 自由函数：将枚举值转为字符串，如 `"function"`、`"struct"` |

## 与周边文件·阶段的关系

- 读取 `parser/AST.h` 中的 `Program` 和具体 `Decl` 子类（`FunctionDecl`、`StructDecl`、`EnumDecl` 等）
- 输出由 `ReferenceIndex`（`tooling/ReferenceIndex.h`）和 `AnalysisSnapshot` 消费
- 不会编辑阶段之前执行，但即使语义分析失败也会尝试构建部分索引（见 `AnalysisSnapshot::fail`）

## 延伸阅读

- `SymbolIndex.cpp` — `build` 与匿名辅助函数的实现细节
- `ReferenceIndex.h` — 引用索引如何利用 `IndexedSymbol::linkageName` 和 `id`
- `AnalysisSnapshot.cpp` — 调用 `SymbolIndex::build` 的上下文


---
