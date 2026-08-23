---
Document category: implementation note
Applies to: Luna 0.3.0 development
Status: Implemented Experimental
Normative status: non-normative
---

# MoonIR 阅读指南

## 一句话定位

MoonIR 是 Luna 编译器的中间表示层，位于语义分析（Sema）与代码生成（CodeGen）之间。
它定义在命名空间 `moon` 中，核心头文件为 `src/moonir/MoonIR.h`。

## 为什么需要 MoonIR 层

MoonIR 不是"又一个 IR"。它的设计聚焦于三个目标：

1. **前后端隔离**
   前端（解析器 → 语义分析）产生的是包含 AST 指针、类型对象的"结构化"表示。MoonIR 将程序转换为**纯表格**形式：所有跨节点的引用都是整数索引或稳定字符串 ID，没有裸指针。这使得后端、运行时、序列化工具可以独立于前端内存布局工作。

2. **可验证**
   所有表格引用必须在密封后通过 `Verifier` 的检查，确保不存在悬空索引、类型不匹配、所有权合约违反等错误。验证在密封（Sealing）之后立即执行，不验证的 MoonIR 不可用于后端。

3. **可优化**
   MoonIR 提供 `Optimizer` 框架，当前已完成表示规范化（canonicalization），后续可添加语言级变换和运行时热点版本化。所有优化都是 MoonIR → MoonIR 的纯变换，不依赖 LLVM。

## 核心数据类型导读

### TypeRef / SymbolRef / ContractRef

这三个类型是 MoonIR 中最基础的"句柄"（handle），用于跨模块引用类型和声明。

```cpp
// src/moonir/MoonIR.h, 第 22-25 行
using TypeRef    = luna::types::TypeId;       // 指向类型
using TypeRefVec = std::vector<TypeRef>;
using SymbolRef  = luna::identity::SymbolId;    // 指向符号
using ContractRef = luna::identity::ContractId;  // 指向合约
```

它们实际是 `StableId<Tag>` 模板的实例化（定义在 `src/core/StableIdentity.h`）：

```cpp
// src/core/StableIdentity.h, 第 10-21 行
template <typename Tag>
struct StableId {
    std::string value;   // 64 位十六进制哈希字符串，如 "symbol_abc123..."
    bool empty() const;
    bool operator==(const StableId&) const;
};
```

**类比 C++**：这就像 `std::string` 作为唯一标识符，但通过模板标签（`TypeIdTag`、`SymbolIdTag`、`ContractIdTag`）在类型层面区分三种 ID，不会意外混用。
ID 的生成方式是：取规范字符串（canonical string）的 FNV-1a 64 位哈希，加上前缀（如 `"symbol_"`）。
**关键设计**：ID 是**确定性**的——给定相同的规范字符串，总是产生相同的 ID。这保证了序列化/反序列化的可重现性。

### DeclarationRef

一个声明引用由**符号**和**合约**两部分组成：

```cpp
// src/moonir/MoonIR.h, 第 30-42 行
struct DeclarationRef {
    SymbolRef symbol;      // 符号身份
    ContractRef contract;  // 合约身份（所有权、控制流等）

    bool empty() const;          // 两者都为空
    bool complete() const;       // 两者都不为空
};
```

**为什么不是只用一个 ID？** 因为同一个符号可以有多个合约版本（例如函数的不同所有权约定）。引用时必须同时指定符号和合约，后端才能找到正确的声明记录。

### TableRef<Tag> 模板与 InvalidTableIndex

```cpp
// src/moonir/MoonIR.h, 第 46-60 行
inline constexpr uint32_t InvalidTableIndex = std::numeric_limits<uint32_t>::max();

template <typename Tag>
struct TableRef {
    uint32_t value = InvalidTableIndex;  // 默认值为"空"
    bool empty() const;
};
```

`TableRef<Tag>` 是一个**类型安全的整数索引**。它用模板标签（Tag）来防止不同表格的索引被混用。
`InvalidTableIndex = 0xFFFFFFFF` 表示"不存在"。

