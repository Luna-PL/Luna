# MoonIR、Metadata 与按需运行时重构 RFC

> 状态：Accepted / Core implemented v0.3（2026-07-22）  
> 范围：MoonIR、Metadata、Selector、Runtime/Dynamic、泛型实例化、Kernel 按需生成  
> 优先级：本文件记录本轮重构的新约束；与旧版 versioning/label 草案冲突时，以本文件为准。

## 1. 目标

本轮重构建立唯一的后端输入 MoonIR，并用通用 Metadata 与 Selector 机制替代编译器内建的 SemVer/versioning。所有运行时能力都必须显式启用，且成本可定位、可解释、可裁剪。

最终编译路径：

```text
Luna Source
  -> Lexer / Parser / AST
  -> Name Resolution / Sema / Trait / Ownership
  -> Luna-to-Moon lowering
  -> MoonIR verification
  -> MoonIR optimization
  -> LLVM lowering
  -> JIT or AOT
```

后续预留路径：

```text
MoonIR
  -> Moon container writer
  -> .moon
  -> MoonRuntime loader
  -> validation
  -> hotspot optimization / JIT
```

本轮不实现 MoonRuntime、容器二进制格式和 hotspot 优化器，但 MoonIR 必须为它们保留稳定边界。

## 2. 重构前实现与目标之间的差距

本节保留重构前的问题清单，用于说明迁移原因；不代表当前实现状态。重构前仓库直接以 `Program(AST) + SymbolTable` 驱动 LLVM `CodeGenerator`。版本机制同时侵入：

- Lexer 的 `VersionLiteral`；
- AST 的 `SemanticVersion`、`VersionTag`、`VersionSelector`；
- Parser 的声明、调用、类型、Trait、Fragment 和 Kernel 选择语法；
- Sema 的版本候选表、SemVer 比较和 latest 策略；
- LLVM linkage name；
- 示例、测试和文档。

当前泛型已经只为实际调用生成单态实例，但实例化发生在 Sema 修改 AST 的阶段。当前 Kernel 会生成所有声明，并且 AOT `main` 无条件引用 `rt_gpu_*`；驱动也在读取源文件之前初始化 GPU。这些行为违反按需付费原则。

## 3. MoonIR 定位

MoonIR 是 Luna 的语言级、已定型中间表示，不是 LLVM IR 的轻量包装，也不是持有 AST 指针的适配层。

MoonIR 必须：

- 保留执行一个 Luna 程序所需的完整语言语义；
- 已完成名称解析、类型解析、Trait 求解、所有权判定和静态 Selector 绑定；
- 使用稳定 declaration/type/metadata identity，不依赖源码位置作为身份；
- 显式表达控制流、所有权操作、泛型实例化请求、Apply/Select 和 Kernel；
- 将运行时保留信息与普通静态信息分区；
- 可独立验证；
- 不依赖 Parser AST、Sema SymbolTable 或 LLVM 类型；
- 能被 LLVM AOT/JIT 后端和未来 Moon 容器写出器共同消费。

MoonIR 不保留：

- 已经完成的名称查找过程；
- Trait/类型求解的搜索过程；
- 默认静态 Metadata 的运行时副本；
- 未被请求的运行时能力；
- 仅用于 Luna 源码恢复的语法细节。

### 3.1 初始模块模型

```text
Module
  format_version
  feature_flags
  target_assumptions
  types
  metadata_schemas
  declarations
  generic_recipes
  runtime_descriptors
  functions
  kernels
```

每个声明至少具有：

```text
DeclarationId
DeclarationFamilyId
DeclarationKind
CallableOrTypeShape
LinkageIdentity
StaticMetadata[]
Retention
CapabilityFlags
```

### 3.2 验证分层

MoonIR verifier 分为：

1. 结构验证：ID、引用、块、终结指令和类型存在。
2. 类型验证：操作数、返回值、调用签名和所有权操作一致。
3. 能力验证：未声明 Dynamic/Kernel 等能力时不得出现对应指令。
4. 保留验证：运行时查询只能访问被保留的 Metadata。
5. 容器安全验证（预留）：资源上限、允许的指令集、导入能力和 ABI。

未来 MoonRuntime 必须先验证后执行；后端不能以“LLVM verifier 会处理”为理由跳过 MoonIR 验证。

## 4. Metadata 语言模型

`meta` 声明一个具名、类型化的 Metadata schema。Metadata 是语言一等公民：它有类型、有值、可作为编译期参数传递和由静态反射读取；默认不进入运行时。

```luna
meta version {
    major: i32;
    minor: i32;
    patch: i32;
}

@version(1, 2, 1)
fn func() -> i32 {
    return 121;
}
```

初始规则：

