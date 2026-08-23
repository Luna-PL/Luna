> Document category: implementation note
> Applies to: Luna 0.3.0 development
> Status: Implemented Experimental
> Normative status: non-normative
> 语言：中文权威版（英文版为占位）

# src/package/ —— 目录逐文件指南

本指南合并了 src/package/ 下全部源码文件的五段式导读，方便你一次读完整个目录。

---
title: Package.cpp
source: src/package/Package.cpp
language: C++ (C++17)
audience: Luna 编译器开发者 / 插件开发者
---

# src/package/Package.cpp

Package.cpp 实现了 `PackageLoader` 的两个静态方法 `load` 和 `exports`，以及 `packageKindName` 工具函数，是包加载入口与公开导出符号提取的实现文件。

## 这个文件做什么

- 实现 `packageKindName` 枚举转字符串。
- 实现 `PackageLoader::load` 作为便捷入口，委托 `PackageManager::load` 执行实际加载。
- 实现 `PackageLoader::exports` 遍历 AST 提取所有带 `export` 标记的公开声明。
- 提供匿名命名空间内的辅助函数 `publicDeclarationName`，处理 `generatedSymbolName` 覆盖逻辑。

## 关键结构体·类·枚举

### 匿名命名空间辅助函数

#### `publicDeclarationName(const Decl* declaration, const std::string& sourceName) -> std::string`

决定一个声明的导出名称。如果该声明有 `generatedSymbolName` 且与源名称不同，则使用 `generatedSymbolName`；否则返回 `sourceName`。这类似于 C++ 中 `__attribute__((alias(...)))` 或 `#pragma redefine_extname` 的效果——元数据可为声明分配一个不同于源码标识符的链接符号。

## 关键函数·方法

### `packageKindName(PackageKind kind) -> const char*`

switch 实现，将 `PackageKind` 枚举值映射为字符串字面量。匹配 `Unspecified` 时返回 `"unspecified"`；fallthrough 也返回 `"unspecified"`。

### `PackageLoader::load(const std::string& path, LoadedPackage& result, std::vector<diagnostic::Diagnostic>& errors) -> bool`

1. 构造 `PackageManager` 实例（默认构造，使用默认 `MacroProcessor`）。
2. 构造 `PackageGraph` 和 `PackageRequest`。
3. 将 `path` 填入 `request.inputPath`。
4. 调用 `manager.load(request, result, graph, errors)` 返回结果。

此函数是提供给外部调用者的最小化 API。所有复杂逻辑（清单解析、源码收集、宏展开、依赖解析）均在 `PackageManager::load` 中完成。

### `PackageLoader::exports(const Program* program) -> std::vector<PackageExport>`

遍历 `program->declarations`，逐个检查：

1. `declaration->isExported` 必须为 `true`。
2. `declaration->packageId` 必须为空或等于 `program->packageName`（排除跨包未导出的声明）。
3. 通过 `dynamic_cast` 判断具体声明类型：
   - `FunctionDecl` → `kind = "function"`
   - `FragmentDecl` → `kind = "fragment"`
   - `StructDecl` → `kind = "struct"`
   - `EnumDecl` → `kind = "enum"`
   - `TraitDecl` → `kind = "trait"`
4. 用 `publicDeclarationName` 确定导出名，填入 `PackageExport` 结构并收集。

## 与周边文件·阶段的关系

```
Package.cpp  →  Package.h（声明）
              →  PackageManager.h/.cpp（委托调用）
              →  parser/AST.h（Program / Decl 层次结构）
```

- **Package.h**：声明所在头文件，`LoadedPackage`、`PackageExport` 等类型在此定义。
- **PackageManager.h/.cpp**：`PackageLoader::load` 将实际工作委托给 `PackageManager::load`，后者承担清单解析、依赖解析、递归加载等全部复杂逻辑。
- **parser/AST.h**：定义了 `Program`、`Decl`、`FunctionDecl`、`FragmentDecl`、`StructDecl`、`EnumDecl`、`TraitDecl` 等类型，`exports` 方法通过 `dynamic_cast` 遍历它们。