**为什么用 stable table reference 而非指针？**
在 C++ 中，指向 `std::vector` 内部元素的指针在 `push_back` 后可能失效。MoonIR 的表格（如 `typeTable`、`declarationTable`）在构建过程中会增长，如果用裸指针会带来悬挂风险。使用整数索引则完全避免了这个问题，同时序列化时可以直接写入索引值，无需指针修复。

### BlockId / RegionId / ScopeId / LocalId / CleanupId

```cpp
// src/moonir/MoonIR.h, 第 62-71 行
struct BlockTag;    // 仅用于类型区分
struct RegionTag;   // 仅用于类型区分
struct ScopeTag;    // 仅用于类型区分
struct LocalTag;    // 仅用于类型区分
struct CleanupTag;  // 仅用于类型区分

using BlockId   = TableRef<BlockTag>;
using RegionId  = TableRef<RegionTag>;
using ScopeId   = TableRef<ScopeTag>;
using LocalId   = TableRef<LocalTag>;
using CleanupId = TableRef<CleanupTag>;
```

这些是控制流图（CFG）中各实体的句柄：

| 句柄类型 | 指向表格 | 含义 |
|----------|----------|------|
| `BlockId` | `ControlFlowGraph::blocks` | 基本块（basic block） |
| `RegionId` | `ControlFlowGraph::regions` | 区域（函数、lambda、循环等） |
| `ScopeId` | `ControlFlowGraph::scopes` | 作用域 |
| `LocalId` | `ControlFlowGraph::locals` | 局部变量（参数、绑定等） |
| `CleanupId` | `ControlFlowGraph::cleanups` | 清理操作 |

### RegionKind

```cpp
// src/moonir/MoonIR.h, 第 73-82 行
enum class RegionKind : uint8_t {
    Function,      // 函数体
    Lambda,        // 闭包（lambda）体
    Fragment,      // 片段（拦截器或上下文）
    Continuation,  // 延续
    Lexical,       // 词法作用域
    Loop,          // 循环
    MatchArm,      // match 分支
    Apply,         // apply 表达式
};
```

每个 `Region` 都有一个 `RegionKind`，后端据此决定如何处理这个区域的生命周期和执行模型。

### LocalKind

```cpp
// src/moonir/MoonIR.h, 第 84-90 行
enum class LocalKind : uint8_t {
    Parameter,   // 函数/方法参数
    Binding,     // let 绑定
    Pattern,     // 模式匹配中引入的变量
    Synthetic,   // 编译器生成的临时变量
    Allocation,  // 堆分配
};
```

### CleanupKind

```cpp
// src/moonir/MoonIR.h, 第 92-95 行
enum class CleanupKind : uint8_t {
    Value,       // 值类型的析构/清理
    Allocation,  // 堆分配相关的释放
};
```

### TerminatorKind

```cpp
// src/moonir/MoonIR.h, 第 97-106 行
enum class TerminatorKind : uint8_t {
    Invalid,      // 未初始化
    Jump,         // 无条件跳转到另一个块
    Branch,       // 条件分支
    Switch,       // 多路分支（match 消糖后）
    Return,       // 从函数返回
    Resume,       // 从片段中恢复
    Abort,        // 终止
    Unreachable,  // 不可达
};
```

每个基本块的最后一个指令必然是终结器（terminator），它决定了控制流的下一个去向。

### ProjectionKind 与 PlaceProjection

```cpp
// src/moonir/MoonIR.h, 第 108-136 行
enum class ProjectionKind : uint8_t {
    Field,          // 结构体字段（按序数）
    ConstantIndex,  // 常量数组索引
    DynamicIndex,   // 动态数组索引（运行时值）
    Dereference,    // 解引用
};

struct PlaceProjection {
    ProjectionKind kind = ProjectionKind::Field;
    uint64_t index = 0;          // 字段序数或常量索引
    LocalId dynamicIndex;         // 动态索引的局部变量
};

struct PlaceRef {
    LocalId root;                           // 根局部变量
    std::vector<PlaceProjection> projections; // 投影链
};
```