- Metadata schema 是声明。
- `version(1, 2, 1)` 构造一个 `version` 值。
- `@value` 将 Metadata 值附加到紧随其后的声明。
- 位置参数按字段声明顺序绑定；后续可增加命名参数和默认值。
- 参数在默认模式下必须可编译期求值。
- Metadata 不进入普通 callable type。
- Metadata 是否参与 declaration identity 由 Selector/声明族规则决定，不能由任意文档 Metadata 自动绕过重复定义检查。

### 4.1 Retention 是附着实例的属性

```luna
runtime@version(1, 2, 1)
fn func() -> i32 { ... }
```

表示这个 `version` 实例进入目标声明的 Runtime Descriptor。普通 `@version(...)` 只存在于编译期。

建议把保留级别建模为附着实例属性，而不是 schema 的永久属性：同一 schema 可以在某些声明上静态使用，在另一些声明上运行时保留，避免全局成本扩散。

初始保留等级：

```text
compile_time < runtime < dynamic
```

`runtime@meta(...)` 至少使目标声明具有最小 Runtime Descriptor；否则运行时没有承载该实例的位置。

## 5. Declaration、Family 与 Selector

Metadata 与函数类型分离：

```text
CallableType       = 如何调用
DeclarationId      = 调用哪一个声明
DeclarationFamily = 哪些声明构成候选集合
LinkageIdentity    = 如何在二进制中唯一链接
Metadata           = 声明携带的信息
```

Selector 是普通 Luna 函数。编译器提供只读候选视图类型，并负责：

- 构造目标的候选集合；
- 按调用上下文先做种类、可见性和结构签名过滤；
- 调用 selector；
- 验证返回值属于输入候选集合；
- 验证最终恰好得到一个合法解；
- 把静态结果固化为 `DeclarationId`。

正式语法：

```luna
let f = select func with choice(1, 2, 2);
```

语法糖：

```luna
let f = @choice(1, 2, 2) func;
@choice(1, 2, 2) func();
```

语法糖必须先脱糖为正式 `select ... with ...`，不能单独实现一套版本选择路径。

### 5.1 建议的内建边界类型

```luna
compiler type DeclarationView<T>;
compiler type DeclarationRef<T>;
compiler type Selection<T>;
```

- `DeclarationView<T>`：只读、有限、由编译器维护的候选列表视图。
- `DeclarationRef<T>`：携带 declaration identity 的已验证引用。
- `Selection<T>`：成功、无解或多解；编译器最终只接受唯一成功结果。

编译器向 selector 隐式注入第一个 `DeclarationView<T>` 参数，使源码调用保持 `choice(1, 2, 2)`。Selector 本身仍是可独立类型检查和测试的普通函数。

### 5.2 静态与动态 Select

- `select`：编译期执行，结果在 MoonIR 中成为固定 `DeclarationId`，运行时零选择成本。
- `dynamic select`：运行时执行，MoonIR 显式包含候选 descriptor set、selector 调用和 binding slot。
- Dynamic selector 只能读取被 `runtime@...` 或更高等级保留的信息。
- 绑定完成后的普通调用退化为间接指针调用；不默认持续重选。
- Plugin load/unload、显式 reselect 和未来 hotspot 生命周期必须使缓存失效规则可见。

## 6. Runtime 与 Dynamic 的建议权利边界

采用以下最小、可实现且成本明确的划分：

| 能力 | 普通 | runtime | dynamic |
|---|---:|---:|---:|
| 正常执行 | 是 | 是 | 是 |
| 静态 Metadata | 编译期 | 编译期 | 编译期 |
| 最小 Descriptor | 否 | 是 | 是 |
| 查询保留的 Metadata | 否 | 是 | 是 |
| Registry/发现 | 否 | 显式注册时 | 是 |
| Runtime Select 候选 | 否 | 是 | 是 |
| 完整声明/泛型反射 | 否 | 否 | 是 |
| Replace/Inspect/Weaving | 否 | 否 | 是 |
| 运行时 Apply/Select 操作 | 否 | 否 | 显式操作点 |
| 携带可供 JIT 的泛型/MoonIR recipe | 否 | 可选能力位 | 是 |

冻结语义：

- `runtime` 修饰对象：保留最小可执行身份；默认只读，不代表可替换。
- `dynamic` 修饰对象：`runtime` 的超集，保留完整重化与修改所需信息。
- `dynamic apply` / `dynamic select` 修饰操作点：要求宿主生成运行时调度、验证、绑定和失效处理。
- “对象可被动态发现”和“某次操作在运行时发生”分开表达，防止一个 `dynamic` 让所有调用点永久付费。

## 7. 泛型与插件

AOT 默认只为可达的具体实例生成机器码：

```text
generic declaration
  -> compile-time instantiation request
  -> concrete MoonIR function
  -> LLVM function
```

