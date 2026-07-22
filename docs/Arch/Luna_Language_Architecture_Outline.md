# Luna Language Architecture Outline

> 状态：Draft v0.1  
> 定位：Luna 语言架构总纲  
> 作用：统一语言哲学、编译器层级、类型系统、声明系统、元数据、运行时、Dynamic、包系统、ABI 与安全模型。  
> 原则：后续 RFC、Specification 与实现文档可以细化本文件，但不应在未明确修订本文件的情况下与其冲突。

---

# 目录

1. 文档定位与规范层级  
2. 语言目标  
3. 设计哲学  
4. 成本模型  
5. 总体层级模型  
6. Compiler 层  
7. Core 层  
8. Standard Library 层  
9. Runtime 层  
10. Dynamic 层  
11. 核心语言  
12. 原语与数据表示  
13. 约束系统  
14. Trait 与能力抽象  
15. 类型系统  
16. 结构化类型与名义信息  
17. 泛型与编译期计算  
18. 所有权、线性与 Affine  
19. 声明系统  
20. 声明身份与声明族  
21. Selector 与候选选择  
22. 版本、Channel 与 latest  
23. Metadata 系统  
24. Runtime Metadata 与注册机制  
25. 静态反射  
26. Runtime Reflection  
27. Runtime Object Model  
28. Runtime Descriptor  
29. Runtime Registry 与 Pool  
30. Runtime Select  
31. Dynamic Reflection  
32. Replace、Inspect 与 Runtime Weaving  
33. Fragment、Slot 与 Apply  
34. Package 与 Module  
35. Plugin 系统  
36. ABI 与二进制兼容  
37. 安全、权限与信任边界  
38. 编译器实现管线  
39. Runtime 实现管线  
40. 标准库职责  
41. 工具链与工程组织  
42. RFC 流程  
43. 规范与实现分离  
44. 术语冻结  
45. 当前已确定原则  
46. 尚待讨论的问题  
47. 推荐文档结构  
48. 架构摘要

---

# 1. 文档定位与规范层级

Luna 的长期文档应分为四类：

```text
Philosophy
    ↓
Architecture
    ↓
Specification
    ↓
Implementation
```

- **Philosophy**：解释为什么这样设计。
- **Architecture**：规定系统由哪些层组成，以及各层职责。
- **Specification**：规定语法、静态语义、动态语义和可观察行为。
- **Implementation**：解释编译器、Runtime、工具链如何实现规范。

RFC 位于上述体系之外，作为设计变更的入口：

```text
Idea
  ↓
RFC
  ↓
Architecture / Specification
  ↓
Implementation
```

本文件属于 **Architecture**，不承担完整语法规范，也不绑定某个具体编译器实现。

---

# 2. 语言目标

Luna 的目标不是简单堆叠高级特性，而是形成一套统一、可扩展、可按需付费的语言体系。

核心目标：

- 静态优先。
- 结构化类型优先。
- 强编译期计算。
- 强静态反射。
- Runtime 能力显式启用。
- Dynamic 能力显式启用。
- 用户只为实际使用的能力支付成本。
- 语言策略尽量下放到 Core 或标准库，而非硬编码进编译器。
- 插件、版本、选择器、反射与元数据建立在统一抽象之上。
- 保持接近系统语言的性能与可预测性。
- 支持 AOT，并允许按需扩展 JIT 或动态装载。
- 允许语言能力在不破坏核心编译器的情况下扩展。

---

# 3. 设计哲学

## 3.1 Static First

能在编译期完成的工作，应优先在编译期完成。

包括：

- 类型检查。
- 结构比较。
- Trait 求解。
- 元数据查询。
- Selector 求值。
- 静态反射。
- 编译期生成。
- 可静态确定的插件织入。

Runtime 不应成为静态系统能力不足后的默认补丁。

## 3.2 Pay for What You Use

用户只为实际启用的能力支付：

- 编译时间。
- 二进制体积。
- Runtime Metadata。
- Descriptor。
- Registry。
- 完整运行时反射。
- 热替换与 Runtime Weaving。

## 3.3 Structural Type First

Luna 默认以结构判断兼容性。

名义信息可以：

- 提供额外约束。
- 参与元数据查询。
- 参与候选发现。
- 提供诊断与文档信息。

但名义信息不应默认取代结构兼容性。

## 3.4 Policy Outside Compiler