**类比 C++**：`PlaceRef` 类似于一个"路径表达式"——从根变量出发，经过一系列子对象访问，最终定位到某个内存位置。
例如 `person.address.city` 表示为 `{root: LocalId(0), projections: [{Field, 1}, {Field, 0}]}`（假设字段顺序）。
`PlaceProjection` 的链式结构允许后端直接计算内存偏移量。

## Module 结构

`Module` 是 MoonIR 的顶层容器，包含一个编译单元的所有信息。
它在 `src/moonir/MoonIR.h` 中定义（约第 300-395 行），主要成员如下：

```cpp
struct Module {
    // 格式版本
    uint32_t formatMajor = FormatMajor;  // = 0
    uint32_t formatMinor = FormatMinor;  // = 3

    // 元数据
    std::string name;
    std::string targetTriple;
    std::string dataLayout;
    FeatureFlags features;
    bool isPackage = false;

    // 源文件追踪
    std::vector<std::string> sourceFiles;
    std::vector<PackageUse> packageUses;
    std::vector<std::string> sourceModules;

    // 核心表格
    std::vector<TypeRecord> typeTable;
    std::vector<DeclarationRecord> declarationTable;

    // 元数据模式
    std::vector<MetadataSchema> metadataSchemas;

    // 前端 AST 声明（尚未密封）
    std::vector<std::unique_ptr<Decl>> declarations;

    // 快速索引（重建自表格，不序列化）
    std::unordered_map<std::string, size_t> typesById;
    std::unordered_map<std::string, size_t> declarationRecordsById;
    // ...

    // 关键方法
    TypeRef registerType(const TypePtr& type);
    void rebuildIndexes();
    const TypeRecord* findType(const TypeRef& ref) const;
    DeclarationRecord* findDeclaration(const DeclarationRef& ref);
};
```

**`formatMajor` 和 `formatMinor`**：MoonIR 使用语义版本控制格式。`formatMajor` 不兼容更改时递增，`formatMinor` 在兼容添加时递增。当前为 0.3。
**表格设计**：`typeTable` 和 `declarationTable` 是 MoonIR 的骨干。所有跨节点的引用都指向这些表格的行。表格行一旦插入即固定，不会因后续插入而失效。

## 各 .cpp 文件责任表

| 文件 | 主要责任 | 关键符号/入口函数 |
|------|----------|-------------------|
| `MoonIR.h` | 核心类型定义：Module、CFG、所有 Stmt/Expr 子类、TypeRecord、DeclarationRecord、PlaceRef 等 | `Module`、`ControlFlowGraph`、`TypeRef`、`DeclarationRef`、`TableRef<Tag>` |
| `MoonIR.cpp` | Module 方法实现：rebuildIndexes()、registerType()、findType() 等；规范字符串生成 | `Module::rebuildIndexes()`、`Module::registerType()`、`canonicalAbiLayout()`、`canonicalContract()` |
| `ControlFlowBuilder.h` | CFG 构建器接口：从结构化 AST 到 CFG 的转换 | `ControlFlowBuilder::build()`、`setCaptureEnvironment()` |
| `ControlFlowBuilder.cpp` | CFG 构建实现：深度优先遍历 AST、克隆表达式、生成基本块链 | `build()`、`cloneStructuredExpr()`、`IteratorRecipePlan` |
| `Sealer.h` | 密封器接口：从函数体生成 CFG 并验证 | `Sealer::sealFunctionBodies()` |
| `Sealer.cpp` | 密封实现：遍历所有函数、构造 CFG、验证通过后原子替换 | `sealFunctionBodies()`、`isConcreteExecutable()` |
| `Verifier.h` | 验证器接口 | `Verifier::verify()`（两个重载） |
| `Verifier.cpp` | 验证实现：检查表格、引用、类型、合约等 | `verify()`、`verifyDeclarationRef()`、`verifyCleanupAction()`、`verifyCanonicalTables()`、`verifyRegions()` |
| `Optimizer.h` | 优化器接口 | `Optimizer::run()` |
| `Optimizer.cpp` | 优化实现：当前仅做表示规范化 | `run()` → `canonicalize()` → `Module::rebuildIndexes()` |
| `Printer.h` | MoonIR 文本打印接口 | `Printer::print()`、`str()`、`printCostReport()` |
| `Printer.cpp` | 打印实现：输出模块、类型、声明、CFG 等 | `print()`、`typeName()` |
| `Lowering.h` | 前端到 MoonIR 的 lowering 接口 | `LunaLowerer::lower()` |
| `Lowering.cpp` | lowering 实现：将前端 AST 和符号表转换为 MoonIR Module | `lower()`、`lowerDecl()`、`lowerExpr()`、`lowerStmt()`、`resolveDeclarationReferences()` |
| `Container.h` | 二进制容器（序列化）的低层读写 | `ContainerWriter::encode()`、`ContainerReader::parse()` |
| `Container.cpp` | 容器实现：魔数、头部、目录、校验和 | `encode()`、`parse()`、`digestFor()` |
| `ContainerModel.h` | 高层序列化模型：Codec 将 Module 编码为容器各节 | `ContainerModelCodec::encodeManifest()`、`encodeTypes()`、`encodeDeclarations()` |
| `ContainerModel.cpp` | 序列化实现：Encoder/Decoder 类，逐节编码/解码 | `Encoder`、`Decoder`、`encodeManifest()`、`encodeTypes()`、`decodeDeclarations()` |

