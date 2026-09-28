# Slot/Fragment 有界契约证据

[English](slot_fragment_contract.md) | 简体中文

> 状态：已实现的有界行为及回归证据；v1 验收快照更新于 2026-09-28。
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
| 并发宿主发布／分派 | 四个 native reader 在仍存活的续体内暂停并跨越显式 None／One／chain 发布；旧分派在正常完成、逃逸和失败时都保留原选择。Runtime 销毁后，reader 各自持有的同一 context 状态的副本仍可分派，activation 独立，最终按序清理。Linux TSan 门禁同时插桩实现与测试。 | `luna.runtime-fragment-concurrency` |
| 执行中快照的生命周期 | 分派持有已选快照，直到所有 handler 返回。同步 native 回调可清空或替换公开 C++ 句柄，不会释放当前 One／chain 的环境或 generation；替换只影响后续调用。Owned／borrowed 环境先于最后的 module lease 释放，逃逸与失败路径也如此。 | `luna.runtime-fragment-v1` |
| Factory generation 生命周期 | Owned 构造在调用 factory 前固定已验证的 generation。同步清空或替换输入 binding 不会在构造或拒绝产物清理期间释放原 generation，也不能让新引用改用另一 binding。成功将原 pin 交给引用；失败先销毁非空产物一次，再释放原 pin。 | `luna.runtime-fragment-v1` |
| 引用清理回调 | `reset` 在 owned destroy／borrowed lease 释放前摘走旧状态，保留其 generation 直到这些操作完成。嵌套 reset 看到空引用；同步重新绑定仍存活的引用会保留。移动赋值先安装 incoming 状态，再清理旧状态，不覆盖回调修改。Owned 环境仅销毁一次；引用持有的 borrowed lease 先于其 module pin 释放。 | `luna.runtime-fragment-v1` |
| Single-shot 失败传播 | 有效 activation 的 resume 失败会保持失败；忽略重复 resume 的错误不能使它所属的 One／chain 分派伪装为成功，续体仍最多执行一次。下游失败向外传播且保留其诊断，同一 context 的新调用不受旧失败影响。 | `luna.runtime-fragment-v1` |
| Runtime 同 Slot 续体嵌套 | 即使使用同一 pinned context 与已选 chain，每次 dispatch 仍创建独立 single-shot activation；被继续传播的内层续体逃逸会跳过暂停的外层 chain 的 post-resume 代码。 | `luna.runtime-fragment-v1` |
| Verified container 的本包 Slot publication | 解码保留 canonical Slot／contract 与 export 行，而非前端 Slot 对象。本包确切 Slot export 可通过 verified generation adapter 分派；缺失、错误 kind／contract 的 export，以及外包 import／re-export 冒充均被拒绝。生成的 Copy 环境 factory、None／One、resume／discard、capture 回写与续体 return 逃逸在 Runtime 销毁后仍可执行。 | `luna.moonir-canonical` |
| 嵌套局部 override／None | 明确传入的内层 context 只在自身替换或移除目标 Slot 的 chain，不修改暂停的外层 chain；None 仍传播续体逃逸。 | `luna.runtime-fragment-v1` |
| Runtime 参数与环境地址 | Slot 参数的实际地址必须满足声明的对齐；空载体固定为 size 0、alignment 1、null data。None 与 One 都在执行回调前拒绝非法载体，借用及工厂返回的环境也必须实际对齐。被拒绝的非空工厂产物销毁一次。 | `luna.runtime-fragment-v1` |
| Runtime 身份表示 | C++ module、symbol、contract、Slot 与参数 layout ID 不允许内嵌 NUL。Generation staging 在初始化前拒绝它；发现、activation、None／One 分派与局部 override 拒绝歧义键，不会静默截断成另一个 C ABI 身份。 | `luna.moon-runtime`、`luna.runtime-fragment-v1` |
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
[并发宿主测试](../tests/runtime_fragment_concurrency_test.cpp)使用不可变 native 环境和
逐调用独立参数。回调／环境的同步仍由宿主负责；这不是任意 Fragment 环境或 Arc
payload 的源码线程安全准入。

## v1 验收快照（2026-09-28）

本次对照源码、宿主头文件、verified container 测试与实施记录。核心机制已完成仅指
**原生宿主 API 驱动的有界 v1**，不是全部 SFR001 源码计划或稳定版验收。C++
`RuntimeFragmentRef` 不是按 `S` 参数化的模板；它在构造和安装时检查精确 Slot／
Contract，并不意味着 Luna 已有可传递的 `RuntimeFragmentRef<S>` 类型。源码类型／
所有权验证与宿主边界的运行时名义检查不能互相冒充。