编译器负责语言正确性，不应硬编码可替换的语言策略。

例如：

- SemVer。
- latest。
- Channel。
- 兼容版本范围。
- 用户自定义 Selector 策略。

这些应尽量由 Core Protocol 与标准库实现。

## 3.5 Explicit Runtime

进入 Runtime 世界必须具有明确语义。

普通静态对象不应自动携带：

- Descriptor。
- Runtime Metadata。
- Registry Entry。
- 完整 Reflection 数据。

## 3.6 Explicit Dynamic

Dynamic 表示完整运行时反射与变更能力，不应与一般 Runtime 对象混为一谈。

## 3.7 Everything Extensible Through Metadata

元数据是 Luna 的统一扩展机制之一。

版本、权限、导出、插件发现、选择器条件、运行时保留策略等，都应尽可能由统一的 Metadata 模型表达，而不是为每种特性增加一套独立语法和内部表示。

---

# 4. 成本模型

Luna 的能力层次：

```text
Compile Time
    ↓
Runtime
    ↓
Dynamic
```

## 4.1 Compile Time

默认只存在于编译阶段：

- AST / HIR / MIR。
- 完整类型结构。
- 声明定义。
- 静态 Metadata。
- Trait 求解信息。
- 静态反射信息。
- 编译期执行状态。

编译结束后，未显式保留的信息可以全部删除。

## 4.2 Runtime

Runtime 对象需要最小运行时表示：

- Runtime Descriptor。
- Entry Pointer。
- Signature。
- Runtime Metadata。
- Owner。
- Lifecycle 信息。
- 必要时的 Registry Entry。

Runtime 支持执行、发现、选择和生命周期管理，但不默认支持完整声明反射。

## 4.3 Dynamic

Dynamic 是 Runtime 的超集：

```text
Dynamic
=
Runtime
+
Full Runtime Reflection
```

Dynamic 可进一步保留：

- Declaration Reflection。
- Generic Reflection。
- Replace 所需信息。
- Inspect 所需信息。
- Runtime Weaving 所需结构。
- 动态 Fragment / Slot 关系。
- 更完整的类型与调用信息。

## 4.4 最低层原则

任何能力都应放入能够实现它的最低层：

- 能在编译期完成，就不进入 Runtime。
- 只需要 Descriptor，就不保留完整 Reflection。
- 只需要 Runtime Select，就不要求对象成为 Dynamic。
- 只需要 Runtime Metadata，就不自动建立 Registry。
- 只需要候选发现，就不让名字参与类型身份。

---

# 5. 总体层级模型

Luna 的总体层级：

```text
Compiler
    ↓
Core
    ↓
Standard Library
    ↓
Runtime
    ↓
Dynamic
```

这里的箭头表示能力和依赖逐步增加，而不是简单的软件包调用关系。

每一层都必须回答：

1. 它负责什么？
2. 它不负责什么？
3. 它引入什么成本？
4. 它依赖哪些下层协议？
5. 它是否可以被替换？

---

# 6. Compiler 层

Compiler 负责证明程序满足语言规则，并把程序转换为可执行形式。

主要职责：

- Lexer 与 Parser。
- AST 构建。
- 名称解析。
- Module 与作用域解析。
- Declaration Collection。
- Candidate Collection。
- 类型推导。
- 结构化类型比较。
- Constraint 求解。
- Trait 求解。
- 所有权与生命周期检查。
- 编译期计算。
- 静态反射。
- HIR / MIR / LLVM IR 生成。
- 优化。
- AOT 代码生成。
- 诊断。

Compiler 不应负责：

- SemVer 的具体规则。
- latest 的具体排序。
- 用户级版本 Channel 策略。
- Plugin Manifest 的业务格式。
- 完整 Runtime Reflection。
- 长期运行的 Registry。
- Runtime Replace。
- 热更新策略。

Compiler 应提供通用协议和中间表示，使 Core 与标准库能够表达这些策略。

---

# 7. Core 层

Core 是语言稳定抽象的集合。

Core 应提供：

- 基础 Trait。
- Constraint Protocol。
- Type Reflection Protocol。
- Metadata Protocol。
- Declaration Protocol。
- Selector Protocol。
- Runtime Object Protocol。
- Runtime Descriptor Protocol。
- 基本 Ownership / Linear / Affine 抽象。
- 最基础的错误与结果类型。

Core 不应承载高层策略，例如：

