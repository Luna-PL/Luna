# src/moonir/Sealer.cpp

Sealer.h 的实现：把"结构化 body"批量、原子地转换为"已验证 CFG"。

## 这个文件做什么

核心 sealFunctionBodies 采用**两阶段提交**式流程：

1. **准备阶段**：遍历 module.declarations（顶层 FunctionDecl + ImplDecl::methods），对每个"具体可执行"函数：
   - isConcreteExecutable 过滤：排除 extern、selector、不可达 kernel、未实例化的泛型；
   - 校验"恰好拥有一种体"（body 与 controlFlow 二选一，同时为真或同时为假即报错）；若已有 controlFlow 跳过；
   - 用 ControlFlowBuilder 造图；失败则收集 builder/verifier 的报错并入 mErrors；
   - 通过后暂存到 pending（不立即写入）。
2. 提交阶段：若 mErrors 为空，才把所有 pending 的 controlFlow 移入 Module、release body；否则返回 false，一个都不发布。

## 关键函数·方法

| 函数 | 作用 |
| --- | --- |
| bool sealFunctionBodies(Module&) | 准备+提交两阶段的主体。 |
| isConcreteExecutable(function)（匿名） | 判定函数是否为"应密封的可执行体"。 |

## 与周边文件·阶段的关系

- 调用 ControlFlowBuilder::build 与 Verifier::verify(graph, module)。
- 通过一个 FunctionDecl 必须二选一的约束（body ↔ controlFlow）与 MoonIR.h 保持一致。
- 阶段：Lowering 之后、Verifier/ContainerModel 之前的密封环节。

## 延伸阅读

- 接口：src/moonir/Sealer.h。
- 造图：src/moonir/ControlFlowBuilder.h。编译期约束：src/moonir/MoonIR.h。


---

---
title: Sealer 接口：事务性密封函数体
file: src/moonir/Sealer.h
namespace: moon
阶段: MoonIR 密封（构造 CFG）
---

# src/moonir/Sealer.h

声明 Sealer：把结构化函数体（body）原子性地替换为已验证的规范化 ControlFlowGraph。

## 这个文件做什么

这是 body→密封体 的**原子事务边界**：为所有具体可执行函数调用 ControlFlowBuilder 造图，全部构造并验证通过后才统一把 body 换成 CFG——保证 Module 不会落入"一半是 body、一半已是 CFG"的中间态。

- 注释明言：generic、selector、deferred-kernel、fragment 等"配方"仍是编译器输入，等专门 canonicalization 切片完成后才处理（当前只管具体函数）。
- C++ 类比：一种提交式变更集——先全部 build+verify，成功才 commit；任一失败即整体回滚，不发布任何体。

## 关键结构体·类

| 类 | 目的 |
| --- | --- |
| class Sealer | sealFunctionBodies 主入口 + errors()。 |

## 关键函数·方法

| 函数 | 作用 |
| --- | --- |
| bool sealFunctionBodies(Module&) | 遍历顶层声明与 impl 方法，为具体可执行者逐一构建/验证，全部成功后原子替换 body→controlFlow。 |
| errors() 常量 | 返回累积的错误字符串列表。 |

## 与周边文件·阶段的关系

- 调用 ControlFlowBuilder（造图）与 Verifier（验证）。
- 处理对象：MoonIR Module 的 FunctionDecl / ImplDecl::methods。
- 上游：Lowering 产出的带 body 的 Module；下游：带 sealed CFG 的 Module 供容器化。

## 延伸阅读

- 实现：src/moonir/Sealer.cpp。
- 造图：src/moonir/ControlFlowBuilder.h/.cpp。
- 验证：src/moonir/Verifier.h。


---

---
title: Verifier 实现：MoonIR 的完整性校验
file: src/moonir/Verifier.cpp
namespace: moon
阶段: 密封 / 容器化 / 装载前的门闸校验
---