下列键与状态只是审计标签，不是新关键字、公共 API、决策 ID 或发布批准。

<!-- SLOT_FRAGMENT_V1_ACCEPTANCE_BEGIN -->

| 边界 | 状态 | 已有证据／实际剩余项 |
| --- | --- | --- |
| `host-ref` | `implemented` | C++ move-only 引用、owned／borrowed 环境、factory／cleanup、generation pin 及已校验的 native 拥有 carrier 转移；`luna.runtime-fragment-v1`。 |
| `source-ref-apply` | `implementation-open` | Native singleton handle／校验／Drop／transfer 桥、源码 `RuntimeFragmentRef<S>` 类型拼写／名义约束检查、内部 LLVM Ref Drop／直接局部转移及冻结 Slot／Contract 目标解析预备已实现。生成源码签名／import／参数／返回接线、完整 compiler dropGlue、wire round-trip、运行时 operand 与端到端门仍未完成，内部 Ref 暂禁止发布；见实施切片。 |
| `candidate-snapshot` | `implemented` | `snapshotRuntimeFragmentCandidates(generation, slot, ...)` 按精确 Slot／Contract 过滤显式给定的单个 generation，快照不可变且固定 generation；不是所有已加载包的全局查询。 |
| `candidate-aggregation` | `host-managed` | 宿主知道自己加载的包并可组合各 generation 的候选；Runtime 没有内建全局候选集合或跨 generation 聚合查询。便利 API 是后续范围选择，不是当前热路径缺陷。 |
| `candidate-notification` | `host-managed` | 加载／激活结果和 generation identity 供宿主观察；没有内建候选变化事件总线，不自动发现、排序、选胜者或注入。 |
| `binding-dispatch` | `implemented` | 显式 None／One／ordered chain、safe point、pinned context、局部 override；执行中释放／替换句柄仍保留原快照与环境。 |
| `context-entry` | `implemented` | `runtime fn` 显式宿主入口、direct-call effect 最小不动点、参数与 continuation frame、跨包 verified container、return／`?` 逃逸；`luna.moonir-canonical`。 |
| `handler-context-reentry` | `deferred` | v1 published execute wrapper 不传 context；handler 正文直接／传递动态 dispatch 被拒绝。Native base continuation 嵌套不授予此能力；`TBD-SF008` 稳定承诺仍开放。 |
| `context-indirect-call` | `deferred` | 需要 context 的 function value／间接调用仍 fail closed，不靠 TLS 或删除拒绝检查扩展 ABI。 |
| `noncopy-public-abi` | `deferred` | Exported Slot 参数与 Fragment 环境为 Copy-only；静态 affine 环境不等于跨宿主 move/drop 协议。 |
| `multi-shot-nonunit` | `deferred` | 首版仍是 unit-result、single-shot，不增加可逃逸续体、异步 activation 或多次 resume。 |
| `runtime-cost-structure` | `implemented` | 静态擦除、动态 site 单次 dispatch、控制平面不进入热路径；作用域 activation 与固定身份复用有分配／寿命回归，不是整次派发零分配。 |
| `performance-acceptance` | `acceptance-open` | 已有固定线程匹配观察，不是受控交替 A/B、跨平台性能预算或正式批准。CI 微型／短系列证明行为和协议，不能代替性能验收。 |
| `durable-evidence` | `storage-open` | 本机证据包和 14 天 CI artifacts 不等于持久外部存储；需确定目的地、保留期与身份锚点，不能仅延长 CI 保留就声称永久归档。 |
| `stable-release` | `authorization-open` | 有界行为完成与稳定语言／发布授权分离；不关闭历史 TBD 登记、不扩大 0.3 核心冻结、不改 tag／lock／发布门禁。 |

<!-- SLOT_FRAGMENT_V1_ACCEPTANCE_END -->