- SemVer。
- HTTP。
- 文件系统策略。
- 插件仓库。
- 默认包管理策略。

Core 的目标是稳定、最小、难以被移除。

---

# 8. Standard Library 层

标准库负责通用策略和高层能力。

典型职责：

- Version。
- SemVer。
- Channel。
- latest。
- Compatibility。
- 常用 Selector。
- 集合。
- IO。
- Async。
- 字符串。
- 并发。
- Runtime Helper。
- Plugin Helper。
- 标准 Metadata Schema。

编译器不需要理解 SemVer 的内部规则，只需要理解：

- 某个 Selector 可以在编译期执行。
- 某个 Selector 可以在 Runtime 执行。
- Selector 的结果必须满足结构与签名要求。

标准库应尽可能可替换，但语言规范可以定义一套官方标准库。

---

# 9. Runtime 层

Runtime 表示对象进入运行时对象世界。

Runtime 负责：

- Runtime Object。
- Runtime Descriptor。
- Entry Pointer。
- Runtime Signature。
- Runtime Metadata。
- Registry。
- Pool。
- Runtime Select。
- 生命周期。
- Module Owner。
- 动态装载后的最小可执行管理。

Runtime 不负责：

- AST。
- 源码级声明反射。
- 完整 Generic Reflection。
- Replace。
- Runtime Weaving。
- 任意结构修改。

Runtime 的设计目标是轻量、稳定、可预测。

---

# 10. Dynamic 层

Dynamic 是 Runtime 的超集，不是 Runtime 的同义词。

Dynamic 负责：

- Full Runtime Reflection。
- Declaration Inspect。
- Generic Inspect。
- Replace。
- Runtime Weaving。
- Dynamic Apply。
- 热重载。
- 可恢复、可迁移的动态声明关系。
- 动态 Package / Fragment / Slot 操作。

一个对象可以是 Runtime 对象而不是 Dynamic 对象。

因此：

- Runtime Select 不应要求完整 Dynamic。
- Registry 不应要求完整 Dynamic。
- Runtime Metadata 不应要求完整 Dynamic。
- 只有需要完整反射或修改能力时才启用 Dynamic。

---

# 11. 核心语言

核心语言部分定义不依赖 Runtime 的基础能力：

- 声明。
- 表达式。
- 控制流。
- 模式匹配。
- 函数。
- 结构体。
- ADT。
- Trait。
- Constraint。
- Module。
- 泛型。
- 编译期求值。
- 所有权与生命周期。

核心语言的语义必须在不加载 Runtime 的情况下成立。

---

# 12. 原语与数据表示

当前原语设计方向：

```text
bit   = 1 bit
byte  = 8 bit
word  = 32 bit
```

上层数值语义由约束与解释方式决定：

- integer。
- float。
- char。

这里需要区分：

1. **存储宽度**：数据占用多少位。
2. **解释语义**：数据按整数、浮点、字符或其他方式读取。
3. **约束**：允许的值域、运算与转换。
4. **映射**：上层类型如何映射到底层表示。

该模型目标是让用户能够通过约束、静态反射与标准库建立新的数值和数据类型，而不必为每种类型增加新的编译器内建。

---

# 13. 约束系统

Constraint 是 Luna 的一等公民。

Constraint 可用于描述：

- 值域。
- 类型结构。
- 内存布局。
- 可调用能力。
- 所有权性质。
- 泛型要求。
- ABI 要求。
- Metadata 要求。

Constraint 应支持：

- 声明。
- 组合。
- 推导。
- 编译期验证。
- Trait 与结构条件共同求解。
- 必要时的 Runtime 检查。

模块内部可以进行更强推导；模块边界需要固定、稳定、可导出的签名。

---

# 14. Trait 与能力抽象

Trait 用于描述能力，而不是替代结构化类型。

Trait 系统需要明确：

- Trait 声明。
- 实现匹配。
- 自动推导。
- 冲突规则。
- 泛型约束。
- Associated Item。
- 默认实现。
- Trait Object 是否存在，以及其 Runtime 成本。
- Trait 与 Metadata 的关系。
- Trait 与 Structural Matching 的优先级。

Trait 应主要服务于行为抽象；结构兼容仍由类型结构决定。

---

# 15. 类型系统

类型系统包括：

- Primitive。
- Tuple。
- Array。
- Slice。
- Function / Callable。
- Struct。
- ADT。
- Generic。
- Reference。
- Pointer。
- Ownership-qualified Type。
- Constraint-qualified Type。
- Structural Type。
- Nominal Information。

