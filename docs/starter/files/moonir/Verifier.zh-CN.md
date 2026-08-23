# src/moonir/Verifier.cpp

Verifier.h 的完整实现（约 3600 行）：对 Module 与 ControlFlowGraph 执行数百条冻结不变式检查，任一违反即记入诊断并返回 false。

## 这个文件做什么

两个入口：

**verify(Module)** （约 530 行开始）：
- 格式版本、模块名、sourceModule 去重、packageUses 别名不冲突；
- 类型表：每个 TypeRecord 的 id/kind/domain/identity/sysmeta 等字段完整性，shape/abiLayout 一致性，referencedTypeIds 引用可解析；
- 声明表：每条 DeclarationRecord 的 symbolId/contractId/type 引用有效，DeclarationRef 跨表可解析，字段名与类型匹配；
- 导入/导出/声明符号表一致性，extern 与入口点约束。

**verify(CFG, module)** （约 289 行开始）：
- 要求 typeTable 与 graph 均已 sealed；
- verifyCanonicalTables：blocks/regions/scopes/locals/cleanups 表索引完整、parent/scope 链无环；
- verifyRegions：region 层次结构、root 锚点、块属主（block 的 region 与 scope 匹配）；
- 随后逐块校验：stmt 操作数类型、Expr 类型引用、清理动作与 local 类型匹配、terminator 的跳转目标有有效目标块；
- 递归 verifyStmt/verifyExpr 覆盖全部节点类型。

## 关键函数·方法

| 函数 | 作用 |
| --- | --- |
| verify(const Module&) | 整模块 500+ 条检查。 |
| verify(const CFG&, const Module&) | 单图结构/类型/清理检查。 |
| verifyDeclaration/Function/Block/Stmt/Expr | 递归下行。 |
| verifyCanonicalTables / verifyRegions | CFG 表完整性与 region 结构。 |
| verifyCleanupAction / verifyType / verifyDeclarationRef | 简单语义检查。 |

## 与周边文件·阶段的关系

- 被 Sealer（密封前）、ContainerModel（容器化前后）、以及装载路径调用。
- 读取 MoonIR.h 全部结构，依赖 core/ownership、core/types、diagnostics。
- 阶段：贯穿密封、容器化、装载的"不变式门闸"。

## 延伸阅读

- 接口：src/moonir/Verifier.h。
- 被调：src/moonir/Sealer.cpp、src/moonir/ContainerModel.cpp。
- 结构：src/moonir/MoonIR.h。


---

---
title: Verifier 接口：MoonIR 的完整性校验器
file: src/moonir/Verifier.h
namespace: moon
阶段: 密封/容器化/装载前的门闸校验
---

# src/moonir/Verifier.h

声明 MoonIR 的校验器：验证一个 Module 或一个 ControlFlowGraph 是否满足所有冻结不变式，失败时给出诊断。

## 这个文件做什么

提供两个验证入口：verify(Module) 整模块校验、verify(graph, module) 单图校验；以及一个 errors() 访问器。私有方法族是各类检查的分解：

- 声明/函数/块/语句/表达式逐级校验（verifyDeclaration/Function/Block/Stmt/Expr）；
- 类型与引用校验（verifyType/verifyDeclarationRef）；
- 清理动作校验（verifyCleanupAction）与 CFG 结构校验（verifyCanonicalTables/verifyRegions）。

这类"先验证再发布"的纪律让它成为容器化、装载、密封的共同门闸。

## 关键结构体·类

| 类 | 目的 |
| --- | --- |
| class Verifier | verify() ×2、errors()，私有 *verify* 族。 |

## 关键函数·方法

| 函数 | 作用 |
| --- | --- |
| verify(const Module&) | 整模块校验（类型表、声明表、类型引用可解析、sysmeta 一致等）。 |
| verify(const ControlFlowGraph&, const Module&) | 图结构校验（region 结构、块属主、清理不变式）。 |
| verifyDeclaration/Function/Block/Stmt/Expr | 递归例行分解。 |
| verifyCanonicalTables / verifyRegions | CFG 规范表与 region/块属主结构检查。 |
| verifyCleanupAction / verifyType / verifyDeclarationRef | 单一职责的局部校验。 |
| error(location, message) | 记一条诊断到 mErrors。 |

## 与周边文件·阶段的关系

- 被 Sealer（密封前验证 graph）、ContainerModel（容器化前后验证 Module）、以及下游装载路径调用。
- 依赖 MoonIR.h 结构与 diagnostics::Diagnostic，涉及 core/ownership、core/types。

## 延伸阅读

- 实现：src/moonir/Verifier.cpp。
- 被调：src/moonir/Sealer.cpp、src/moonir/ContainerModel.cpp。
- 结构：src/moonir/MoonIR.h。


---
