# Luna 语言设计讨论总结：Trait、Dynamic、Label、版本与声明身份

> 本文用于交给 Codex 继续完善设计与实现。  
> 重点是明确当前已经达成的设计共识、尚未解决的问题，以及后续实现时应保持的边界。

---

## 1. 当前背景

Luna 当前已经具备或计划具备以下能力：

- `trait`
- 强大的编译期计算
- 静态反射
- 结构化类型
- 名义类型信息可读取
- `dynamic` 运行时重化能力
- `@label(...)` 标记系统
- 多版本声明
- 动态插件系统

当前尚未正式引入一个独立的「约束系统」。

此前部分讨论曾错误地把 constraint 当成已存在能力。当前正确前提是：

> Luna 目前有 trait，但没有独立 constraint 系统。

因此，后续设计不能预设已经存在 Haskell typeclass / Rust-style constraint solver 一类完整机制。

---

# 2. Trait 与未来约束系统

## 2.1 Trait 已经足以承担大部分能力表达

例如序列化可以通过 trait 实现：

```luna
trait Encode<Format> {
    fn encode(
        self: &Self,
        encoder: &mut Encoder<Format>
    ) -> Result<(), EncodeError>;
}

trait Decode<Format> {
    fn decode(
        decoder: &mut Decoder<Format>
    ) -> Result<Self, DecodeError>;
}
```

因此目前没有必要为了序列化单独引入：

```luna
serializable
```

关键字，也没有必要立即引入完整 constraint 系统。

Trait 可以承担：

- 行为能力
- 泛型条件
- 静态分派
- 动态 trait object
- 标准库协议
- 插件协议

未来如果引入 constraint，应该用于 trait 无法自然表达的编译期命题，例如：

```luna
sizeof(T) <= 16
fields_of<T>().all(...)
N > 0
T structurally_compatible_with U
```

因此：

> trait 表达「具备哪些操作」；  
> constraint 如果未来存在，应表达「满足哪些编译期命题」。

不要为了序列化或版本系统强行引入 constraint。

---

# 3. Dynamic 的统一语义

`dynamic` 不应仅表示传统意义上的动态分派。

更适合 Luna 的统一定义是：

> 被标记为 dynamic 的语言实体，在编译后仍保留足够的语言身份，使运行时可以重新识别、查询、选择、调用或重化该实体。

可能包括：

- dynamic value
- dynamic function
- dynamic fragment
- dynamic package
- dynamic trait object
- dynamic metadata / label
- dynamic declaration descriptor

例如动态声明可以被表示为：

```luna
DynamicDeclaration {
    name,
    callable_type,
    entry,
    labels,
    owner_package,
    type_descriptor,
}
```

这样动态插件可以直接通过语言自身的声明与元数据系统完成查询，不需要另建一套脱离语言的 manifest 系统。

---

# 4. Label 不应是写死特殊语法

目前类似：

```luna
fn go@test(1.3.2)() -> {}
```

的问题包括：

- 函数名、label、版本、声明身份混在一起
- 修改 label 或 version 很麻烦
- 语法像 name mangling
- `1.3.2` 被写死为特殊格式
- label 无法携带任意用户元数据
- 不利于静态反射与动态插件闭环

更合理的方向是：

> 将 `@` 提升为独立的、完整的二等元数据实体，而不是某种写死的特殊标记语法。

推荐形式：

```luna
@release(
    channel = stable,
    version = v"1.3.2"
)
fn go() -> i32 {
    return 132;
}
```

或者用户自定义：

```luna
meta release {
    channel: Channel;
    version: Version;
}
```

使用：

```luna
@release(
    channel = stable,
    version = v"1.3.2"
)
fn go() -> i32 {
    ...
}
```

这里：

- `go` 是函数名
- `release(...)` 是声明元数据
- `version` 是普通字段
- 版本机制不再由编译器硬编码
- label 字段由用户定义 schema 决定

---

# 5. 版本系统应移出编译器核心