## 延伸阅读

- `PackageManager.cpp`：完整的包加载流水线实现
- `parser/AST.h`：AST 声明层次结构
- `diagnostics/Diagnostic.h`：诊断错误格式


---

---
title: Package.h
source: src/package/Package.h
language: C++ (C++17)
audience: Luna 编译器开发者 / 插件开发者
---

# src/package/Package.h

Package.h 定义了 Luna 的包数据结构（PackageExport、PackageUse、PackageKind、PackageManifest、ResolvedPackage、LoadedPackage）以及包加载入口 `PackageLoader` 类，是编译器中描述"一个包是什么"的核心类型头文件。

## 这个文件做什么

- 声明包的元数据模型：标识、版本、种类、源码根、依赖、host 导入等。
- 声明解析后的包信息（ResolvedPackage）和加载后的完整包（LoadedPackage）。
- 提供 `PackageLoader` 静态接口，用于加载单个 `.luna` 文件或整个目录，并提取包的公开导出符号。

## 关键结构体·类·枚举

### `PackageKind`（enum class）

包的种类枚举，相当于 C++ 中标识"库"与"应用程序"的 tag。

| 枚举值 | 含义 |
|--------|------|
| `Unspecified` | 未指定（默认） |
| `Application` | 可执行应用 |
| `Library` | 库 |

配套函数 `packageKindName(PackageKind)` 返回字符串名称。

### `PackageExport`

表示包对外公开导出的一个符号，类比 C++ 动态库的 `__declspec(dllexport)` 信息。

```cpp
struct PackageExport {
    std::string name;
    std::string kind;
    std::string modulePath;
};
```

### `PackageUse`

表示一个源文件中对另一个包的使用声明（`use pkg::...`），相当于 C++ 的 `#include` + `using namespace` 的跨包版本。

```cpp
struct PackageUse {
    std::string packageId;
    std::string alias;
    std::string sourcePath;
    int line;
    int column;
};
```

### `PackageManifest`

从 `luna.package` 清单文件解析出的完整元数据。类比 C++ 的 `conanfile.py` 或 `vcpkg.json`。

```cpp
struct PackageManifest {
    std::string id;
    std::string version;
    PackageKind kind;
    std::vector<std::string> sources;
    std::unordered_map<std::string, std::string> dependencies;
    std::unordered_map<std::string, std::string> hostImports;
    std::string path;
};
```

`hostImports` 字段将包内模块限定的 `extern` 声明映射到宿主运行时提供的稳定能力 ID（如 `std.io`），编译器据此生成对宿主 FFI 的调用。

### `ResolvedPackage`

依赖解析后的包定位信息，类比 C++ 的 `find_package` 结果与 lock 缓存的复合体。

```cpp
struct ResolvedPackage {
    std::string id;
    std::string version;
    std::string rootPath;
    std::string source;
    std::string hash;
};
```

### `LoadedPackage`

`PackageLoader::load` 的输出，包含解析后的完整包。类比 C++ 翻译单元汇编后的"编译单元"乘包元数据。

```cpp
struct LoadedPackage {
    std::unique_ptr<Program> program;
    std::string rootPath;
    std::vector<std::string> sourceFiles;
    std::vector<std::string> modules;
    std::vector<PackageUse> packageUses;
    PackageManifest manifest;
};
```

### `Program::PackageUse`（AST.h 内嵌）

在 AST 层记录跨包使用关系，字段与 `PackageUse` 相似但增加了 `ownerPackageId` 字段标识该 use 声明所属的包。

### `Program::HostImport`（AST.h 内嵌）

记录包对宿主能力的导入声明，由 `PackageManifest::hostImports` 填充到 AST 层。

## 关键函数·方法

### `packageKindName(PackageKind kind) -> const char*`

将 `PackageKind` 枚举值转为字符串 `"application"` / `"library"` / `"unspecified"`。失败默认返回 `"unspecified"`。

### `PackageLoader::load(const std::string& path, LoadedPackage& result, std::vector<diagnostic::Diagnostic>& errors) -> bool`

