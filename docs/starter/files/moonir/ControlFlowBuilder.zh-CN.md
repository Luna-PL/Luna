# src/moonir/ControlFlowBuilder.cpp

ControlFlowBuilder.h 的完整实现（约 6300 行）：把结构化语句/表达式逐类展开成 canonical CFG，处理作用域、清理义务、迭代器配方、fragment/slot/动态调度与捕获 lambda。

## 这个文件做什么

核心入口 build() 先克隆结构体（防副作用、控嵌套深度），建 root region/scope/block，声明式：

1. 如需捕获 lambda，设环境参数 local 并把 body 里对捕获名的读取重写为 EnvLoad（rewriteCaptureReads*）；
2. 把参数逐一登记为 Parameter local（并按冻结类型的 usage 加强 Affine/Linear）；
3. 递归 lowerSequence/lowerStatement 处理整棵语句树，末尾挂 TerminatorKind::Return；
4. canonicalizeCleanupTable()（去重、规范化清理表）；
5. 任何错误 → graph 不标记 sealed 并返空。

匿名命名空间还提供深度守卫 kMaxStructureDepth=4096 与 clone 家族（clonedNode/cloneStructured*）。

## 关键函数·方法

| 函数 | 作用 |
| --- | --- |
| build()×2 | 构造整图；非消耗式先 clone 再构，供 Sealer 原子试构。 |
| lowerSequence/lowerStatement | 顺序与单条语句展开。 |
| lowerIf/lowerWhile/lowerFor/lowerMatch/lowerApply/lowerSlotInvoke/lowerResume | 各类语句。 |
| normalizeControlFlowExpression/lowerIfExpr/TryExpr/ShortCircuit/Block/RecordAllocation/HeapAllocation | 控制流表达式归一化，用 replacement 传回 new Expr。 |
| parseIteratorRecipe/validateIteratorRecipe/bindIteratorRecipe/materializeIteratorRecipe | 迭代器管线：解析配方、校验、绑定、物化成带 cursor 的局部。 |
| containsIteratorTerminal/containsPendingControlFlow/containsPotentialEarlyExit/hoistOrderedOperand | 判定是否有需要在构造中提前展开的控制流/迭代器终止；操作数提升。 |
| lowerCleanupObligations/canonicalCleanupOrder/canonicalizeCleanupTable | 清理义务与规范顺序。
| bindExpr(set 绑定) | 给表达式绑定 locals/declaration 引用。 |
| rewriteCaptureReads* | 引用捕获为 EnvLoad。 |

> 内部结构 OpenBlock/BuiltBlock/FragmentContext/IteratorRecipePlan/MaterializedIteratorRecipe 已在 .h 指南详述。

## 与周边文件·阶段的关系

- 调用者：Sealer::sealFunctionBodies 对每个具体可执行函数调用 build()。
- 校验：输出图交给 Verifier::verify(CFG) 验证 region/scope/跳转清理不变性。
- 下游：ContainerModel encodeCode 序列化 CFG。
- 阶段：密封（sealing）这一环节的"构造半边"。

## 延伸阅读

- src/moonir/ControlFlowBuilder.h：接口与内部结构。
- src/moonir/Sealer.cpp：原子试构的调用姿势。
- src/moonir/Verifier.h：图结构检查。
- src/moonir/MoonIR.h：ControlFlowGraph 表示。


---

---
title: ControlFlowBuilder —— 结构化语句到规范化 CFG 的唯一构造桥
file: src/moonir/ControlFlowBuilder.h
namespace: moon
阶段: MoonIR 密封前的 CFG 构造
---

# src/moonir/ControlFlowBuilder.h

声明把前端 Lowering 产生的结构化语句树（BlockStmt）编译成单条规范化 ControlFlowGraph 的构造器，是 Sealer 密封函数体的灰箱。

## 这个文件做什么

这是"结构化 → 规范化控制流"的唯一桥。它消费 frontend 的结构化 body（绝不留在 sealed Module 里），产出由 region/scope/block/local/cleanup 五张表组成的 canonical CFG。

它不只是简单展开语句：它负责

- 顺序、if/while/for/match、块表达式、`?`（TryExpr）、短路（&&/||）、record/heap 分配等结构的控制流归一化；
- 把迭代器（iterator recipe）解析、验证并具体化为"materialized iterator"；
- fragment apply/slot invoke/动态调度候选的图构造；
- 捕获 lambda 的合成环境参数与 EnvLoad 重写（C016 CL007）；
- 逐条的规范清理（cleanup）顺序与义务绑定。

## 关键结构体·类

| 成员 | 含义 |
| --- | --- |
| class ControlFlowBuilder | 主类；build()×2 对外，私有 lower* 系列实现。 |
| struct OpenBlock | 构造期游标：当前块 + 活跃 cleanup。 |
| struct BuiltBlock | 一个已构造的 region/scope/entry/exit 组合。 |
| struct FragmentContext | 克隆的静态 fragment 体的跳转上下文（return/abort 结束的是 fragment 而非外层函数）。 |
| struct IteratorRecipePlan / MaterializedIteratorRecipe | 迭代器配方：mode、source、range、step 与被物化的 local/cursor。 |

## 关键函数·方法

| 函数 | 作用 |
| --- | --- |
| build(root, parameters, regionKind, module) ×2 | 构造整图；非消耗式重载供 Sealer 在移除 body 前先试构与验证。 |
| setCaptureEnvironment(captures, closureType, envParamName) | 声明式：打开后 build() 会设环境参数并重写捕获读取为 EnvLoad。 |
| lowerIf/While/For/lowFor/match/apply/slotInvoke/resume... | 各类语句的结构化展开。 |
| lowerIfExpr/TryExpr/ShortCircuit/Block/RecordAllocation/HeapAllocation | 控制流表达式的归一化。 |
| parseIteratorRecipe/validate/bind/materialize | 迭代器配方的解析-校验-绑定-具体化管线。 |
| lowerCleanupObligations/canonicalCleanupOrder/canonicalizeCleanupTable | 清理义务的降级与规范化排序。 |
| connectJump/pushBindings/popBindings/lookupLocal | 跳转边与词法绑定的辅助。 |

## 输入/状态

构造器维护多张作用域栈：mBindings、mMaterializedIterators、mSlotDefaults、mLexicalDynamicApplies、mDynamicApplyScopes、mFragmentContexts，以及 mCaptureNames/mCaptureClosureType 等捕获上下文。mErrors 收集失败；graph 未 seal 时返空。

## 与周边文件·阶段的关系

- 上游：Lowering 产出结构化 body 的 Module。
- 自身阶段：密封（sealing）——示例 Sealer 调用它 + Verifier，先试构所有候选图、逐个验证，全部通过后才把 body 换成 CFG。
- 下游：Sealer 持有图；Verifier.verify(CFG) 结构校验；ContainerModel 序列化 graph。

## 延伸阅读

- src/moonir/ControlFlowBuilder.cpp：all 实现的几百个函数。
- src/moonir/Sealer.cpp：如何在构建前做原子试构。
- src/moonir/Verifier.h：对图结构的校验。
- src/moonir/MoonIR.h：CFG 与各结构体定义。
- src/moonir/MoonIR.h：CFG 与各结构体定义。

---

---
title: LunaLowerer 实现：从 AST 到 MoonIR Module
file: src/moonir/Lowering.cpp
namespace: moon
阶段: 前端 → MoonIR 下降
---