版本机制不需要写死进编译器。

推荐由 `core` 或标准库提供：

```luna
struct Version {
    major: u32;
    minor: u32;
    patch: u32;
    prerelease: ...;
}
```

或者提供：

```luna
SemVer
```

及其：

- 字面量
- 比较
- 兼容性判断
- 范围匹配
- stable / dev / test channel
- latest 策略

例如：

```luna
v"1.3.2"
```

可以是标准库定义或 core 级字面量协议，而不是版本系统本身被编译器写死。

编译器不需要知道：

- 1.3.2 是否大于 1.2.9
- prerelease 如何排序
- stable 是否优先于 dev
- 哪个版本算 compatible

这些都可以交给库。

---

# 6. 但编译器仍需参与候选选择框架

虽然版本语义可完全移出编译器，但以下工作无法完全由普通库独立完成：

- 名称解析
- 收集同名声明候选
- 检查签名兼容性
- 识别多个候选
- 启动默认声明选择
- 生成最终符号引用

因此合理边界是：

> 编译器提供「声明候选集合与选择协议」；  
> core / 标准库定义「版本、排序、latest、兼容性、channel」等具体语义。

概念接口：

```luna
trait DeclarationSelector {
    comptime fn select(
        query: DeclarationQuery,
        candidates: DeclarationSet
    ) -> SelectionResult;
}
```

编译器只知道：

1. 有多个候选
2. 需要选择
3. 调用某个选择协议
4. 返回一个声明或报歧义

编译器不理解 `Version` 本身。

---

# 7. 留空时自动选择 latest

目标语义：

```luna
go();
```

在存在多个同名版本声明时，默认选择 latest。

例如：

```luna
@release(channel = stable, version = v"1.2.0")
fn go() -> i32 { ... }

@release(channel = stable, version = v"1.3.0")
fn go() -> i32 { ... }
```

调用：

```luna
go();
```

可在默认策略下选择：

```luna
go@release(channel = stable, version = v"1.3.0")
```

但必须明确：

> `latest` 是对候选集合计算得到的结果，不是某个声明的固有属性。

因此不建议：

```luna
@latest
fn go() {}
```

这会让声明自身声称自己是 latest，而 latest 会随着候选集合变化。

---

# 8. Label 应进入元数据系统，而不是普通类型系统

这是今晚最重要的结论之一。

需要区分以下概念：

## 8.1 函数类型

两个函数：

```luna
@release(version = v"1.2.0")
fn go() -> i32 {}

@release(version = v"1.3.0")
fn go() -> i32 {}
```

它们的 callable type 都应该仍然是：

```luna
fn() -> i32
```

因此可以赋值到同一种函数值：

```luna
let f: fn() -> i32;
```

Label 不应该污染普通函数类型。

---

## 8.2 声明身份

这两个函数必须是两个不同声明。

因此内部声明身份应包含：

```luna
DeclarationKey {
    namespace,
    name,
    callable_signature,
    discriminator_metadata,
}
```

例如：

```text
namespace::go
+ fn() -> i32
+ release(stable, 1.2.0)
```

与：

```text
namespace::go
+ fn() -> i32
+ release(stable, 1.3.0)
```

这里 label 进入的是：

- 声明身份系统
- 名称解析系统
- 候选选择系统
- 符号 mangling
- 动态 descriptor

而不是普通类型系统。

---

## 8.3 二进制符号身份

为了让链接器区分多个声明，mangled symbol 必须包含 discriminator 信息。

例如内部可生成：

```text
pkg.go.release.stable.1.2.0.<hash>
pkg.go.release.stable.1.3.0.<hash>
```

这是：

> declaration identity / linkage identity

不是：

> callable type

因此应该明确区分：

```text
TypeSignature
SourceSignature
DeclarationIdentity
LinkageIdentity
RuntimeDescriptor
```

不要继续把它们统称为「签名」。

---

# 9. 不是所有 label 都参与声明区分