静态方法，包加载的便捷入口。接收文件或目录路径，内部创建 `PackageManager` 和 `PackageGraph` 后委托 `PackageManager::load` 执行实际加载。返回 `true` 表示成功且无错误。

### `PackageLoader::exports(const Program* program) -> std::vector<PackageExport>`

静态方法，提取程序所有的公开导出声明。遍历 `Program::declarations`，只保留 `isExported == true` 且属于当前包（`packageId` 为空或等于 `program->packageName`）的声明，按 `dynamic_cast` 区分函数（FunctionDecl）、片段（FragmentDecl）、结构体（StructDecl）、枚举（EnumDecl）、trait（TraitDecl），并会用 `publicDeclarationName` 处理 `generatedSymbolName` 覆盖。

## 与周边文件·阶段的关系

```
Package.h  ──────→  AST.h（Program、Decl 及其派生类）
       │
       ├── diagnostics/Diagnostic.h（错误报告）
       │
       └── PackageManager.h（实际加载逻辑）
```

- **PackageManager.h/.cpp**：`PackageLoader::load` 的底层实现，包含清单解析、依赖解析、递归加载等。
- **parser/AST.h**：`Program` 结构体在此定义，`LoadedPackage::program` 的类型。
- **diagnostics/Diagnostic.h**：错误诊断格式工具，`PackageLoader` 用其报告错误。

Luna 的软件包加载流程：`PackageLoader::load` → `PackageManager::load` → 解析清单 → 收集源码 → 词法/宏展开/语法分析 → 解析依赖 → 合并 AST。

## 延伸阅读

- `PackageManager.h` / `PackageManager.cpp`：包的完整加载与解析逻辑
- `parser/AST.h`：`Program`、`Decl` 及其派生类的完整定义
- `diagnostics/Diagnostic.h`：诊断错误格式化与渲染
- `macro/MacroProcessor.h`：宏展开处理器，在加载阶段应用于每个源码文件


---

---
title: PackageManager.cpp
source: src/package/PackageManager.cpp
language: C++ (C++17)
audience: Luna 编译器开发者 / 插件开发者
---

# src/package/PackageManager.cpp

PackageManager.cpp 是 Luna 包加载流水线的完整实现，包括 TOML 清单解析器、workspace/lock 文件解析、源码收集与解析、依赖图构建与递归加载、AST 合并等所有核心逻辑。

## 这个文件做什么

- 实现匿名命名空间中的辅助函数：TOML 值解析、manifest/workspace/lock 文件读取与校验。
- 实现 `PackageManager::load`：完整的包加载流程，从路径验证到 AST 合并。
- 递归加载依赖包，构建完整的传递依赖闭包。
- 校验依赖版本一致性、lock 文件锁定、无循环依赖等约束。

## 关键结构体·类·枚举

### 匿名命名空间辅助函数

此文件大量使用匿名命名空间封装内部工具函数，相当于 C++ 的 `static` 函数（内部链接）：

| 函数 | 用途 |
|------|------|
| `trim(std::string)` | 去除字符串首尾空白字符 |
| `withoutComment(const std::string&)` | 去掉 TOML 行中的 `#` 注释（处理引号内的 `#` 不被误删） |
| `parseTomlString(const std::string&, std::string&)` | 解析 TOML 双引号字符串，处理转义序列（`\\n`、`\\t` 等） |
| `parseTomlStringArray(const std::string&, std::vector<std::string>&)` | 解析 TOML 字符串数组 `["a", "b"]` |
| `splitTomlAssignment(const std::string&, std::string& key, std::string& value)` | 按 `=` 分割 TOML 键值对，处理引号键名 |
| `manifestError(...)` | 构造并添加清单解析错误诊断 |
| `parsePackageManifest(...)` | 解析 `luna.package` 文件，填充 `PackageManifest` |
| `parseWorkspace(...)` | 解析 `luna.workspace` 文件，提取 `members` 列表 |
| `parseLock(...)` | 解析 `luna.lock` 文件，填充 `ResolvedPackage` 向量 |
| `findWorkspace(const fs::path&)` | 从当前目录向上搜索 `luna.workspace` 文件 |
| `readSource(...)` | 读取源码文件内容到字符串 |
| `parseSource(...)` | 对单个源文件执行宏展开→词法分析→语法分析完整流程 |
| `collectManifestSources(...)` | 根据清单的 `sources` 列表收集所有 `.luna` 文件 |
| `assignDeclarationOwner(...)` | 设置声明的 `packageId` 和 `modulePath`，并递归处理 `ImplDecl` 的方法 |

