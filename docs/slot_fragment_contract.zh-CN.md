# Slot/Fragment 有界契约证据

[English](slot_fragment_contract.md) | 简体中文

> 状态：已实现的有界行为及回归证据，2026-09-26。
> 本文记录 SFR001/v1 的实现，不是稳定版发布授权。

## SF008 有界规则（2026-09-26）

以下规则区分静态构造、运行时选择和续体执行，不增加语法、不改变 v1 ABI，
也不授权任意 handler body 重入。

| 边界 | 已实现规则 | 回归门禁 |
| --- | --- | --- |
| 静态嵌套 `apply` | 最内层词法绑定生效，退出后恢复外层绑定；不会隐式构造 runtime chain。 | `luna.semantic-regressions` |
| 静态正文展开 | 直接和相互递归展开均由 Sema 与 CFG builder fail closed，伪造的 structured MoonIR 也被拒绝。 | `luna.semantic-regressions`、`luna.moonir-canonical` |
| 静态同 Slot 续体嵌套 | 有限嵌套合法。内层 discard 只跳过自己的续体；内层续体 return 则退出发起调用的函数，并跳过内外 handler 的 post-resume 代码。 | `luna.semantic-regressions` |
| 未绑定 private Slot | Identity 降为普通词法块，在 handler 内也如此；不会引入 runtime context 依赖。 | `luna.semantic-regressions`、`luna.moonir-canonical` |
| Runtime ordered chain | `resume` 按宿主明确给出的顺序前进，post-resume 按逆序返回；catalog 顺序和加载顺序不决定 chain。 | `luna.runtime-fragment-v1` |
| Runtime 同 Slot 续体嵌套 | 即使使用同一 pinned context 与已选 chain，每次 dispatch 仍创建独立 single-shot activation；被继续传播的内层续体逃逸会跳过暂停的外层 chain 的 post-resume 代码。 | `luna.runtime-fragment-v1` |
| 嵌套局部 override／None | 明确传入的内层 context 只在自身替换或移除目标 Slot 的 chain，不修改暂停的外层 chain；None 仍传播续体逃逸。 | `luna.runtime-fragment-v1` |
| Runtime 参数与环境地址 | Slot 参数的实际地址必须满足声明的对齐；空载体固定为 size 0、alignment 1、null data。None 与 One 都在执行回调前拒绝非法载体，借用及工厂返回的环境也必须实际对齐。被拒绝的非空工厂产物销毁一次。 | `luna.runtime-fragment-v1` |
| 发布 handler 的所有权 | Exported body 即使没有本地 `apply` 也独立检查所有权：局部 linear 状态必须消耗，冲突借用与重复 free 被拒绝。隐式 affine 清理在重复静态应用、续体逃逸和仅宿主选择的 runtime dispatch 中仍有效。 | `luna.analysis-snapshot`、`luna.semantic-regressions`、`luna.moonir-canonical` |
| 发布 handler 的 capability | Exported body 即使没有本地使用也必须分析；生成的 Fragment runtime entry 若直接或传递地需要 execution context，由独立重算的 effect 拒绝，伪造 summary 不能绕过。 | `luna.semantic-regressions`、`luna.moonir-canonical` |
| 静态／runtime 成本边界 | 静态绑定组合不引入 runtime dispatch 或候选发现；未绑定 exported Slot 使用显式 context-directed dispatch。 | `luna.moon-cost-boundaries`、`luna.moonir-canonical` |

逃逸相对于拥有续体的函数或 callback 的明确控制结果而言。普通被调用函数的正常
return 不会自动逃逸调用者的续体或外层 chain。

静态源码证据包括[有限嵌套](../tests/fixtures/fragment_nested_continuation.luna)、
[嵌套 discard 与 return](../tests/fixtures/fragment_nested_discard.luna)、
[静态 handler 发布](../tests/fixtures/exported_fragment_static_body.luna)、
[发布 handler 清理](../tests/fixtures/exported_fragment_ownership.luna)，以及
[私有组合继承 context](../tests/fixtures/fragment_static_dynamic_body.luna)。
独立 CFG／effect 检查在
[canonical lowering 测试](../tests/moonir_canonical_sealing_lowering_test.cpp)；
显式 context／chain／override 检查在
[runtime Fragment 测试](../tests/runtime_fragment_test.cpp)。

## 剩余决定与发布边界

`TBD-SF008` 现在有明确的有界实现规则，静态递归不再是未定义的隐式行为。
稳定语言承诺的接受，以及允许 published handler 获取 execution context 并从自身
正文进行动态 dispatch 的扩展，仍保持开放。当前 v1 execute wrapper 接收环境与
opaque activation，不接收这项 capability。未来扩展必须先明确 context 传递、
重入语义及 ABI 兼容，再实施；仅删除拒绝检查是不安全的。

宿主编写的 native callback 不属于本文验证的 Luna 源码。它能调用 C dispatch ABI，
不代表任意 Luna handler body 递归已有稳定保证。本文不引入隐式 TLS／current Runtime、
自动抑制同 Slot、runtime 递归深度限制或 multi-shot continuation 策略。

地址对齐检查不能证明任意 native pointer 确实指向声明大小、有效生命周期的分配；
这些义务仍由宿主与工厂承担。

带 context 的间接调用及非 Copy 的 exported 契约仍不在有界首版 ABI 内。本文不扩大
2026-09-15 的核心冻结，也不关闭独立的性能、稳定性及发布授权门禁。
另见[运行时计划](slot_fragment_runtime_plan.zh-CN.md)和
[发布登记](ecosystem_release.zh-CN.md)。

## 复现有界门禁

先构建编译器和测试目标，然后运行：

```sh
ctest --test-dir build --output-on-failure -R 'luna\.(analysis-snapshot|semantic-regressions|runtime-fragment-v1|moonir-canonical|moon-cost-boundaries|0\.3-design-contract)$'
```

六项门禁都是行为／结构检查，不使用微基准时间阈值。稳定版发布声明仍需要完整
回归，以及针对确切拟发布 commit 的跨平台证据。