类型检查应区分：

- 类型身份。
- 类型兼容性。
- 可转换性。
- ABI 兼容性。
- Runtime 可替换性。

这些概念不可混为一谈。

---

# 16. 结构化类型与名义信息

Luna 默认采用结构化类型。

结构兼容由以下信息决定：

- 字段结构。
- 字段类型。
- 调用签名。
- Constraint。
- 所有权要求。
- 必要的布局条件。

名义信息可以作为：

- Metadata。
- 文档标识。
- 诊断标识。
- Candidate Discovery 条件。
- 显式 nominal constraint。
- ABI / 安全策略的一部分。

但默认情况下：

> Name 不参与 Type Identity，只参与 Candidate Discovery。

运行时或编译期通过名字发现候选后，仍必须进行结构与签名验证。

---

# 17. 泛型与编译期计算

泛型应与静态反射和 Constraint 求解统一。

需要定义：

- 类型泛型。
- 值泛型。
- Metadata 泛型。
- Declaration 泛型。
- 泛型实例化。
- 单态化。
- 擦除策略。
- 泛型 Runtime Representation。
- 泛型在 Dynamic Reflection 中的保留级别。

默认情况下，泛型信息只在编译期存在。

只有 Dynamic 或显式 Runtime Reflection 需求，才保留完整泛型结构。

---

# 18. 所有权、线性与 Affine

当前设计方向：

- 线性语义主要作用于绑定级。
- 后续引入 Affine，允许资源至多使用一次。
- 所有权与结构化类型必须协调。
- 所有权信息应可参与 Constraint。
- 模块边界需要稳定的所有权签名。
- Runtime Object 与 Dynamic Object 的生命周期必须受所有权系统约束。

需要进一步明确：

- Move。
- Copy。
- Borrow。
- Shared Borrow。
- Mutable Borrow。
- Drop。
- Escape。
- Runtime Registry 是否持有强引用。
- Plugin Unload 时如何证明对象不再被使用。

---

# 19. 声明系统

Declaration 是 Luna 中可被引用、选择、反射和调用的语言实体。

可能包括：

- Function。
- Struct。
- Trait。
- Context。
- Fragment。
- Slot。
- Metadata Schema。
- Module Export。
- Package Export。

Declaration 系统应统一处理：

- 声明身份。
- 声明族。
- 版本。
- Channel。
- Overload。
- Selector。
- Reflection。
- Runtime Descriptor。

---

# 20. 声明身份与声明族

需要区分：

## Declaration Identity

唯一标识一个具体声明。

可能由以下信息构成：

- 所属 Module / Package。
- 声明种类。
- 结构签名。
- Metadata Discriminator。
- 显式版本。
- 编译器内部稳定 ID。

## Declaration Family

表示多个相关声明属于同一个可选择集合。

例如：

```luna
fn greet @stable(1.0.0)() -> i32
fn greet @stable(1.2.0)() -> i32
fn greet @dev(2.0.0)() -> i32
```

这些声明可以属于同一 family，但具有不同 identity。

Name 可以帮助发现 family，却不应独立决定类型身份。

---

# 21. Selector 与候选选择

Selector 是从候选声明集合中选择一个或多个声明的策略。

流程：

```text
Candidate Discovery
    ↓
Structure / Signature Check
    ↓
Metadata Filter
    ↓
Selector Policy
    ↓
Binding
```

Selector 可以存在于：

- 编译期。
- Runtime。
- Dynamic。

但三者成本不同。

编译器只需定义 Selector Protocol 与执行边界，不应硬编码所有选择策略。

---

# 22. 版本、Channel 与 latest

Version、SemVer、Channel、latest 建议主要由标准库提供。

例如：

```luna
@stable(1.2.0)
@dev(2.0.0)
```

`@tag()` 可以选择某个 Channel 中最大声明版本；显式 `x.y.z` 可以固定历史版本。

需要明确：

- Version 是否属于 Metadata。
- Channel 是否属于 Metadata。
- latest 是 Selector 还是语法糖。
- 兼容范围如何表达。
- 多版本共存如何影响 ABI。
- Runtime Plugin 更新后是否自动重新绑定。

当前原则：

> Plugin 更新后，Luna 不自动长期重新绑定；用户需要显式重新 Select，或在明确生命周期事件中触发 Select。