如果所有元数据都参与声明身份，那么下面两个函数就可能同时存在：

```luna
@author(name = "A")
fn go() {}

@author(name = "B")
fn go() {}
```

这会导致：

- 普通文档元数据也能绕过重复定义
- 名称解析维度失控
- 用户可随意构造无意义重载
- 动态选择规则混乱

因此必须区分至少两类 label：

## 9.1 普通元数据

例如：

```luna
@author(name = "Yuan")
@documentation("...")
@benchmark(group = "parser")
@route(path = "/users")
```

这些通常不参与声明唯一性。

---

## 9.2 声明 discriminator / selector metadata

例如：

```luna
@release(
    channel = stable,
    version = v"1.3.0"
)
```

这类元数据可参与：

- 声明身份
- 候选选择
- 默认 latest
- 显式版本引用
- 动态插件选择

可以通过 trait 表示：

```luna
trait DeclarationDiscriminator {
    fn key(self) -> ComparableKey;
}
```

例如：

```luna
impl DeclarationDiscriminator for release {
    fn key(self) {
        return (self.channel, self.version);
    }
}
```

没有实现该 trait 的普通元数据，不进入声明身份。

---

# 10. 同一声明族应有明确选择域

假设出现：

```luna
@release(version = v"1.0.0")
fn go() {}

@platform(os = linux)
fn go() {}
```

即使 `release` 和 `platform` 都能参与筛选，也不代表它们应该独立作为不同声明族主键。

建议为同名同 callable signature 的声明族确定一个主 discriminator 类型。

概念结构：

```luna
Declaration {
    base_key: {
        namespace,
        name,
        callable_signature,
    },

    discriminator: {
        metadata_type,
        key,
    },

    metadata: [...],
}
```

规则：

```text
base_key 相同
且 discriminator key 相同
=> 重复定义
```

```text
base_key 相同
=> 主 discriminator metadata type 必须一致
```

其他 metadata 可作为：

- eligibility filter
- documentation
- runtime metadata
- plugin capability
- platform restriction

而不是声明族主身份。

---

# 11. Dynamic Label 是动态插件闭环的必要条件

如果 label 只在编译期存在，那么动态插件加载后，宿主无法知道：

- 插件版本
- ABI 版本
- capability
- platform
- priority
- deprecated 状态
- 用户自定义元数据

最终仍需要额外手写 manifest，造成两套系统：

```text
语言内 @label
插件外 manifest
```

这会破坏闭环。

因此应支持：

> 编译期附着的 label，可选择保留为运行时动态元数据。

例如：

```luna
dynamic meta release {
    channel: Channel;
    version: Version;
}
```

或者使用 retention 属性：

```luna
meta release @runtime {
    channel: Channel;
    version: Version;
}
```

结合 Luna 现有 dynamic 设计，更推荐：

```luna
dynamic meta release
```

其含义应统一为：

> 该元数据类型及其实例在编译后仍保留足够的语言身份，可被动态查询、比较、解码和参与声明选择。

---

# 12. Dynamic Label 的运行时表示

概念结构：

```luna
DynamicLabel {
    label_type_id,
    schema_id,
    schema_version,
    payload,
    owner_package,
    source,
}
```

动态声明：

```luna
DynamicDeclaration {
    name,
    callable_type,
    entry,
    labels: [DynamicLabel],
    owner_package,
}
```

插件运行时可执行：

```luna
plugin
    .exports_named("go")
    .filter(has_label<release>)
    .filter(|decl|
        decl.label<release>().channel == stable
    )
    .max_by(|a, b|
        compare(
            a.label<release>().version,
            b.label<release>().version
        )
    );
```

这使静态与动态选择共享同一套规则：

```text
静态候选集合
→ 编译期 selector
→ declaration
```

```text
动态候选集合
→ 运行时 selector
→ DynamicDeclaration
```

---

# 13. Dynamic Label 不应默认可变

第一阶段更安全的设计是：

```text
编译期构造
运行时只读
```