源码定位：[宿主 API](../src/runtime/RuntimeFragment.h)、
[单 generation 查询](../src/runtime/RuntimeFragment.cpp)、
[静态 apply parser](../src/parser/ParserStatements.cpp)及
[跨包真实容器加载](../tests/moonir_canonical_runtime_slot_container_test.cpp)。
固定身份复用的测量锚点为 `bc9d6fd`，当时 77／77 非 hardware 回归和三平台 CI
通过；性能观察构建也是该提交。报告提交 `7757b77` 的跨平台结果另行跟踪，不把报告或
本次审计提交当作原始测量构建。完整数值及摘要在
[实施计划](slot_fragment_runtime_plan.zh-CN.md#固定身份复用的匹配协议复测2026-09-28)。

审计阶段用既有严格警告构建运行有界八项门禁及文档 inventory，9／9 通过（最终 14.68 秒），
不声称重新构建了全部编译器或重跑了 77 项。新增状态门禁先因缺少验收快照失败，补齐后
通过；内存负夹具还拒绝提前宣称完成、漏项、重复项和未登记项。
只读 `verify_release_readiness.cmake` 当前明确阻止发布：lock 中的已验证 Luna candidate
`41ce85ec9d6d2c2f22b193b3ece60abb09d4c5c3` 不是当前 HEAD 的祖先。脚本退出 0 表示
fail-closed 策略正常，不表示 release-ready；这是独立生态证据／提升问题，不是 Slot
执行缺陷。不能自动换掉 candidate、改 lock 或移动历史 tag 来绕过该门禁。
本地仓库不是 shallow clone，候选 commit 对象存在，直接 `git merge-base --is-ancestor`
返回 1；这不是仅因浅历史或缺失对象产生的检查失败。

2026-09-28 已选择源码级 Ref／runtime apply 为下一开发方向，优先补齐功能而非第三项微优化：

1. 若验收目标是当前 native-host v1，先明确实用性能预算、稳定承诺和证据存储政策；
   不必为此增加全局 catalog、候选通知或 handler 重入。
2. `source-ref-apply` 是实质未完成项。见[源码实施切片](slot_fragment_runtime_plan.zh-CN.md#源码级-refapply-的实施切片2026-09-28)：先用冻结 singleton 与显式 parent 打通宿主输入、名义类型和局部 apply；源码 `.bind`、加载策略和 handler 重入不混入首批。

`tests/luna_0_3_design_contract.cmake` 保护两种语言的完整分类，防止把源码缺口、宿主
可选设施、延期能力和批准混作“实现完成”。它是状态一致性门禁，不是这些能力的源码
行为证明或负责人批准。

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
C ABI 入口处的 context 必须仍有效。执行中的保留不允许再次使用已释放的 opaque
pointer，也不允许在没有同步的情况下并发修改同一个 C++ 句柄。
清理回调不得抛异常、销毁仍在使用的对象，或复活正在析构的对象。仍存活引用的清理
重入是 native 宿主生命周期规则，不是任意 Luna handler body 重入的授权。
失败传播不回滚已经发生的副作用，也不隔离 native handler；它保证 handler 返回后，
失败的 activation 不会被报告为成功分派。

带 context 的间接调用及非 Copy 的 exported 契约仍不在有界首版 ABI 内。本文不扩大
2026-09-15 的核心冻结，也不关闭独立的性能、稳定性及发布授权门禁。
跨包 verified container 以所属包自身的根 `Exports` 持久化 Slot 公开事实。
完整解码该所属包产物后，编译器签发不可变 `SlotPublicationEvidence`；宿主在解码或
stage 消费者时显式传入。owner、直接依赖、target/layout、精确 SlotId/ContractId、
结构类型与参数布局必须吻合。缺少证据仍 fail closed，包括 load-once 缓存命中。
消费者不重新导出依赖 Slot；没有新增关键字、容器字段、ContractId 编码、Runtime ABI
或派发时查找。产物校验只保证结构与完整性，不认证发布者，也不隔离 native code；
产物信任与 Fragment 选择仍由宿主负责。编译器适配器不扫描磁盘、不递归加载依赖、
不自动选候选或激活绑定。
正式 `build -t moon` 编码在内部依照已验证的源码 projection 完成自校验，不对外
提供可供消费者加载使用的依赖证据。
另见[运行时计划](slot_fragment_runtime_plan.zh-CN.md)和
[发布登记](ecosystem_release.zh-CN.md)。

## 复现有界门禁

先构建编译器和测试目标，然后运行：

```sh
ctest --test-dir build --output-on-failure -R 'luna\.(analysis-snapshot|semantic-regressions|moon-runtime|runtime-fragment-v1|runtime-fragment-concurrency|moonir-canonical|moon-cost-boundaries|0\.3-design-contract)$'
```

八项门禁都是行为／结构检查，不使用微基准时间阈值。稳定版发布声明仍需要完整
回归，以及针对确切拟发布 commit 的跨平台证据。
