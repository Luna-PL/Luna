> Document category: implementation note / tutorial
> Applies to: Luna 0.3.0 development
> Status: Implemented Experimental
> Normative status: non-normative（本文讲代码怎么走，不定义语言契约）
> 语言：中文权威版（英文版为占位）

# 整体流水线走读：一个 .luna 程序从源码到可执行

读完前置课 A（LLVM）与 B（ABI/布局）之后，把整条管线串起来看一遍。本文是"从 main() 出发，直到跑出程序"的完整旅程。

## 0. 总览图

```text
hello.luna
   │  Driver::run + CommandLine
   ▼
AnalysisSnapshot::analyzePath   ──►  lexer → parser → AST
   │                                     │
   │        SemanticAnalyzer / TraitChecker / OwnershipChecker
   ▼                                     ▼
   │  program + symbolTable
   ▼ ────────────────────────────────────┘
CompilerPipeline::lowerAnalyzedProgram
   │  LunaLowerer.lower            (moon-lower)
   ▼
   moon::Module   （未密封 MoonIR）
   │  Verifier::verify             (moon-verify)
   ▼
   ✓ 已验证 MoonIR
   │  Sealer::sealFunctionBodies   (moon-seal)
   ▼
   verified canonical MoonIR（函数体全部密封）
   │  CodeGenerator::generate
   ▼
   llvm::Module → 优化
   │
   ├─ JIT  : LLJIT::jitRun() → 直接运行 main
   └─ AOT  : emitObjectFile → AotLinker → 可执行
```

## 1. 起点：main.cpp 与 Driver

src/main.cpp 只有两行：
```cpp
#include "driver/Driver.h"
int main(int argc, char* argv[]) { return luna::driver::run(argc, argv); }
```
真正的分派在 src/driver/Driver.cpp 的 run()：解析命令行（CommandLine），根据模式分发到 run / check / build / `-t moon` / REPL 等分支。

## 2. 前端：AnalysisSnapshot

CompilerPipeline::compileToMoonIR 第一步（真实代码）：
```cpp
auto snapshot = luna::tooling::AnalysisSnapshot::analyzePath(options.inputPath);
mAnalysisSnapshot = std::make_unique<AnalysisSnapshot>(std::move(snapshot));
if (!mAnalysisSnapshot->success()) return fail(...);
auto* program = mAnalysisSnapshot->program();
```

AnalysisSnapshot（src/tooling/AnalysisSnapshot.*，内部调用 lexer/parser/sema）只产出一个**已分析、无错误**的 Program（顶层结构 + 语义结果）以及 symbolTable。

> 位置：整个前端 = lexer + parser + AnalysisSnapshot（sema）。这是"读入并理解"的部分。

## 3. MoonIR 生成：Lowering → verify → seal

拿到 analyzed program，CompilerPipeline::lowerAnalyzedProgram（真实代码）：
```cpp
moon::LunaLowerer lowerer;
mMoonModule = lowerer.lower(
    *program, *mAnalysisSnapshot->symbolTable(), options.reserveKernelRuntime);
moon::Verifier verifier;
if (!verifier.verify(*mMoonModule)) return fail(verifier.errors(), "moon-verify");
moon::Sealer sealer;
if (!sealer.sealFunctionBodies(*mMoonModule)) return fail(...);
```

三步把语义程序变成 verified, sealed MoonIR：
- **Lowering**（src/moonir/Lowering.cpp）：把程序翻译成 moon::Module 的声明与指令表。
- **Verifier**（src/moonir/Verifier.cpp）：检查结构完整性（类型引用、表索引不越界）。
- **Sealer**（src/moonir/Sealer.cpp）：把可执行函数体转成规范 CFG（"密封"），后端只见到单一表示。

值得一提的是，`-t moon` 等用 "确定可序列化/已验证加载" 的 Moon Container 在不同消费者间搬运同一份 MoonIR。

## 4. Codegen：MoonIR → LLVM IR → 机器码

CompilerPipeline 随后调 CodeGenerator（src/codegen/*）。它对 sealed MoonIR 每个函数翻译成一个 llvm::Function：alloca 分配局部、每节点映射为指令、CFG 映射为基本块与分支（详见 [codegen 导读](./codegen.zh-CN.md)）。随后：

- **JIT 模式**：CodeGenerator::jitRun() 用 ORC / LLJIT 加载，找到 main 运行并返回退出码。
- **AOT 模式**：CodeGenerator::emitObjectFile(outputPath) 产出目标文件，AotLinker 调用系统链接器变成可执行。

## 5. 优化级别

`LunaOptimizationLevel { O0, O2, O3 }` 由 CommandLine 解析后传给 codegen。O0 便于调试；O2/O3 走 LLVM 优化。

## 6. 各阶段错误上报

CompilerPipeline 在每个阶段失败时用 fail() 记录一个打上阶段名的诊断（如 "moon-verify"），Driver 顶部再统一 printErrors 输出。阶段名帮你判断"错在哪个阶段"。

## 7. 给 C++ 读者的"新概念"清单

- **AnalysisSnapshot**：前端产出（program + 符号表）的容器，好的"模块封装"。
- **Lower/Verify/Seal 三分离**：翻译、校验、归一化三件事分开，错误可定位到阶段。
- **单表示契约**：后端只吃 sealed 的规范 CFG，防止前端泄露任意表示。

## 8. 继续阅读

- 后端映射细节：[codegen 导读](./codegen.zh-CN.md)
- MoonIR 数据模型：[moonir 导读](./moonir.zh-CN.md)
- 语义侧：[sema 导读](./sema.zh-CN.md)
- Driver 细节：[frontend_driver_runtime 导读](./frontend_driver_runtime.zh-CN.md)