## ControlFlowBuilder 如何构建 CFG

### 输入输出

**输入**：结构化 AST（`BlockStmt`、`Stmt`、`Expr` 等——这些是 `moon::` 命名空间中的树状节点，如 `LetStmt`、`CallExpr`）。

**输出**：`ControlFlowGraph`，一个纯表格的图结构。

### 核心流程

`ControlFlowBuilder::build()` 的重载（`ControlFlowBuilder.h`，第 22-32 行）：

构建过程大致如下：

1. **创建根区域**：根据 `rootKind`（如 `RegionKind::Function`）创建根 `Region`，分配 `RegionId`。
2. **创建根作用域**：分配 `ScopeId`。
3. **创建入口基本块**：分配 `BlockId`。
4. **遍历结构化 AST**：递归遍历 `BlockStmt` 中的语句列表，为每条语句生成对应的 CFG 节点。
   - `LetStmt` → 分配一个 `Local`，后续赋值变为操作。
   - `IfStmt` → 生成 `Branch` 终结器和两个分支块。
   - `ForStmt` → 生成迭代器协议，包含 `IteratorRecipePlan`。
   - `MatchStmt` → 生成 `Switch` 终结器和多个分支块。
5. **链接基本块**：每个块以 `Terminator` 结尾，指向后继块。
6. **填充表格**：`blocks`、`regions`、`scopes`、`locals`、`cleanups`。

### 内部辅助结构

`ControlFlowBuilder` 使用多个内部结构来跟踪状态：

- **`OpenBlock`**（第 50-53 行）：正在构建的块，包含清理操作的栈。
- **`BuiltBlock`**（第 55-60 行）：已完成的块，包含区域、作用域和入口块。
- **`FragmentContext`**（第 62-70 行）：跟踪片段（interceptor/context）的边界。
- **`IteratorRecipePlan`**（第 80-94 行）：迭代器协议的构建计划，包含模式、源、步骤等。

### 深度保护

`ControlFlowBuilder.cpp` 定义了 `kMaxStructureDepth = 4096`，用 `StructureDepthGuard` 防止栈溢出。

### 克隆机制

`ControlFlowBuilder.cpp` 包含完整的结构化 AST 克隆函数（`cloneStructuredExpr`、`cloneStructuredStmt`、`cloneStructuredBlock`），用于非消费性构建模式（密封器使用此模式：先构建所有 CFG，验证通过后再原子替换原始 body）。

## Sealer / Verifier

### 什么叫 Sealed