---

# 23. Metadata 系统

Metadata 是附着于语言实体的数据。

建议统一使用 **Metadata** 作为正式术语，逐步淘汰可能混淆的 Label / Annotation / Attribute 多套叫法。

Metadata 能用于：

- 版本。
- Channel。
- 权限。
- 导出。
- Runtime 保留。
- Registry。
- Plugin Discovery。
- Selector。
- ABI。
- 文档。
- 编译器提示。
- 用户自定义扩展。

Metadata 需要具备：

- Schema。
- 类型检查。
- 静态反射。
- 继承或传播规则。
- 冲突规则。
- Retention。
- Migration。
- Trust Level。

---

# 24. Runtime Metadata 与注册机制

Metadata 分层：

```text
meta
    ↓
runtime meta
    ↓
registered runtime meta
```

## meta

只存在于编译期。

编译结束后可删除。

## runtime meta

保留 Runtime Representation，并存入 Runtime Descriptor。

例如：

```luna
runtime meta version {
    value: Version;
}
```

## registered runtime meta

除保留 Runtime Representation 外，还自动进入对应 Registry / Pool。

例如：

```luna
registered runtime meta service {
    key: string;
}
```

应避免让所有 Runtime Metadata 都自动注册，否则会增加：

- 启动成本。
- 内存成本。
- Registry 维护成本。
- Module unload 成本。

---

# 25. 静态反射

静态反射是 Luna 的默认强能力。

应允许编译期读取：

- 结构化类型。
- 名义信息。
- 字段。
- 方法。
- Constraint。
- Trait。
- Metadata。
- Declaration。
- Generic 参数。
- 所有权属性。
- ABI 属性。

静态反射可用于：

- 结构比较。
- 自动生成。
- 序列化。
- FFI。
- Selector。
- 编译期验证。
- 标准库扩展。

静态反射不应自动导致 Runtime 成本。

---

# 26. Runtime Reflection

Runtime Reflection 分为不同级别。

## 最小 Runtime Reflection

由 Runtime Descriptor 提供：

- Entry。
- Signature。
- Runtime Metadata。
- Owner。
- Lifecycle。
- 必要的类型摘要。

## Full Runtime Reflection

属于 Dynamic：

- Declaration 结构。
- Generic 结构。
- Fragment / Slot 关系。
- Replace 所需信息。
- Runtime Weaving 所需信息。
- 更完整的调用和类型信息。

Runtime Reflection 不应只有“有/无”两个状态，而可以进一步设计保留等级。

---

# 27. Runtime Object Model

对象层级：

```text
普通对象
    ↓
Runtime 对象
    ↓
Dynamic 对象
```

## 普通对象

- 仅需要正常执行。
- 不持有 Descriptor。
- 不进入 Registry。
- 无 Runtime Reflection。

## Runtime 对象

- 具有最小 Runtime Descriptor。
- 可以被发现、选择或动态装载。
- 可以携带 Runtime Metadata。
- 不具有完整 Runtime Reflection。

## Dynamic 对象

- 是 Runtime 对象。
- 额外具有完整 Runtime Reflection。
- 支持 Inspect、Replace、Runtime Weaving 等能力。

---

# 28. Runtime Descriptor

Runtime Descriptor 是 Runtime 对象的最小描述。

建议至少包含：

- Stable Runtime ID。
- Declaration Kind。
- Entry Pointer。
- Signature。
- Runtime Metadata Table。
- Module / Package Owner。
- Lifecycle State。
- Capability Flags。
- Optional Type Summary。
- Optional ABI Version。

Descriptor 不应默认包含：

- 完整 AST。
- 源码。
- 完整 HIR。
- 所有泛型求解过程。
- 完整 Declaration Reflection。

---

# 29. Runtime Registry 与 Pool

Registry 用于发现 Runtime 对象。

Pool 可以按某种 Metadata Schema 或能力组织对象。

例如：

```text
pool<service>
pool<serializer>
pool<plugin>
```

需要明确：

- 注册时机。
- 注销时机。
- Module Load / Unload。
- Registry 所有权。
- 强引用还是弱引用。
- 并发安全。
- 选择结果缓存。
- 失效策略。
- Plugin 更新后的状态。

只有 registered runtime metadata 应默认触发自动注册。

---

# 30. Runtime Select

Runtime Select 真正依赖：

