# Luna 0.3 宿主进化 API

[English](evolution.md) | 简体中文

Luna 0.3 通过 C++17 源级 API 向嵌入宿主开放最小无状态 generation 闭环。使用时
包含 `<luna/runtime/Evolution.h>` 并链接已安装的 `runtime` 静态库；该表面的
`luna::runtime::EvolutionApiVersion` 为 `1`。这套控制面刻意独立于 C-compatible
Runtime ABI v1，不承诺 C ABI，也不承诺跨 C++ toolchain 的二进制兼容。

0.3 不增加用于 generation evolution 的 Luna 源码关键字，也不提供
`luna activate` 命令。runtime 状态属于嵌入进程；一次性编译器命令无法安全识别该
进程的 safe point、retained generation 与活动 reference。进程管理器可以封装本 API，
但其协议属于应用策略，不是语言或编译器契约。

## 冻结的对象模型

- `MoonRuntime` 持有 module history 及每个 module 原子发布的 active generation。
- `GenerationStagingRequest` 携带稳定 module ID、content digest 与非空共享 module
  lease；该 lease 必须维持代码和 descriptor storage 的寿命。
  新增的可选尾字段 `materializationKey` 把 loader 的生成代码配置与产物摘要分开。
  原三字段 request 保持空 key。key 在 staged／pinned 快照中不可变；load-once 复用要求
  content 与 key 都相同，改变 key 必须显式 `activate`，不会隐式替换。非空 key 在
  callback 前拒绝 CR／LF／tab／内嵌 NUL。该兼容的 C++ 源级扩展不改变 Runtime C ABI
  v1 或任何容器 record。
- `GenerationVerifier`、`GenerationResolver` 和可选
  `GenerationInitializer` 构成可信 staging 边界；staging 严格按此顺序执行，且不发布。
- `StagedGeneration` 是 move-only candidate；`loadOnce` 用于首次发布，`activate`
  用于 evolution transition。
- `PinnedGeneration` 与 `PinnedBinding` 保留它们实际观察到的 generation，绝不改指向。
- `SwitchableBinding` 必须通过 `GenerationBindingRequirement` 显式创建；每次
  `pin()` 原子取得一个快照并返回 `PinnedBinding`，调用者只通过该快照调用。
- `SafePoint` 是由 `safePoint()` 创建、move-only 且只能使用一次的 token。它表示
  宿主确认当前可切换，不会替宿主暂停线程；`activate` 与 `rollback` 都要求来自同一
  `MoonRuntime` 的全新 token。
- `RuntimeFragmentBindingSet` 是宿主 Slot 策略生成的不可变结果。它拥有已经验证的 Fragment
  reference 并固定其 generation。严格构造器要求每个精确 Slot 最多一个 binding；显式 chain
  构造器则保留宿主顺序。`activateFragmentBindings` 使用同一 safe-point 协议发布，
  `pinFragmentBindings` 返回稳定的 dispatch snapshot。局部 override 可以替换或屏蔽一个 Slot，
  且不会修改任一 snapshot。
- `RuntimeFragmentExecutionContext` 把一个 pinned BindingSet 转为传给生成后 runtime-aware entry
  的显式 data-plane capability。它不查找 ambient Runtime；即使宿主在调用期间激活替代集合，
  该 context 仍保持原 snapshot。

typed binding requirement 包含 `symbolId`、`contractId`、`declarationKind` 与
`requiredFlags`。switchable binding 建立后，runtime 会把当时 active binding 的精确
kind/flags 固定为后续每次 activation 的最低要求。兼容性检查发生在发布新的不可变
generation pointer 之前，不进入普通调用热路径。
`GenerationBindingFragmentContext` 只能与 `GenerationBindingCallable` 一起用于 Function
binding。它告知宿主 implementation pointer 会在源码声明参数之前接收一个 opaque
`RuntimeFragmentExecutionContext` pointer；这是 ABI capability，不是候选发现请求。

## 生命周期

公开拼写如下：

```cpp
#include <luna/runtime/Evolution.h>

luna::runtime::MoonRuntime runtime;
luna::runtime::MoonRuntime::StagedGeneration staged;
std::string error;

bool ok = runtime.stage(request, verifier, resolver, initializer,
                        staged, error);
if (ok) {
    auto safePoint = runtime.safePoint();
    ok = runtime.activate(staged, safePoint, error);
}

auto pinned = runtime.pin(moduleId);
auto entry = pinned.find(requirement);

luna::runtime::MoonRuntime::SwitchableBinding switchable;
ok = runtime.makeSwitchable(moduleId, requirement, switchable, error);
auto currentEntry = switchable.pin();

auto rollbackPoint = runtime.safePoint();
ok = runtime.rollback(moduleId, oldGenerationId, rollbackPoint, error);

luna::runtime::RuntimeFragmentBindingSet selected;
ok = luna::runtime::makeRuntimeFragmentBindingSet(
    std::move(fragmentRefs), selected, error);
auto bindingPoint = runtime.safePoint();
ok = ok && runtime.activateFragmentBindings(
    selected, bindingPoint, error);
auto dispatchSnapshot = runtime.pinFragmentBindings();
luna::runtime::RuntimeFragmentExecutionContext executionContext;
ok = ok && luna::runtime::makeRuntimeFragmentExecutionContext(
    dispatchSnapshot, executionContext, error);
```

仓库内 Moon/Native generation adapter 会提供已验证产物专用的 verifier/resolver callback
与被保留的 loader/JIT lease。通用公开 `stage` 是信任边界：提供其他 callback 的嵌入
宿主必须负责认证 artifact、核对 target 与 descriptor ABI，并且只解析已验证 entry。
本 API 不会让任意 implementation pointer 自动变得可信。

## 失败与寿命规则

编译器内部 Moon adapter 可显式选择 `MoonJitOptimization::O0`／`O2`／`O3`，默认
仍为 O0。配置选择 LLVM IR 优化流水线，ORC 机器码生成保持默认设置。生成配置 key
包含 adapter profile 版本、LLVM 版本、IR 级别及 ORC 策略。缓存复用前仍验证
target/layout、完整性与依赖 Slot 证据。未知配置与同内容／不同 key 请求失败时，不改变
发布状态或输出句柄。key 是可信 loader 的声明，不是签名或安全证明；native loader
须如实记录影响代码生成的配置。Native adapter 的行为不变。

验证、解析、初始化、兼容性检查、activation 或 rollback 失败，都不改变先前 active
generation。initializer 在 staging 时执行，可能已经产生外部副作用；若 staging 或后续
activation 失败，Luna 不会反向撤销这些副作用。因此 initializer 应由宿主管理，并明确
设计失败行为。

0.3 保留全部已激活 generation 及其 module lease；rollback 只会重新发布一个 retained
generation。本版本不提供 persistent-state migration、自动更新发现、隐式路径身份、代码
回收、hotspot JIT policy 或 cross-target container activation。

EV001–EV004 决定见[0.3 总体设计](luna_0.3_design.zh-CN.md#c014moonruntime-承担进化confirmed-direction)，
可执行状态机与真实产物证据见[测试与回归](testing.zh-CN.md)。