"密封"（Sealing）是一个**一次性转换**：将函数体的结构化 AST 转换为 `ControlFlowGraph`，然后**丢弃原始 AST**。

`Sealer::sealFunctionBodies()` 的实现（`Sealer.cpp`，第 20-76 行）：

1. 遍历 `module.declarations` 中的所有函数，检查 `isConcreteExecutable()`（第 12-16 行：非 extern、非 selector、非泛型等）。
2. 对每个符合条件的函数：
   - 使用 `ControlFlowBuilder` 构建 CFG。
   - 使用 `Verifier` 验证 CFG。
   - 如果任何一步失败，**不提交任何更改**（事务语义）。
3. 所有函数都通过后，**原子地**替换：将 `function->controlFlow` 设为新 CFG，`function->body.reset()` 丢弃结构化 AST。

### Verifier 验证什么

`Verifier` 有两个重载的 `verify()`（`Verifier.h`，第 15-16 行）：

验证检查包括：

- **声明引用完整性**：`verifyDeclarationRef()` — 检查 `DeclarationRef` 的 `symbolId` 和 `contractId` 是否指向有效的声明记录，以及声明类型是否匹配（`Verifier.cpp`，第 35-71 行）。
- **类型引用完整性**：`verifyType()` — 检查 `TypeRef` 是否指向有效的 `TypeRecord`。
- **清理操作正确性**：`verifyCleanupAction()` — 检查清理操作是否与类型的 `ResourceContract` 一致（`Verifier.cpp`，第 15-33 行）。
- **CFG 结构不变量**：`verifyCanonicalTables()` — 检查表格索引是否连续、根实体是否存在。
- **区域结构**：`verifyRegions()` — 检查块的所有权、父子关系是否有环。

### 为什么只在密封后验证

验证需要**完整的表格**。在密封前，函数体还是结构化 AST 树，其中的引用（如 `DeclarationRef`）可能尚未解析。密封（CFG 构建）将所有引用解析为表格索引，而验证器检查的就是这些索引的有效性。
此外，验证是**一次性成本**——密封后模块不再变化（除非显式优化），验证通过后后端可以信任所有引用。

## Optimizer / Printer / Lowering / Container

### Optimizer

`Optimizer`（`Optimizer.h`，第 32-41 行）当前只有一个功能：**表示规范化**（canonicalization）。

```cpp
bool Optimizer::run(Module& module, const OptimizationRequest& request) {
    canonicalize(module);
    return mErrors.empty();
}
```

`canonicalize()` 只做一件事：调用 `module.rebuildIndexes()` 重建所有查找映射。注释明确指出（`Optimizer.cpp`，第 8-11 行）：
> "The first implementation intentionally performs only representation canonicalization."

也就是说，Optimizer 是一个预留的扩展点，当前仅确保反序列化后的模块具有最新的索引。

### Printer

`Printer`（`Printer.h`，第 10-15 行）提供三个方法：
```cpp
void print(const Module& module, std::ostream& out) const;
std::string str(const Module& module) const;
void printCostReport(const Module& module, std::ostream& out) const;
```

`print()` 输出格式化的模块文本表示，包含：模块头、类型表、元数据模式、声明表、控制流图等。

### Lowering

`LunaLowerer`（`Lowering.h`，第 26-76 行）是**前端到 MoonIR 的桥梁**。

```cpp
std::unique_ptr<Module> lower(const Program& program,
                              const SymbolTable& symbols,
                              bool reserveKernelRuntime = false);
```

简化流程：
1. **创建 Module**：设置名称、特性、包引用等。
2. **遍历 Program 的声明**：对每个前端 `Decl`，调用 `lowerDecl()`。
3. **注册类型**：每次遇到类型时，调用 `addDeclarationRecord()` 将其添加到 `declarationTable`。
4. **延迟解析引用**：`deferDeclarationRef()` 记录未解析的引用，`resolveDeclarationReferences()` 在最后统一解析。
5. **构建模块接口**：`buildModuleInterfaces()` 生成导入/导出表。
6. **注入编译器内置 trait**：如 `Drop`、`From` 等。