## 关键函数·方法

### `PackageManager::load(const PackageRequest&, LoadedPackage&, PackageGraph&, vector<diagnostic::Diagnostic>&) -> bool`

这是整个包加载的核心。其执行流程分阶段如下：

#### 阶段 1：路径验证

检查输入路径是否存在。若不存在则报告 `PKG0001` 错误。

#### 阶段 2：清单解析与源码收集

- 如果输入是目录，检查是否存在 `luna.package` 文件。
- 存在清单 → 调用 `parsePackageManifest` 解析，再调用 `collectManifestSources` 收集所有 `.luna` 文件。
- 无清单 → 直接目录遍历所有 `.luna` 文件。
- 如果输入是单个文件，直接使用该文件。

#### 阶段 3：初始化 LoadedPackage

创建 `Program` 实例，设置 `isPackage` 和 `packageName`，填充 `hostImports`。

#### 阶段 4：源码覆盖层验证

检查 `request.overlays` 中的路径是否属于当前包的源文件，防止覆盖不属于该包的文件。

#### 阶段 5：逐文件解析

对每个源文件依次：
1. 检查是否有覆盖层源码，否则从磁盘读取。
2. 调用 `MacroProcessor::process` 进行宏展开。
3. 调用 `Lexer::tokenize` 进行词法分析。
4. 调用 `Parser::parse` 进行语法分析。
5. 收集 `packageUses`，检查别名冲突。
6. 检查 `packageName` 一致性。
7. 调用 `assignDeclarationOwner` 标记声明所属包。
8. 将声明迁移到 `result.program`。

#### 阶段 6：Workspace 与依赖解析

通过 `findWorkspace` 向上搜索 `luna.workspace`，解析所有 workspace 成员，构建 `workspacePackages` 映射。然后：

1. 检查 `luna.lock` 文件是否存在，如果存在依赖则必须存在。
2. 对每个 `PackageUse`，验证：
   - 该包 ID 已在 `[dependencies]` 中声明。
   - 该包 ID 是一个 workspace 成员。
   - 版本约束精确匹配。
   - lock 文件锁定版本一致。
3. 递归加载依赖包（`loadDependency` lambda），使用 `loadingDependencies` / `loadedDependencies` 集合检测循环依赖。

#### 阶段 7：依赖递归加载

`loadDependency` lambda 是递归函数，对每个依赖包：
1. 解析其 `luna.package` 清单。
2. 收集其 `hostImports`。
3. 收集并解析其源码文件。
4. 收集其 `packageUses`，递归加载其依赖。
5. 将所有声明合并到 `result.program`。

#### 阶段 8：最终合并与排序

合并所有 `packageUses` 到 `result.packageUses` 和 `result.program->packageUses`，排序去重依赖列表，设置 `result.modules` 和 `graph.modules`。

### 辅助解析器详解

#### TOML 解析器

文件内嵌了一个轻量级 TOML 解析器，支持：
- 字符串值：`key = "value"`，含转义序列（`\\n`、`\\t`）
- 字符串数组：`key = ["a", "b"]`
- 行注释：`# comment`
- 引用键名：`"key with spaces" = "value"`
- 节标题：`[section]`

不支持多行字符串、内联表、整数/浮点数/布尔值等——这些在 Luna 的 manifest 中不需要。

#### `parsePackageManifest`

解析 `luna.package`，支持三个节：
- `[package]`：`id`、`version`、`kind`（application/library）、`sources`（字符串数组）。
- `[dependencies]`：包 ID → 版本约束映射。
- `[host-imports]`：模块限定的 extern 声明 → 宿主能力 ID 映射。

