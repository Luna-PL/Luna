# Luna 0.2 Alpha 架构决策

> 文档类别：已采用设计决策
> 适用版本：Luna 0.2.1
> 状态：冻结的 0.2 理由基线；与 0.3 总体设计冲突处已被取代
> 规范性：非规范理由记录；当前行为以 [Alpha 语义参考](reference/README.md) 为准
> 实现核对：2026-07-31

本文件压缩已采用 RFC 的“为什么”。它不重复完整语义、ABI 数字或未来路线。
如果决策摘要和参考文档冲突，以参考文档及其回归证据为准。

## D001：MoonIR 是唯一后端输入

**决定**

- 类型正确的程序先降低到 MoonIR，再进入 JIT 或 AOT。
- MoonIR 保存 package/module 身份、类型表、所有权清理义务、metadata 和可达性。
- 进入 LLVM 前必须验证；优化后再次验证。

**理由**

直接从多个前端阶段生成 LLVM IR 会让语义检查、所有权清理和 JIT/AOT 行为漂移。
一个可验证中间层能把语言正确性与目标相关代码生成分开。

**延后**

Moon 容器、签名验证、独立 MoonRuntime 装载和热点 JIT 尚未成为 Alpha 契约。

## D002：Metadata、Selector 与声明身份分离

**决定**

- Metadata 是结构化声明附加数据，不默认改变函数类型。
- 候选发现和候选选择是两个步骤；selector 只在类型正确的候选集合中工作。
- 普通 metadata 不参与声明身份；需要区分声明族成员时必须使用显式的
  discriminator/selector 规则。
- 版本、channel 和 `latest` 是可由 Core/标准库表达的选择策略，不写死进类型系统。

**理由**

把版本或 label 拼进函数名、函数类型或符号身份会混淆发现、兼容与链接。独立协议
允许静态选择无运行时成本，也为显式动态选择保留同一套候选模型。

**延后**

稳定 Runtime Descriptor、registry 生命周期、动态 metadata schema 和插件卸载仍是
计划能力。

## D003：类型域、结构形状与名义身份

> 0.3 更新：关系分离仍然有效，但具名 struct/enum 现在默认名义。见 0.3 总体设计的 C008/TY002。

**决定**

- 区分 Value、Meta、Compiler/Inference/Error 等类型域。
- TypeId 表示语言身份，ShapeId 表示结构形状，ABI compatibility 是第三种关系。
- 具名 struct/enum 默认具有名义声明身份；结构比较是显式 ShapeId/constraint relation；
  `nominal` 在 0.3 中不是声明修饰符。
- 类型、布局和所有权是不同维度。

**理由**

单一“类型相等”无法同时支撑结构泛型、名义安全、反射和 ABI 检查。把关系拆开后，
每个边界可以明确选择需要的证明。

精确规则和实现矩阵见[类型系统参考](reference/type_system.md)与
[内置类型清单](reference/builtin_types.md)。

## D004：Ownership relation 与 usage cardinality 正交

**决定**

- Owned、SharedBorrow、MutableBorrow 描述值与存储的关系。
- Copy、Affine、Linear 描述值的允许消费次数。
- 借用作用于 Place；Place 由根 binding 和字段、索引、解引用投影组成。
- 清理义务在 MoonIR 中显式表示，并在所有可达退出路径上验证。

**理由**

“拥有”不等于“必须恰好使用一次”。正交模型能够表达可丢弃但需要析构的拥有值、
必须消费的 event，以及不拥有资源的借用。

**边界**

多次执行的 context、循环和 GPU in-flight 状态必须证明每条路径具有一致的 usage
与 loan 状态。泛型递归 Drop 已完成；通用堆拥有容器仍等待稳定的 element move-out/
initialization tracking 与 mutable-view 失效规则。

## D005：`Result`、`?` 与 abort 型 `panic`

**决定**

- `Result<T, E>` 是带活动 tag 的 inline ADT，只拥有活动 variant 的载荷。
- `?` 成功时解包，失败时提前返回；错误路径执行与显式 `return` 相同的清理。
- Alpha 的 `panic` 是不可恢复的 abort 边界，不是异常和代数效应。
- `never` 是控制流 bottom type，不是可实例化的普通值。

**理由**

可恢复失败必须在函数类型和控制流中可见；不可恢复失败则需要简单、可预测且不绕过
所有权清理证明的边界。

当前转换限制与状态矩阵见[错误模型契约](reference/error_model.md)。

## D006：标准错误与外部边界显式转换

**决定**

- Core 提供最小错误抽象，标准库定义平台和领域错误。
- `From<Source>` 转换是静态、显式可证明的一跳转换；当前不做隐式链式搜索。
- C FFI、Runtime 和 GPU 的 status/errno/快照必须由 adapter 立即复制到拥有错误。
- `Result` 和标准错误 ADT 不直接穿过 C ABI。

**理由**

外部 borrowed error 字符串、errno 和 backend 状态有不同生命周期。隐式提升会掩盖
数据所有权和失败来源，显式 adapter 才能稳定 ABI。

## D007：Fragment 控制契约显式化

**决定**

- interceptor 正常完成后继续；context 通过 `resume()` 控制续体；`abort()` 丢弃续体。
- once/many 是声明契约，不从源码中出现几次 `resume()` 推断。
- 静态 context 使用栈上 continuation frame；动态外部插件目前只允许 host-only、
  single-shot interceptor。

**理由**

显式控制契约使所有权检查能够验证每条 resume/abort 路径，也避免把栈 continuation
暴露为不受约束的共享库回调。

## D008：Runtime ABI 版本化且按需付费

**决定**

- allocator、console、错误快照、GPU 和插件使用版本化 C ABI。
- JIT 显式向 ORC 注册 Luna runtime 符号，不依赖宿主可执行文件导出策略。
- 安装后的 AOT 必须显式或通过环境选择匹配的 runtime archive 与 native compiler。
- 未使用 runtime/kernel 能力的程序不应生成对应符号。

**理由**

源码语义、编译器内部布局和宿主 ABI 具有不同稳定周期。版本化 C 边界可以单独测试，
同时保持普通程序的运行时成本可解释。

## 修改规则

新决策只有在实现、测试和参考文档同步后才能加入本文件。被推翻的决定直接修改当前
摘要并在 `CHANGELOG.md` 记录迁移；详细讨论过程留在 Git 历史，不重新建立历史文档树。