即：

```text
@label(...)
→ 编译进 descriptor
→ runtime inspect
```

不要默认支持运行时随意向已编译声明添加 label。

原因包括：

- 权限问题
- 线程安全
- 缓存失效
- 声明选择结果变化
- 安全标签伪造
- ABI / capability 欺骗
- 包签名失效

特别是：

```luna
@trusted
@permission(...)
@abi(...)
@sandbox(...)
```

不能被插件自行动态伪造。

DynamicLabel 应保留来源：

```luna
source = compiler
source = package
source = host
source = runtime
```

安全策略不能只检查 label 值，还要检查其 issuer / source。

---

# 14. Runtime Label 的字段约束

如果 label 要运行时保留，其字段必须能稳定进入运行时表示。

不能直接携带仅编译期存在的对象，例如：

- ASTNode
- compiler SymbolHandle
- source token
- 临时编译器内部引用

可以要求字段实现类似：

```luna
trait RuntimeMetadata {}
```

例如标准库为以下类型实现：

```text
integer
bool
String
Symbol
Version
List<T>
Map<K, V>
用户自定义可运行时描述类型
```

但这仍应通过 trait / core protocol 表达，不要在编译器中写死具体类型列表。

---

# 15. Label Schema 也需要兼容机制

动态插件跨版本加载时，label schema 可能变化。

例如旧版：

```luna
meta release {
    version: Version;
}
```

新版：

```luna
meta release {
    channel: Channel;
    version: Version;
    deprecated: bool;
}
```

因此 DynamicLabel 至少需要：

```text
label_type_id
schema_id
schema_version
payload
```

宿主可以：

- 精确解码
- 兼容旧 schema
- 迁移
- 退化到通用字段查询
- 拒绝不兼容插件

---

# 16. 显式选择旧版本

默认调用：

```luna
go();
```

可选择 latest。

但必须提供显式声明引用语法。

候选方案：

```luna
go@release(
    channel = stable,
    version = v"1.2.0"
)();
```

或：

```luna
let old_go: fn() -> i32 =
    go@release(version = v"1.2.0");
```

选择完成后，如果得到普通函数值：

```luna
fn() -> i32
```

则 label 身份可以被擦除。

如果用户需要保留声明身份，应使用：

```luna
DeclarationRef<fn() -> i32>
```

或 dynamic declaration：

```luna
dynamic fn() -> i32
```

其中可以查询：

```luna
decl.labels()
decl.owner()
decl.metadata()
decl.version()
```

---

# 17. require 语句目前没有必要立即加入

曾考虑：

```luna
require(namespace::go@test(1.3.2))
```

但当前尚未明确它解决什么核心问题。

普通调用本身已经会验证候选是否存在：

```luna
go@release(version >= v"1.3.0")();
```

因此 `require` 只有在表达下面语义时才真正有价值：

> 即使当前代码暂时没有调用，也必须在编译期确认某个依赖声明、能力、ABI 或插件契约存在。

例如：

```luna
require namespace::go
    @release(version >= v"1.3.0");
```

可能用于：

- package 依赖验证
- ABI 检查
- 插件契约
- feature 需求
- 提前生成依赖清单

当前建议：

> 先实现 metadata、声明身份、候选选择、显式查询与 dynamic descriptor，再决定是否需要 require。

不要先创造没有明确语义的关键字。

---

# 18. 可复现构建问题

如果：

```luna
go();
```

总是绑定到环境中的 latest，那么依赖更新后同一份源码可能自动改变行为。

因此应考虑：

```text
源码默认 latest
+
构建解析结果锁定
```

例如 lockfile 或编译结果记录：

```text
namespace::go
selected = release(stable, 1.3.2)
declaration_hash = ...
```

重新构建时使用锁定结果，只有显式 update 时重新解析 latest。

否则默认 latest 会方便，但会破坏 reproducible build。

---

# 19. 当前推荐的统一模型

## 19.1 语言核心提供

