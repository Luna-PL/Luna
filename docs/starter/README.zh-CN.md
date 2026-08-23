# Luna 源码阅读入门手册（Starter Manual）

> Document category: tutorial / implementation note
> Applies to: Luna 0.3.0 development
> Status: Implemented Experimental
> Normative status: non-normative（本手册只讲"怎么读代码"，不定义语言契约；语言规则以 `docs/reference/` 为准）
> Implementation audit: 代码结构审计于本次编写
> 语言：中文权威版（英文版为占位，后续统一生成）

本手册面向**学过 C/C++ 的开发者**，帮助你从头读懂 Luna 编译器工程的**全部 `src/` 源码**（约 4.9 万行 C++）。它**不假设**你了解：

- LLVM 的 C++ API（`llvm::IRBuilder`、`llvm::Type`、BasicBlock、IR 指令……）；
- 内存布局（struct 对齐、对象头、tag、引用计数）；
- ABI / 调用约定（SysV x86-64、栈传参、寄存器传参）；
- 编译原理内部术语（CFG、SSA、lowering、sealing……）。

每条概念都会先用 **C/C++ 类比**解释，再落到真实代码。

## 为什么需要这份手册

Luna 是"静态优先、按使用付费"的系统语言编译器。编译管线：

```text
source/package
  -> Lexer / Parser
  -> SemanticAnalyzer / TraitChecker / OwnershipChecker
  -> verified MoonIR
  -> MoonIR optimizer
  -> LLVM lowering
  -> ORC JIT  或  textual LLVM IR + native AOT linker
```

`MoonIR` 是前后端之间唯一的中间表示契约（`src/moonir/` 约 1.5 万行，全工程最大的一块）。要`读懂所有源码`，应按**编译管线顺序**读，而不是按目录顺序。

## 阅读路径（推荐顺序）

| 顺序 | 主题 | 对应文档 |
|---|---|---|
| 1 | 前置课 A：LLVM C++ API | [prerequisites_llvm.zh-CN.md](prerequisites_llvm.zh-CN.md) |
| 2 | 前置课 B：ABI / 内存布局 / 调用约定 | [prerequisites_abi.zh-CN.md](prerequisites_abi.zh-CN.md) |
| 3 | 整体流水线走读 | [pipeline.zh-CN.md](pipeline.zh-CN.md) |
| 4 | MoonIR（最大、最关键） | [moonir.zh-CN.md](moonir.zh-CN.md) |
| 5 | 语义分析 Sema | [sema.zh-CN.md](sema.zh-CN.md) |
| 6 | 代码生成 Codegen | [codegen.zh-CN.md](codegen.zh-CN.md) |
| 7 | 前端(词法/语法/AST)、驱动与运行时 ABI | [frontend_driver_runtime.zh-CN.md](frontend_driver_runtime.zh-CN.md) |
| 8 | 核心类型系统、编译辅助（core/tooling/package/selector） | [core_tooling_rest.zh-CN.md](core_tooling_rest.zh-CN.md) |
| 9 | 术语表 | [glossary.zh-CN.md](glossary.zh-CN.md) |
| 10 | 调试技巧 | [debugging.zh-CN.md](debugging.zh-CN.md) |

顺序读过 1–8，其余目录基本都是同一套思想的落点，可跳读。


## 逐文件源码指南（每个 src 目录一份）

`docs/starter/files/` 下为 `src/` 每个子目录提供一份合并指南，覆盖该目录全部源码文件。每个指南包含目录下所有文件的责任、关键结构体/类/枚举、关键函数。

```text
files/  (16 份合并指南 + 英文占位 = 32 文件)
  codegen.md / codegen.zh-CN.md     — 代码生成后端（15 文件）
  core.md / core.zh-CN.md           — 核心类型系统（10 文件）
  moonir.md / moonir.zh-CN.md       — 中间表示 MoonIR（18 文件）
  sema.md / sema.zh-CN.md           — 语义分析 Sema（26 文件）
  driver.md / driver.zh-CN.md       — 驱动/CLI（10 文件）
  lexer.md / lexer.zh-CN.md         — 词法分析（3 文件）
  parser.md / parser.zh-CN.md       — 语法分析/AST（3 文件）
  runtime.md / runtime.zh-CN.md     — 运行时/ABI（6 文件）
  tooling.md / tooling.zh-CN.md     — 代码导航/快照（8 文件）
  package.md / package.zh-CN.md     — 包管理（4 文件）
  selector.md / selector.zh-CN.md   — 编译期选择器（2 文件）
  instantiation.md / — 泛型实例化（2 文件）
  macro.md / macro.zh-CN.md         — 宏处理（2 文件）
  diagnostics.md / — 诊断（1 文件）
  main.md / main.zh-CN.md           — 入口（1 文件）
  version.md / version.zh-CN.md     — 版本号（1 文件）
```

> 内容由后台代理逐目录合并产出；符号均经源码核实。

## 库内既有文档导航

- [文档体例（必读，区分契约/实现/内部表示/计划）](../reference/documentation_rules.md)
- [类型系统契约](../reference/type_system.md)
- [0.3 整体设计](../luna_0.3_design.md)
- [架构说明](../architecture.md)
- [文件与责任清单](../file_guide.md)
- [Runtime ABI](../runtime_abi.md)

## 双语与文件命名约定

每篇成对：英文默认名（`.md`，当前为英文占位）+ 中文实际内容（`.zh-CN.md`）。
后续英文统一生成时，翻译并替换到对应 `.md`，文件名与链接不变。代码块、类型名、命令、路径、错误码**不翻译**。