校验：`id`、`version`、`sources` 必须非空。

#### `parseWorkspace`

解析 `luna.workspace`，只支持 `[workspace]` 节和 `members` 数组，成员路径必须非空。

#### `parseLock`

解析 `luna.lock`，支持多个 `[[package]]` 条目，每个条目包含 `id`、`version`、`source`、`hash` 四个字段，全部为必填字符串。

## 与周边文件·阶段的关系

```
PackageManager.cpp  →  PackageManager.h（声明）
                    →  Package.h（类型定义）
                    →  macro/MacroProcessor.h（宏展开）
                    →  lexer/Lexer.h（词法分析）
                    →  parser/Parser.h（语法分析）
                    →  diagnostics/Diagnostic.h（错误报告）
```

- **PackageManager.h**：声明 `PackageManager` 类与 `PackageGraph`。
- **Package.h**：定义了 `PackageManifest`、`ResolvedPackage`、`LoadedPackage`、`PackageUse` 等类型。
- **lexer/Lexer.h**：`Lexer` 类，在 `parseSource` 中调用。
- **parser/Parser.h**：`Parser` 类，在 `parseSource` 中调用。
- **macro/MacroProcessor.h**：`MacroProcessor::process`，在 `parseSource` 中调用。
- **diagnostics/Diagnostic.h**：`diagnostic::format` 用于生成结构化错误。

编译器流水线中的位置：

```
输入路径
  │
  ▼
PackageManager::load  ←── 本文件
  │
  ├── parsePackageManifest（luna.package）
  ├── collectManifestSources
  ├── parseSource（对每个文件）
  │     ├── MacroProcessor::process
  │     ├── Lexer::tokenize
  │     └── Parser::parse
  ├── findWorkspace（luna.workspace）
  ├── parseLock（luna.lock）
  ├── loadDependency（递归依赖加载）
  └── AST 合并
       │
       ▼
  LoadedPackage（交付给语义分析）
```

## 延伸阅读

- `PackageManager.h`：`PackageManager`、`PackageRequest`、`PackageGraph` 声明
- `Package.h`：`PackageManifest`、`ResolvedPackage`、`LoadedPackage`、`PackageUse` 定义
- `macro/MacroProcessor.h`：宏展开处理器详解
- `lexer/Lexer.h`：词法分析器
- `parser/Parser.h`：语法分析器
- `diagnostics/Diagnostic.h`：诊断系统


---

---
title: PackageManager.h
source: src/package/PackageManager.h
language: C++ (C++17)
audience: Luna 编译器开发者 / 插件开发者
---

# src/package/PackageManager.h

PackageManager.h 声明了包加载请求结构体 `PackageRequest`、包图结构体 `PackageGraph` 以及核心加载类 `PackageManager`，是 Luna 编译器中包加载流水线的中枢接口。

## 这个文件做什么

- 定义 `PackageRequest`：封装加载请求的输入路径和可选的源码覆盖层。
- 定义 `PackageGraph`：记录加载过程中产生的完整依赖图信息。
- 声明 `PackageManager` 类：持有 `MacroProcessor` 实例，提供 `load` 方法执行完整的包加载流程。

## 关键结构体·类·枚举

### `PackageRequest`

包加载的输入参数，类比 C++ 中传递给构建系统的"编译请求"。

```cpp
struct PackageRequest {
    std::string inputPath;
    struct SourceOverlay {
        std::string path;
        std::string source;
    };
    std::vector<SourceOverlay> overlays;
};
```

`SourceOverlay` 允许在测试或 REPL 场景中，用内存中的源码字符串替换磁盘上的文件内容，而不实际修改文件系统。

### `PackageGraph`

包加载完成的依赖关系图，承载了后续驱动阶段（driver）需要的所有元数据。

