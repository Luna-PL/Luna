# Luna 语义参考

> 文档类别：参考入口
> 适用版本：Luna 0.3.0 开发期，同时显式保留冻结的 0.2 基线
> 状态：Active
> 规范性：本页只定义导航；各子文档单独声明规范性
> 首次实现核对：`d0ab31c`（2026-07-31）

本目录保存当前可逐项核对的语言参考，以及冻结的 Luna 0.2 Alpha 迁移基线。它把语言承诺、
当前实现、内部表示和未来计划分开，避免把某个编译器版本的偶然实现写成永久语言
规则。

## 阅读顺序

1. [文档记录规则](documentation_rules.md)：文档类别、状态、规范性和变更规则；
2. [类型系统参考](type_system.md)：当前类型域、身份、关系、推断、所有权和布局模型；
3. [内置类型清单](builtin_types.md)：所有源码内置、编译器内在和内部类型的权威清点；
4. [错误模型契约](error_model.md)：`Result`、`?`、`panic` 和外部错误边界；
5. [0.2 Alpha 语义基线](semantic_baseline_0.2.md)：冻结的迁移证据。

补充理由与实现说明：

- [当前架构](../architecture.md)
- [已采用架构决策](../decisions.md)
- [Runtime ABI](../runtime_abi.md)
- [测试与回归](../testing.md)

架构和决策文档不能覆盖本目录中已经冻结的 0.2 Alpha 契约。发现冲突时，按
[文档记录规则](documentation_rules.md)处理并记录差异，不能静默选择一种行为。

## A0 范围

A0 只做语义清点、契约分层和文档冻结，不借文档工作扩展语言表面。当前范围包括：

- 类型域、类型身份、结构/名义关系；
- 源码可见内置类型和编译器内在类型；
- ownership relation、usage cardinality 与类型的关系；
- `Result`、`?`、`panic` 和 Runtime/FFI 错误边界；
- 语言语义、MoonIR 契约和物理布局的分层；
- 已实现、实验性、内部和计划能力的状态标注。

selector、fragment、GPU、package 和标准库仍由各专题文档描述。只有它们与类型或
错误模型相交的部分进入本次基线。