### Container（序列化/确定性加载）

MoonIR 使用自定义的二进制容器格式，由两层组成：

#### 低层：Container.h / Container.cpp

`ContainerWriter::encode()` 和 `ContainerReader::parse()` 处理二进制格式：
- **魔数**：`0x89 4d 4f 4f 4e 0d 0a 1a`（即 "MOON" 前加 0x89）
- **头部**：80 字节，包含格式版本、节目录偏移量、校验和等
- **目录条目**：每个 32 字节，包含节 ID、标志、偏移量、长度、解码后长度
- **校验和**：SHA-256，覆盖头部和所有节
- **节类型**：`ContainerSectionId` 枚举（`Container.h`，第 11-19 行）：Manifest=1, Type=2, Symbol=3, Contract=4, Code=5, Imports=6, Exports=7, Sysmeta=8

#### 高层：ContainerModel.h / ContainerModel.cpp

`ContainerModelCodec` 将 `Module` 编码为各节或从各节解码为 `Module`：
- `encodeManifest()` / `decodeManifest()`：包清单
- `encodeTypes()` / `decodeTypes()`：类型表
- `encodeSymbols()`、`encodeContracts()`、`encodeSysmeta()`：分别编码符号、合约、系统元数据
- `decodeDeclarations()`：**原子地**合并 Symbols + Contracts + Sysmeta 三个节
- `encodeImports()` / `encodeExports()`：导入/导出表
- `encodeCode()` / `decodeCode()`：代码（CFG）序列化

**确定性加载**：`Encoder` 类确保编码过程是确定性的——相同的 `Module` 产生相同的字节序列。

## 数据流：从 Sema 到 verified MoonIR

```
前端 AST (Program) + SymbolTable
          │
          ▼
    LunaLowerer::lower()     ← Lowering.cpp
          │
          ▼
    MoonIR Module (未密封)
    - typeTable
    - declarationTable
    - declarations[] (含 BlockStmt*)
          │
          ▼
    Sealer::sealFunctionBodies()  ← Sealer.cpp
          │
     ┌────┴────┐
     │         │
     ▼         ▼
  CFG       Verifier::verify()
  Builder   (验证 CFG)
     │         │
     └────┬────┘
          │ 通过?
     ┌────┴────┐
     │ YES     │ NO → 回滚，报告错误
     ▼         │
  原子替换:   │
  body.reset()│
  controlFlow │
  = graph     │
          │
          ▼
    Sealed Module (已验证的)
          │
     ┌────┼────┬──────┐
     │    │    │      │
     ▼    ▼    ▼      ▼
  Optim  Cont  Code  Printer
  izer  ainer Gen
     │    │
     │    ▼
     │  .moonc 二进制文件
     ▼
   LLVM CodeGen
```

关键要点：
1. **Lowering** 产生的是"未密封的"Module——函数体仍是结构化 AST（`BlockStmt` 树），包含 `TypePtr` 等前端对象。
2. **Sealing** 是事务性的：先构建所有 CFG 并验证，全部通过后才原子替换。
3. **验证通过后**，Module 可以安全地交给后端（CodeGenerator）或序列化器（ContainerModelCodec）。
4. **Optimizer** 当前仅做索引重建，但设计上是可扩展的 MoonIR → MoonIR 通道。

## 给 C++ 读者的"新概念"清单

### 1. CFG（控制流图）

**C++ 类比**：CFG 就像一个函数的所有可能的执行路径构成的图。每个基本块（Block）是连续执行的一段代码，没有分支进入或离开块的中间。

MoonIR 的 `ControlFlowGraph` 包含 `blocks`、`regions`、`scopes`、`locals`、`cleanups` 五个表格。每个 Block 包含 `id`、`region`、`scope`、`ops`（操作列表）和 `terminator`（终结器）。

### 2. Place / PlaceProjection