- 元数据类型定义
- `@meta(...)` 附着
- metadata schema
- 编译期 metadata 反射
- dynamic metadata retention
- 声明候选集合
- 声明身份模型
- 显式 metadata 查询
- 可插拔声明选择协议
- dynamic declaration descriptor
- 声明链接身份 / symbol mangling

---

## 19.2 Core / 标准库提供

- Version / SemVer
- 版本比较
- 版本范围
- stable / dev / test channel
- latest
- latest stable
- compatible version
- release metadata
- release selector
- schema migration helpers
- plugin metadata helpers

---

## 19.3 用户可以定义

- 自定义 metadata
- 自定义版本类型
- 自定义 selector
- 自定义 channel
- 自定义 plugin capability
- 自定义 ABI 描述
- 自定义 runtime metadata
- 自定义声明 discriminator

---

# 20. 建议语法草案

## Metadata 定义

```luna
meta author {
    name: String;
}
```

```luna
dynamic meta release {
    channel: Channel;
    version: Version;
}
```

---

## Trait 声明其可参与声明身份

```luna
impl DeclarationDiscriminator for release {
    fn key(self) {
        return (self.channel, self.version);
    }
}
```

---

## 函数声明

```luna
@author(name = "Yuan")
@release(
    channel = stable,
    version = v"1.3.2"
)
fn go() -> i32 {
    return 132;
}
```

---

## 默认选择

```luna
go();
```

---

## 精确选择

```luna
go@release(
    channel = stable,
    version = v"1.3.2"
)();
```

---

## 范围选择

```luna
go@release(
    channel = stable,
    version >= v"1.2.0",
    version < v"2.0.0"
)();
```

---

## 动态插件查询

```luna
let plugin = dynamic import "./plugin.so";

let go =
    plugin
        .exports_named("go")
        .select(release::latest_stable);
```

---

# 21. Codex 后续应重点分析的问题

Codex 后续应结合当前仓库实现，重点确认以下事项。

## 21.1 当前 label 的 parser 与 AST 表示

检查：

- label 是否直接写死为 name + version
- version 是否为 parser 特殊节点
- label 是否与函数名绑定
- label 是否进入 symbol name
- 同名同签名声明当前如何判重
- label 当前是否可被静态反射读取

---

## 21.2 当前 symbol table / declaration key

确认当前声明唯一键是否为：

```text
namespace + name + callable signature
```

如果是，需要设计新的：

```text
base declaration key
+
optional discriminator metadata key
```

但不要修改普通 callable type。

---

## 21.3 Type system 与 declaration identity 解耦

必须避免：

```text
fn@release(1.3.2)() -> i32
```

成为普通函数类型。

检查：

- 函数值类型
- 函数指针类型
- trait 方法类型
- dynamic function descriptor
- overload resolution
- symbol mangling

确保 label 只影响 declaration identity，不污染 callable type。

---

## 21.4 Metadata schema

设计一个通用 metadata AST / IR：

```luna
MetadataInstance {
    metadata_type,
    arguments,
    retention,
}
```

字段必须支持：

- named arguments
- positional sugar
- default values
- user-defined types
- compile-time evaluation
- reflection
- runtime retention
- schema id

---

## 21.5 Declaration selector protocol

设计：

```luna
DeclarationSet
DeclarationQuery
SelectionContext
SelectionResult
```

编译器应只负责：

- 收集候选
- 调用 selector
- 检查结果唯一性
- 生成符号引用

不要把 SemVer 比较写进编译器。

---

## 21.6 Dynamic metadata descriptor

确认现有 dynamic / plugin / descriptor 机制如何扩展为：

```luna
DynamicDeclaration {
    name,
    type_descriptor,
    entry,
    metadata,
}
```

并研究：

- metadata payload 编码
- schema id
- type id
- package owner
- ABI stability
- runtime query
- dynamic selector

---

## 21.7 安全与权限

动态 label 不能被当作无条件可信事实。

需要区分：