- Runtime Descriptor。
- Entry Pointer。
- Signature。
- Runtime Metadata。
- Registry / Pool。

不依赖：

- AST。
- 完整 Declaration Reflection。
- Replace。
- Runtime Weaving。

Runtime Select 发生在明确的生命周期点：

- 初始化。
- Plugin Load。
- Plugin Unload。
- 用户显式重新 Select。

绑定完成后，正常调用应尽量退化为：

```text
Pointer Call
```

Luna 不应默认持续监控并自动更换 Binding。

---

# 31. Dynamic Reflection

Dynamic Reflection 提供对运行时声明的完整观察能力。

可能包括：

- 声明结构。
- 泛型参数。
- Trait / Constraint。
- Metadata。
- Fragment / Slot。
- 版本与身份。
- 调用关系。
- 可替换点。
- 可织入点。

Dynamic Reflection 必须显式启用，并具有清晰成本。

---

# 32. Replace、Inspect 与 Runtime Weaving

## Inspect

读取 Dynamic Declaration 的完整信息。

## Replace

用兼容声明替换现有绑定或实现。

必须验证：

- 结构兼容。
- Signature。
- Ownership。
- ABI。
- Metadata 要求。
- 生命周期。
- 安全权限。

## Runtime Weaving

在 Runtime 修改 Fragment、Slot 或调用链关系。

必须定义：

- 原子性。
- 并发行为。
- 回滚。
- 失败恢复。
- 依赖失效。
- Debug 可观测性。

---

# 33. Fragment、Slot 与 Apply

Luna 的插件织入系统可以围绕以下概念构建：

- Slot：可扩展位置。
- Fragment：可插入实现。
- Apply：将 Fragment 应用于 Slot 或声明。
- Dynamic Apply：运行时执行 Apply。
- Disable / Enable。
- Remove。
- Replace。
- Inspect。

静态 Apply 应尽量在编译期完成，避免 Runtime 成本。

Dynamic Apply 需要 Dynamic 层支持。

需要继续明确：

- Apply 是否是一等公民。
- 多 Fragment 的执行顺序。
- CPS 多发射语义。
- Fragment 与 Context 的关系。
- 插件调用插件时的权限与依赖。

---

# 34. Package 与 Module

Module 是语言级组织与可见性边界。

Package 是构建、分发与依赖边界。

需要定义：

- Import。
- Export。
- Visibility。
- Re-export。
- Module Identity。
- Package Identity。
- Source Package。
- Binary Package。
- Dynamic Package。
- Plugin Package。
- Package Metadata。
- Package Version。
- Package Capability。

Module 内可以进行更强推导；Module / Package 边界应提供稳定签名。

---

# 35. Plugin 系统

Plugin 是通过明确协议装载、发现和应用的 Package 或 Module。

插件系统需要包含：

- Manifest / Metadata。
- Runtime Descriptor。
- Registry。
- Dependency。
- Capability。
- Permission。
- Load。
- Unload。
- Update。
- Re-select。
- State Migration。
- ABI Check。

Dynamic Package 是否必要，需要根据实际能力判断。

如果 Runtime Descriptor + Runtime Metadata 已足够完成装载和选择，则不应强制整个插件成为 Dynamic。

---

# 36. ABI 与二进制兼容

ABI 文档需要定义：

- Calling Convention。
- Name Mangling。
- Runtime Descriptor Layout。
- Runtime Metadata Layout。
- Type Layout。
- Ownership ABI。
- Exception / Error ABI。
- Generic ABI。
- Package Export Table。
- Plugin Entry。
- Versioning。
- Compatibility Check。

必须区分：

- 源码兼容。
- 类型兼容。
- ABI 兼容。
- Runtime Replace 兼容。

结构化类型兼容并不自动意味着二进制布局兼容。

---

# 37. 安全、权限与信任边界

安全模型需要覆盖：

- Compile-time Plugin Trust。
- Runtime Plugin Trust。
- Metadata Trust。
- Host-provided Metadata。
- Compiler-provided Metadata。
- Package Signature。
- Capability。
- Permission。
- Sandbox。
- Reflection Permission。
- Replace Permission。
- Weaving Permission。
- Screenshot / Window / System Resource 等高权限接口。

安全检查应尽量通过结构化 Capability 与 Metadata 表达，而不是依赖字符串约定。

---

# 38. 编译器实现管线

建议的实现管线：