**C++ 类比**：`PlaceRef` 类似于 `(base_pointer, {offset_1, offset_2, ...})` 的元组。它描述了一个内存位置，通过一个"根局部变量 + 投影链"来定位。
这类似于 LLVM 的 GEP（GetElementPtr）指令，但更抽象——投影链可以包含字段访问、常量索引、动态索引和解引用。

### 3. Sealing（密封）

**C++ 类比**：密封类似于"冻结"——就像将 `std::vector` 转换为 `std::span`，同时丢弃原始数据的所有权信息。密封后，模块不再依赖前端内存布局，所有引用都变为稳定的整数索引。

### 4. TableRef（类型安全索引）

**C++ 类比**：`TableRef<BlockTag>` 就像 `uint32_t`，但你不能把 `ScopeId` 赋值给 `BlockId`。这类似于 `std::variant` 的标签，但用空结构体模板参数来实现类型安全。

### 5. 终结器（Terminator）

**C++ 类比**：基本块的终结器类似于汇编中的跳转指令。`Jump` = `jmp`，`Branch` = `je`/`jne`，`Switch` = `jmp table`，`Return` = `ret`。

### 6. Region（区域）

**C++ 类比**：`Region` 类似于一个作用域，但更通用。它可以是一个函数体、一个 lambda 体、一个循环体、一个 match 分支等。每个 Region 有自己的局部变量表和清理操作表。

### 7. 所有权合约（Ownership Contract）

MoonIR 中的每个类型和声明都携带所有权信息（`luna::ownership::Contract`），包含 `relation`、`usage`、`cleanup` 等。这在 C++ 中没有直接对应，更接近 Rust 的所有权系统，但更显式地编码在 IR 中。

### 8. 系统元数据（SysMeta）

每个 `TypeRecord` 和 `DeclarationRecord` 都包含 `luna::sysmeta::Facts`，包含 control（控制流）、resource（资源管理）、capability（能力）、abi（ABI 约定）、identity（身份信息）五个方面的元数据。

## 阅读顺序建议

如果你是第一次接触 MoonIR，建议按以下顺序阅读代码：

1. **`src/core/StableIdentity.h`** → 理解 `StableId<Tag>` 和确定性 ID 生成
2. **`src/core/TypeIdentity.h`** → 理解 `TypeId`、`ShapeId`、`IdentityMode`、`TypeDomain`
3. **`src/core/TypeSystem.h`** → 理解 `Type`、`TypeKind`、`TypePtr` 等前端类型系统
4. **`src/moonir/MoonIR.h`** → **最重要的文件**，通读所有类型定义
5. **`src/moonir/MoonIR.cpp`** → 理解 `rebuildIndexes()`、`registerType()`
6. **`src/moonir/Lowering.h` + `Lowering.cpp`** → 理解前端如何生成 MoonIR
7. **`src/moonir/ControlFlowBuilder.h` + `ControlFlowBuilder.cpp`** → 理解结构化 AST 到 CFG 的转换
8. **`src/moonir/Sealer.h` + `Sealer.cpp`** → 理解密封事务
9. **`src/moonir/Verifier.h` + `Verifier.cpp`** → 理解验证规则
10. **`src/moonir/Optimizer.h` + `Optimizer.cpp`** → 理解优化框架
11. **`src/moonir/Printer.h` + `Printer.cpp`** → 理解文本表示
12. **`src/moonir/Container.h` + `Container.cpp`** → 理解二进制格式
13. **`src/moonir/ContainerModel.h` + `ContainerModel.cpp`** → 理解序列化/反序列化
14. **`tests/moonir_canonical_test.cpp`** → 通过测试理解用法

## 相关测试文件

- **`tests/moonir_canonical_test.cpp`**（5639 行）：MoonIR 的主要测试文件，包含：
  - 类型系统静态断言（验证 `TypeRef`、`DeclarationRef` 等类型正确性）
  - CFG 表格引用稳定性测试
  - 迭代器类型注册和比较
  - 结构体/枚举/Result 类型测试
  - 完整的编译管线测试（从解析到代码生成）
  - 容器序列化/反序列化测试
  - 规范字符串一致性测试