未实例化的泛型声明不进入普通 AOT 机器码。

插件若希望在运行时提供新泛型实例，必须自行携带以下至少一种能力：

- 已生成的具体实例；
- 可验证的 MoonIR generic recipe 与所需静态声明；
- 自带且与宿主协商过的生成/JIT 能力。

宿主不因为存在泛型而默认保留 JIT。只有 `dynamic apply`、`dynamic select` 或显式 `--reserve-*` 能力要求宿主预留对应运行时入口。

## 8. Kernel 按需付费

默认策略：

- 未声明 Kernel 且未启用预留 flag：不初始化 GPU、不引用 `rt_gpu_*`、不链接 GPU 所需入口。
- 声明但不可达的 Kernel：不生成 host wrapper、PTX 或 HSACO。
- 被可达 `launch` 使用的 Kernel：只生成对应 Kernel 和必需 runtime 调用。
- 显式编译 flag：即使当前没有可达 launch，也保留 Kernel 动态装载/调用能力。

flag 名称为：

```text
--reserve-kernel-runtime
```

该 flag 表达二进制成本，而不是选择某个 GPU backend；backend 仍由目标/运行配置决定。

## 9. Moon 容器与 Hotspot 预留

Moon 容器至少需要独立区段：

```text
header
feature manifest
type/schema table
MoonIR code
static metadata (可裁剪)
runtime metadata
dynamic reflection (可选)
imports/exports/capabilities
integrity/signature (预留)
profile/hotspot data (可替换区段，预留)
```

动态进化不应允许任意修改已验证代码。建议未来采用：

1. 原始 MoonIR 保持不可变；
2. hotspot profile 独立积累；
3. 优化版本作为带来源和 guard 的新 code version；
4. MoonRuntime 验证新版本的类型、能力、ABI 和资源上限；
5. 原子切换 binding，并保留回滚目标。

## 10. 分阶段迁移

### Phase A：冻结模型并止住无条件成本

- 建立 MoonIR 类型、Module、Verifier 和文本 dump。
- 修正纯 CPU 编译路径的 GPU 初始化与 `rt_gpu_*` 引用。
- 建立回归测试，证明无 Kernel 的 LLVM IR 不含 GPU 符号。

### Phase B：切断后端对 Luna 的依赖

- 实现 Luna-to-Moon lowering。
- 将现有 LLVM CodeGenerator 改为只消费 MoonIR。
- JIT/AOT 都从同一 MoonIR Module 进入 LLVM。
- 将泛型实例化结果显式化为 MoonIR concrete function。

### Phase C：替换旧 versioning

- 引入 `meta` schema 和通用 Metadata value/attachment。
- 引入 declaration family、candidate view 和静态 selector。
- 实现正式 `select` 与 `@selector` 糖。
- 删除编译器内建 SemanticVersion/latest。
- 迁移示例、测试与文档。

### Phase D：Runtime/Dynamic

- 生成最小 Runtime Descriptor 和 runtime metadata table。
- 引入 MoonIR `dynamic.select` / `dynamic.apply` 指令。
- 实现显式 binding slot 与运行时验证边界。
- 完整 Dynamic Reflection、Replace 和 MoonRuntime 留待后续 RFC。

### Phase E：Kernel 可达性与能力预留

- 从可达 `launch` 收集 Kernel。
- 只生成所需 device code。
- 实现 `--reserve-kernel-runtime`。
- 验证普通二进制零 GPU runtime 符号。

## 11. 兼容与迁移原则

- 不保留旧 `@tag(1.2.3)` 的隐式 SemVer 语义。
- 旧语法应产生带迁移提示的错误，而不是静默解释为新 Metadata。
- 旧版本示例改写为显式 `meta version` + selector。
- callable type 不因 Metadata 改变。
- 所有动态能力在 MoonIR feature flags 和最终制品中可审计。

## 12. 已冻结决策

1. Selector 显式声明 `DeclarationView<T>` 首参，调用点由编译器隐式注入。
2. `runtime@version(...)` 隐式使被附着声明成为 Runtime 对象。
3. Dynamic Metadata 第一阶段仍只读；修改只能通过受验证的 Replace/Weaving 事务产生新声明版本。
4. 未限定的同名候选第一阶段报歧义；默认策略以后由模块或标准库显式导入。
5. Kernel 能力预留 flag 使用 `--reserve-kernel-runtime`。

## 13. 独立编译器组件

以下组件必须具有独立目录、数据模型和接口，不能作为 Parser、Sema 或 Driver 中的一组条件分支存在。