```text
Source
  ↓
Lexer
  ↓
Parser
  ↓
AST
  ↓
Name Resolution
  ↓
Declaration Collection
  ↓
HIR
  ↓
Type / Constraint / Trait Solving
  ↓
Compile-Time Evaluation
  ↓
Ownership Check
  ↓
MIR
  ↓
Optimization
  ↓
LLVM IR
  ↓
AOT / Object / Binary
```

可选支线：

```text
HIR / MIR
  ↓
Descriptor Generation
  ↓
Runtime Metadata Emission
  ↓
Dynamic Reflection Emission
```

应确保普通静态代码不会因为 Runtime / Dynamic 支线而承担额外成本。

---

# 39. Runtime 实现管线

建议的 Runtime 流程：

```text
Module Load
  ↓
Descriptor Registration
  ↓
Registered Metadata Registration
  ↓
Registry / Pool Update
  ↓
Runtime Select
  ↓
Binding
  ↓
Pointer Call
```

Plugin Unload：

```text
Unload Request
  ↓
Ownership / Usage Check
  ↓
Binding Invalidation
  ↓
Registry Removal
  ↓
Descriptor Removal
  ↓
Module Unload
```

Dynamic Replace：

```text
Inspect
  ↓
Compatibility Check
  ↓
Prepare
  ↓
Atomic Replace
  ↓
Invalidate Cache
  ↓
Optional Re-select
```

---

# 40. 标准库职责

标准库负责可替换策略，而不是语言正确性。

建议标准库承担：

- Version。
- SemVer。
- Channel。
- latest Selector。
- Compatibility Selector。
- Runtime Collection。
- Registry Helper。
- Reflection Helper。
- Serialization。
- IO。
- Async。
- Concurrency。
- Package Metadata Schema。

编译器只需要支持它们依赖的底层语言协议。

---

# 41. 工具链与工程组织

工具链可能包括：

- `luna` 编译器。
- Package Manager。
- Formatter。
- Linter。
- Language Server。
- Documentation Generator。
- ABI Inspector。
- Metadata Inspector。
- Runtime Registry Inspector。
- Plugin Debugger。
- Dynamic Weaving Debugger。

工具链应共享统一的 Declaration、Metadata 和 Type 模型。

---

# 42. RFC 流程

新特性建议遵循：

```text
Idea
  ↓
Problem Statement
  ↓
RFC
  ↓
Architecture Review
  ↓
Specification
  ↓
Implementation Plan
  ↓
Implementation
  ↓
Validation
```

RFC 至少包含：

- 动机。
- 非目标。
- 术语。
- 所属层级。
- 静态语义。
- Runtime 语义。
- 成本。
- ABI 影响。
- 安全影响。
- 与现有机制关系。
- 替代方案。
- 未决问题。
- 迁移方案。

---

# 43. 规范与实现分离

Specification 描述 **What**：

- 语法。
- 语义。
- 错误。
- 可观察行为。
- 兼容规则。

Implementation 描述 **How**：

- AST / HIR / MIR。
- Solver。
- LLVM。
- Descriptor Layout。
- Registry 数据结构。
- 缓存。
- 优化。

例如，规范可以要求 Runtime Select 在明确生命周期点发生，但不规定 Registry 必须使用哈希表还是树。

---

# 44. 术语冻结

建议建立 `glossary.md` 并冻结核心术语。

推荐官方术语：

- Metadata。
- Runtime Metadata。
- Registered Runtime Metadata。
- Declaration。
- Declaration Identity。
- Declaration Family。
- Candidate Discovery。
- Selector。
- Runtime Object。
- Runtime Descriptor。
- Runtime Registry。
- Runtime Pool。
- Runtime Select。
- Dynamic Reflection。
- Replace。
- Inspect。
- Runtime Weaving。
- Fragment。
- Slot。
- Apply。

避免多套同义词长期混用，例如：

- Label / Annotation / Attribute / Metadata。
- Dynamic Descriptor / Runtime Descriptor。
- Dynamic Metadata / Runtime Metadata。
- Runtime Declaration / Dynamic Declaration 未区分使用场景。

---

# 45. 当前已确定原则