```cpp
struct PackageGraph {
    std::string rootPath;
    std::vector<std::string> sourceUnits;
    std::vector<std::string> dependencies;
    std::vector<PackageUse> dependencyUses;
    std::vector<std::string> modules;
    std::vector<ResolvedPackage> resolvedPackages;
    std::string manifestPath;
    std::string workspacePath;
    std::string lockPath;
    std::vector<std::string> enabledCapabilities;
};
```

`resolvedPackages` 存放从 workspace 解析并在 lock 文件中锁定的所有依赖包，包含直接和间接依赖的完整传递闭包。`enabledCapabilities` 是未来能力系统（capability system）的预留字段。

### `PackageManager`

核心加载类，封装了宏处理器并提供了 `load` 方法。采用 RAII 风格，构造函数接收 `MacroProcessor`。

```cpp
class PackageManager {
public:
    explicit PackageManager(
        luna::macro::MacroProcessor macroProcessor = luna::macro::MacroProcessor());

    bool load(const PackageRequest& request, LoadedPackage& result,
              PackageGraph& graph,
              std::vector<diagnostic::Diagnostic>& errors) const;
private:
    luna::macro::MacroProcessor mMacroProcessor;
};
```

构造函数使用默认参数，因此 `PackageManager` 可默认构造。`MacroProcessor` 在加载过程中对每个源文件进行宏展开。

## 关键函数·方法

### `PackageManager::PackageManager(luna::macro::MacroProcessor macroProcessor)`

构造函数，通过 `std::move` 获取宏处理器实例。接受默认参数，意味着 `PackageManager()` 会构造一个使用默认 `ExpansionLimits` 的 `MacroProcessor`。

### `PackageManager::load(const PackageRequest& request, LoadedPackage& result, PackageGraph& graph, std::vector<diagnostic::Diagnostic>& errors) -> bool`

包加载的核心方法。其实现分为以下阶段（详细在 `PackageManager.cpp` 中）：

1. **输入路径验证**：检查文件或目录是否存在。
2. **清单解析**：对目录输入，查找 `luna.package` 并解析为 `PackageManifest`。
3. **源码收集**：根据清单或目录遍历收集所有 `.luna` 文件。
4. **宏展开与解析**：对每个源文件执行宏展开 → 词法分析 → 语法分析，产生 `Program`。
5. **依赖解析**：对声明了依赖的包，查找 workspace 和 lock 文件，递归加载依赖包。
6. **AST 合并**：将当前包及其依赖的所有声明合并到 `result.program` 中。

## 与周边文件·阶段的关系

```
PackageManager.h  →  Package.h（类型定义）
                  →  macro/MacroProcessor.h（宏展开）
                  →  diagnostics/Diagnostic.h（错误报告）
```

- **Package.h**：定义了 `PackageUse`、`ResolvedPackage`、`LoadedPackage`、`PackageManifest` 等核心类型，`PackageManager` 的输出类型。
- **PackageManager.cpp**：`load` 方法的完整实现，包含匿名命名空间中的辅助函数（TOML 解析、workspace 解析、lock 解析、源码读取与解析、依赖递归加载等）。
- **macro/MacroProcessor.h**：宏处理器，在加载阶段应用于每个源文件，在词法分析前完成宏展开。
- **diagnostics/Diagnostic.h**：`diagnostic::format` 用于生成结构化的错误诊断。

加载阶段在编译器流水线中的位置：

```
PackageManager::load
  ├── 路径验证
  ├── 清单解析（parsePackageManifest）
  ├── 源码收集（collectManifestSources）
  ├── 宏展开（MacroProcessor::process）
  ├── 词法分析（Lexer）
  ├── 语法分析（Parser）
  ├── 依赖解析（递归 loadDependency）
  └── AST 合并（assignDeclarationOwner + 声明迁移）
        ↓
  语义分析（Sema）
```

## 延伸阅读

- `PackageManager.cpp`：`load` 方法的完整实现，包含 TOML 解析器、依赖图构建、递归加载等
- `Package.h`：`PackageRequest`、`PackageGraph` 使用的类型
- `macro/MacroProcessor.h`：宏展开处理器
- `parser/AST.h`：`Program` 结构体定义
- `diagnostics/Diagnostic.h`：诊断错误格式


---