```text
MacroProcessor
  -> expanded token/source unit
Parser / Sema
  -> resolved declarations
Selector
  -> selected DeclarationId
Instantiator
  -> concrete declaration / MoonIR function
MoonIR
  -> backend

PackageManager
  -> package graph + source units + dependency capabilities
  -> feeds MacroProcessor and compiler pipeline
```

### 13.1 Selector

负责：

- declaration family 与候选视图；
- selector 调用协议；
- 静态选择求值；
- 唯一解与候选归属验证；
- dynamic select plan 的生成；
- selection lock/hash 的预留接口。

Selector 不负责名称解析、SemVer 策略、LLVM 符号生成或 Registry 实现。

### 13.2 Instantiator

负责：

- 泛型实例化请求；
- 类型/值/Metadata 参数绑定；
- 实例缓存与稳定实例 ID；
- 递归实例化检测；
- 生成具体的已解析声明；
- 向 MoonIR 报告实例化原因和请求点。

Instantiator 不负责泛型语法解析、Trait 策略、LLVM 单态化或运行时 JIT 策略。

### 13.3 Package Manager

负责：

- package/module graph；
- source unit 枚举与确定性顺序；
- dependency、lock、feature/capability 与 artifact 解析；
- source/Moon/binary package 的统一描述；
- 将加载结果交给编译管线。

现有 `PackageLoader` 只承担本地源码加载，是 Package Manager 的第一阶段实现，不再让 Driver 自行拼装包语义。

Package Manager 不负责 Parser、Metadata 策略、动态插件 Registry 或链接器实现。

### 13.4 Macro Processor

负责：

- 在 Parser 前执行的 token/source transformation；
- hygiene、expansion provenance、递归/资源上限；
- 宏输入输出的确定性；
- 后续编译期宏能力与包权限的隔离。

第一阶段提供 no-op processor 和稳定接口，使当前无宏程序行为不变。宏不得直接操作 LLVM，也不得绕过 MoonIR verifier 或声明权限检查。

### 13.5 依赖方向

四个组件共享稳定的 Core IDs/diagnostics，但不得循环依赖：

```text
PackageManager -> MacroProcessor -> Frontend
Frontend -> Selector
Frontend -> Instantiator
Selector -> declaration protocol
Instantiator -> resolved type/declaration protocol
Frontend -> MoonIR Lowerer
MoonIR -> LLVM backend / future Moon writer
```

Selector 与 Instantiator 都必须可在不构造 LLVM Context 的情况下单元测试；Package Manager 与 Macro Processor 都必须可在不运行 Sema 的情况下单元测试。

## 14. 成本审计要求

每个 MoonIR Module 应能报告：

- 哪些 declaration 生成机器码以及原因；
- 哪些泛型实例被请求以及请求点；
- 哪些 Metadata 被保留到运行时；
- 哪些 Runtime/Dynamic descriptor 被生成；
- 哪些 dynamic operation 生成 binding/registry 成本；
- 哪些 Kernel/device code 被生成；
- 哪个 flag 导致能力预留。

这份报告是“只在需要付费时付费”的可验证接口，而不只是实现约定。

## 15. 本轮实现状态

已完成：

- 独立 `moonir` 模型、文本打印、成本报告、verifier 和 MoonIR-to-MoonIR optimizer 边界；
- Luna-to-Moon 降级，AOT/JIT LLVM 后端只接收 `moon::Module`；
- verifier 在 LLVM 之前校验稳定声明身份、Metadata schema/value/retention、动态候选和能力位；
- `meta` 声明、前置 Metadata 附着、`select ... with ...`、`@selector(...) target` 糖和唯一解规则；
- 移除编译器内建 SemVer/latest 选择路径，旧用例迁移为通用 Metadata；
- `runtime`/`dynamic` 保留级别、最小运行时 descriptor/metadata table、`dynamic select` 与 `dynamic apply` 显式成本；
- 独立 Selector、Instantiator、PackageManager 和 MacroProcessor 目录与稳定边界；
- 泛型实例缓存和稳定实例 ID，只为实际请求的具体类型生成代码；
- Kernel 可达性生成、`--reserve-kernel-runtime` 和纯 CPU 零 `rt_gpu_*` 引用；
- 成本边界回归，覆盖未使用 Kernel、显式预留、动态选择和泛型实例复用。

有意延期：

- `.moon` 二进制容器编解码、签名/完整性和 MoonRuntime loader；
- 容器级资源配额、导入 capability sandbox 和跨平台 ABI 协商；
- 完整 Dynamic Reflection、Replace/Weaving 事务、插件生命周期和绑定失效协议；
- 真实 hotspot profile、guarded code version 和原子切换；当前 optimizer 仅做 MoonIR 规范化；
- 完整宏语言、依赖解析/lockfile 和远程 Moon/binary package；当前 MacroProcessor 是受限 no-op 阶段，PackageManager 实现本地源码图。