1. Luna 默认采用结构化类型。
2. 名字可以参与候选发现，但不默认参与类型身份。
3. 候选发现后仍需结构、签名与 Constraint 检查。
4. Metadata 默认只存在于编译期。
5. `runtime meta` 才具有 Runtime Representation。
6. `registered runtime meta` 才默认进入 Registry。
7. Runtime 与 Dynamic 是不同层级。
8. Runtime Select 依赖 Runtime Descriptor，而非完整 Dynamic Reflection。
9. Dynamic = Runtime + Full Runtime Reflection。
10. Runtime Select 只在明确生命周期点或用户显式请求时发生。
11. 绑定后正常调用应尽量退化为 Pointer Call。
12. Plugin 更新不会默认导致永久自动重新绑定。
13. Compiler 负责语言正确性。
14. Core 负责稳定抽象。
15. Standard Library 负责可替换策略。
16. Runtime 负责最小运行时表示。
17. Dynamic 负责完整运行时反射与修改。
18. 能在低层实现的能力不应被无理由提升到高层。
19. Specification 与 Implementation 必须分离。
20. 新特性应先明确所属层级和成本。

---

# 46. 尚待讨论的问题

以下问题尚未完全冻结：

- Metadata 的最终语法。
- `runtime meta` 与 `registered runtime meta` 的精确声明形式。
- Runtime Descriptor 的二进制布局。
- Stable Runtime ID 的生成规则。
- Declaration Identity 的正式组成。
- Version 与 Channel 是否全部属于标准库 Metadata。
- Selector Protocol 的精确类型。
- Runtime Select 的错误模型。
- Registry 的所有权模型。
- Plugin Unload 的借用与占用检查。
- Dynamic Package 是否需要作为独立概念。
- Apply 是否是一等公民。
- Fragment / Slot 的 CPS 多发射语义。
- Replace 的原子性和回滚模型。
- Runtime Weaving 的线程安全。
- Dynamic Reflection 的分级保留模型。
- JIT 与 AOT 的正式关系。
- Runtime Object 是否可以在运行时升级为 Dynamic Object。
- Dynamic Object 是否可以降级。
- Metadata Migration 的版本策略。
- 二进制插件跨编译器版本兼容策略。
- Capability 与 Permission 的统一模型。

---

# 47. 推荐文档结构

```text
docs/
├── README.md
├── glossary.md
│
├── philosophy/
│   ├── goals.md
│   ├── design_principles.md
│   └── cost_model.md
│
├── architecture/
│   ├── language_architecture.md
│   ├── compiler_architecture.md
│   └── runtime_architecture.md
│
├── specification/
│   ├── lexical_structure.md
│   ├── declarations.md
│   ├── expressions.md
│   ├── type_system.md
│   ├── constraints.md
│   ├── traits.md
│   ├── ownership.md
│   ├── generics.md
│   ├── metadata.md
│   ├── reflection.md
│   ├── selector.md
│   ├── runtime.md
│   ├── dynamic.md
│   ├── package.md
│   ├── plugin.md
│   ├── abi.md
│   └── security.md
│
├── implementation/
│   ├── frontend.md
│   ├── hir.md
│   ├── type_checker.md
│   ├── trait_solver.md
│   ├── ownership_checker.md
│   ├── mir.md
│   ├── codegen.md
│   ├── runtime_descriptor.md
│   ├── registry.md
│   └── dynamic_runtime.md
│
└── rfcs/
    ├── README.md
    ├── RFC-0001-metadata.md
    ├── RFC-0002-runtime-object-model.md
    ├── RFC-0003-selector.md
    └── ...
```

---

# 48. 架构摘要

Luna 的整体架构可以压缩为：

```text
Compiler
    │
    ├── 证明程序正确
    ▼
Core
    │
    ├── 提供稳定语言抽象
    ▼
Standard Library
    │
    ├── 提供可替换策略
    ▼
Runtime
    │
    ├── 提供最小运行时表示
    ▼
Dynamic
    │
    └── 提供完整运行时反射与修改
```

对象能力层级：

```text
普通对象
    ↓
Runtime 对象
    ↓
Dynamic 对象
```

Metadata 能力层级：

```text
meta
    ↓
runtime meta
    ↓
registered runtime meta
```

执行成本层级：

```text
Compile Time
    ↓
Runtime
    ↓
Dynamic
```

最终原则：

> 编译器只负责语言正确性；Core 只负责稳定抽象；标准库负责策略；Runtime 负责最小运行时表示；Dynamic 负责完整运行时反射。任何能力都应存在于足以实现它的最低层级，用户只为实际使用的能力支付成本。
