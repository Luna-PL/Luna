# src/moonir/Lowering.cpp

LunaLowerer 的完整实现：把前端（AST+SymbolTable）递归降级为 MoonIR Module，含类型冻结、声明引用解析与模块导入/导出构建。

## 这个文件做什么

单趟构建模块模型：

1. lower() 初始化 Module（包名、packageUses、features），对每个 declaration 调 lowerDecl；
2. 需要时给编译器内置 Drop/From trait 在声明表补一行规范声明（源码没有的 trait 也以行呈现）；
3. 遍历 kernel 声明，按 --reserve-kernel-runtime 或被引用决定 isCodegenReachable，并记录 kernel cost；
4. sealTypeTable()（统一 identity/layout）→ resolveDeclarationReferences()（把推迟的引用解析成 symbol+contract）→ buildModuleInterfaces()（导入/导出）。

## 关键函数·方法

| 函数 | 作用 |
| --- | --- |
| lower() | 顶层流程：建 Module、降声明、注入内置 trait、算 kernel cost、seal、resolve、buildInterfaces |
| lowerDecl/lowerFunction/lowerStmt/lowerExpr/lowerBlock | 各构造体的下降。 |
| lowerCommonDeclaration | 填充共享声明字段（packageId/identity/sysmeta 等）。 |
| deferDeclarationRef / resolveDeclarationReferences | 推迟声明引用、阶段末统一解析。 |
| buildModuleInterfaces | 生成 imports（包/host）与 exports 并排序。 |
| lowerRetention/lowerOperator/lowerParam | 保留/操作符/参数映射。 |
| inferredExprType / addDeclarationRecord | 类型推断；登记规范声明记录。 |

## 与周边文件·阶段的关系

- 上游：parser/sema 的 Program + SymbolTable。
- 下游：产出 MoonIR Module，交给 Sealer/ControlFlowBuilder/Verifier 至容器化。
- 阶段：前端语义 → MoonIR 的第一步实际产出。

## 延伸阅读
- src/moonir/Lowering.h：声明接口。
- src/moonir/MoonIR.h：Module/DeclarationRecord 定义。
- src/moonir/Sealer.cpp：下降后密封函数体。


---

---
title: LunaLowerer —— 前端 AST 到 MoonIR Module 的下降接口
file: src/moonir/Lowering.h
namespace: moon
阶段: 前端 → MoonIR 下降（前端 Lowering）
---

# src/moonir/Lowering.h

声明 LunaLowerer：把前端语义分析后的 Program（AST + SymbolTable）一次性编译为 MoonIR 的 Module 的下降器。

## 这个文件做什么

Luna 从"源码 AST"到"可验证/可序列化的 MoonIR Module"的翻译入口。它的职责是把类型、声明、表达式、语句、函数逐一带到 MoonIR 命名，收集需推迟解析的声明引用（PendingDeclarationRef），再在阶段末统一 resolve 并构建模块接口（导入/导出）。

- 产出物：std::unique_ptr<Module>（类型与声明表已注册、随后可被 Sealer 构造 CFG）。
- 错误：累积到 mErrors（diagnostic::Diagnostic），供诊断层使用。

类比 C++ 读者：一批从文法（AST 类）到中间表示（IR 类）的递归翻译函数，配合一张待解析符号表（类似 deferred 名称解析）。

## 关键结构体·类

| 成员 | 含义 |
| --- | --- |
| class LunaLowerer | 下降器；lower() 为主入口。 |
| struct PendingDeclarationRef | 延迟解析的声明引用：待写的 DeclarationRef* 目标 + lookup 名 + 来源 AST 节点。 |

状态成员：mProgram/mSymbols/mModule 指针、mReserveKernelRuntime、mRequiredKernelSymbols、mPendingDeclarationRefs、mErrors。

## 关键函数·方法

| 函数 | 作用 |
| --- | --- |
| lower(program, symbols, reserveKernelRuntime) | 顶层入口：建 Module、逐声明 lower、补注入内置 Drop/From trait 行、处理 kernel 可达性、sealTypeTable、resolveReferences、buildInterfaces。 |
| lowerType/lowerExpr/lowerStmt/lowerBlock/lowerDecl/lowerFunction | 各语言层结构的下表。 |
| lowerParam/lowerOperator/lowerRetention | 参数/操作符/保留语义的映射。 |
| addDeclarationRecord | 给下降出的 Decl 写规范 DeclarationRecord。 |
| deferDeclarationRef/resolveDeclarationReferences | 推迟 & 统一解析声明引用（包括 byId 的内建 trait 等）。 |
| buildModuleInterfaces | 填 module->imports/exports（包导入、host import、导出），并排序。 |
| inferredExprType / typeRef / typeRefs | 类型推导与 TypeRef 转录辅助。
| error | 记录带位置诊断。 |

## 与周边文件·阶段的关系

- 上游：parser/sema 产出的 Program + SymbolTable。
- 下游：产出 Module，交给 Sealer（构造 CFG 并验证）。
- 依赖：core/TypeSystem、MoonIR.h、diagnostics。
- 阶段：map 前端语义 → MoonIR 的第一环。

## 延伸阅读

- src/moonir/Lowering.cpp：全部实现。
- src/moonir/MoonIR.h：Module 与结构。
- src/moonir/Sealer.cpp：下降后如何密封函数体。


---

---
title: MoonIR 核心执行：类型注册、密封、查找与具体化
file: src/moonir/MoonIR.cpp
namespace: moon
阶段: 前端 Lowering 产物 / 后端与运行时输入
---