- self-declared metadata
- compiler-issued metadata
- package-signed metadata
- host-granted metadata
- runtime-added metadata

权限系统不能只看：

```luna
@trusted
```

还要验证其来源。

---

## 21.8 构建锁定

评估是否需要：

- package 级 lockfile
- declaration 级 selection lock
- declaration hash
- plugin ABI hash
- metadata schema hash

确保默认 latest 不破坏可复现构建。

---

# 22. 当前明确结论

1. Luna 当前有 trait，但没有独立约束系统。
2. 不应为了序列化或版本系统立即引入 constraint。
3. `@label` 应升级为用户可定义的二等元数据类型。
4. 版本机制应由 core / 标准库提供，而非编译器写死。
5. 编译器仍需提供声明候选与选择框架。
6. 留空调用可以默认选择 latest，但 latest 规则应由库实现。
7. Label 不应进入普通类型系统。
8. 一部分 label 可以进入声明身份系统。
9. 普通 metadata 与 declaration discriminator 必须区分。
10. 同名同 callable signature、不同 discriminator 的声明可以共存。
11. 内部 linkage identity 必须包含 discriminator。
12. dynamic label 是彻底动态插件系统闭环的必要条件。
13. dynamic label 第一阶段应编译期构造、运行时只读。
14. label schema 跨插件边界时必须考虑版本兼容。
15. `require` 暂时没有充分理由，应延后设计。
16. 默认 latest 必须配合锁定机制保证可复现构建。

---

# 23. 一句话设计原则

> 函数类型描述“如何调用”，声明身份描述“调用的是谁”，metadata 描述“这个声明具有什么信息”，dynamic 决定“这些语言身份是否在编译后继续存在”。



---

# 24. Runtime 与 Dynamic 职责重构（新增）

> 本章为新的设计共识，若与前文 Dynamic 定义冲突，以本章为准。

## 三层对象模型

普通对象 → Runtime 对象 → Dynamic 对象

Runtime 保留：
- Runtime Descriptor
- Entry Pointer
- Signature
- Runtime Metadata
- Owner
- Lifecycle

Runtime 提供：
- Runtime Select
- Runtime Registry
- Runtime Metadata Query

Runtime 不负责：
- AST Reflection
- Declaration Reflection
- Generic Reflection
- Replace
- Runtime Weaving

Dynamic = Runtime + Full Runtime Reflection。

因此 Dynamic Select 实际依赖 Runtime，而不是 Dynamic。

真正需要的是：
- Descriptor
- Entry
- Signature
- Runtime Metadata

Metadata 建议分层：

meta
→ runtime meta
→ registered runtime meta

后续全文建议统一将 dynamic meta 更名为 runtime meta。

---

# 25. Luna 编译器层级模型（新增）

Compiler
→ Core
→ Standard Library
→ Runtime
→ Dynamic

## Compiler

负责：
- Parser
- AST/HIR/MIR
- 类型检查
- Trait 求解
- 编译期计算
- 静态反射
- 名称解析
- Candidate Collection
- LLVM IR

不负责：
- SemVer
- latest
- Plugin Manifest
- Runtime Reflection

Compiler 仅提供 Declaration Set 与 Selector Protocol。

## Core

负责语言抽象：
- Trait
- Runtime Object
- Runtime Descriptor
- Metadata Protocol
- Declaration Protocol

## Standard Library

负责语言策略：
- Version
- SemVer
- latest
- Channel
- Compatibility
- Release Metadata
- Selector

## Runtime

负责运行时表示：
- Descriptor
- Runtime Metadata
- Registry
- Pool
- Runtime Select

## Dynamic

负责：
- Inspect
- Replace
- Runtime Weaving
- Full Runtime Reflection

原则：

Compiler 负责语言正确性；
Core 负责语言抽象；
Standard Library 负责语言策略；
Runtime 负责运行时表示；
Dynamic 负责完整运行时反射。

每一种能力，只支付对应能力的成本。
