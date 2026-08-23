# src/moonir/Optimizer.cpp

Optimizer.h 的当前实现：run() 仅调用 canonicalize(module)，后者重建 Module 的索引 map。

## 这个文件做什么

把 MoonIR 优化边界落定为先保证表示一致、再谈性能。当前 canonicalize 只做 index 重建：

- lookup map 属于派生状态，永不进容器；
- 反序列化或变换后的 module 必须在此重新 build indexes，防止把过期的前端指针带进后端。

- C++ 类比：一个快照重建步骤，类似加载后统一重建 unordered_map 索引。

## 关键函数

| 函数 | 作用 |
| --- | --- |
| Optimizer::run(Module&) | 清空错误、走 canonicalize，依 errors 决定返回。 |
| Optimizer::canonicalize(module) | 仅调用 module.rebuildIndexes()。 |

## 与周边文件·阶段的关系

- 配合 Optimizer.h（接口）与 MoonIR::rebuildIndexes。
- 阶段：Verifier 之前、可序列化的后处理。

## 延伸阅读
- OptimizationLevel / OptimizationRequest 见 src/moonir/Optimizer.h。
- Module::rebuildIndexes 见 src/moonir/MoonIR.cpp。


---

---
title: Optimizer 接口：MoonIR→MoonIR 优化
file: src/moonir/Optimizer.h
namespace: moon
阶段: MoonIR 后处理（容器化之前）
---

# src/moonir/Optimizer.h

声明 MoonIR 层面的优化器接口，与 LLVM 优化刻意保持独立；该边界未来可被 MoonRuntime 在容器验证后复用。

## 这个文件做什么

为给定 Module 提供表示规范化的优化入口。当前实现只做 canonicalization（重建索引）；真正的语言级变换与运行热点版本化作为后续要校验的 MoonIR-to-MoonIR pass 保留，LLVM 优化属于后端职责。

- C++ 类比：一个 pass 框架的壳，目前只有一个重建索引的块。

## 关键结构体·类·枚举

| 成员 | 含义 |
| --- | --- |
| enum OptimizationLevel | None / Standard / Aggressive 优化级别。 |
| enum OptimizationPurpose | AheadOfTime / JustInTime / RuntimeHotspot。|
| struct OptimizationRequest | level + purpose 的请求。|
| class Optimizer | run() 主入口、errors() 取诊断。|

## 关键函数·方法

| 函数 | 作用 |
| --- | --- |
| bool run(Module&, const OptimizationRequest&) | 跑优化；现仅调 canonicalize。|
| void canonicalize(Module&) 私有 | 重建 Module 索引映射，保证变换/反序列化后无过期前端指针。|

## 与周边文件·阶段的关系

- 依赖 MoonIR.h、diagnostics；位于 Verifier / ContainerModel 之前的优化层。

## 延伸阅读
- src/moonir/Optimizer.cpp：唯一的 canonicalize 实现。
- src/moonir/Verifier.h：优化前后结构校验。
- src/moonir/ContainerModel.h：优化后整体序列化。

---

---
title: Printer 实现：模块文本化与成本报告
file: src/moonir/Printer.cpp
namespace: moon
阶段: 诊断 / 测试
---
