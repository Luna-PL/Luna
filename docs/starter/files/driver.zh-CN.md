> Document category: implementation note
> Applies to: Luna 0.3.0 development
> Status: Implemented Experimental
> Normative status: non-normative
> 语言：中文权威版（英文版为占位）

# src/driver/ —— 目录逐文件指南

本指南合并了 src/driver/ 下全部源码文件的五段式导读，方便你一次读完整个目录。

---
title: driver/AotLinker.cpp
lang: zh-CN
source: src/driver/AotLinker.cpp
---

# src/driver/AotLinker.cpp

实现 AOT 链接：调用外部 C/C++ 编译器，把 `CodeGenerator` 产出的 LLVM IR 链接成可执行文件或共享库。

## 这个文件做什么

`AotLinker::build` 负责“最后一步”，流程如下：
1. 接收 Driver 已按 package target 选定的产物路径，并以同路径加 `.ll` 作为文本 IR 路径。
2. 调用 `codeGenerator.emitObjectFile(irPath)` 写出 `.ll` 文件；失败则打印错误并返回 1。
3. 确定运行时库：可用 `LUNA_RUNTIME_LIB` 环境变量或默认 `BUILD_DIR/libruntime.a`；不存在则报错。
4. 确定后端编译器：`LUNA_CXX` 环境变量或默认 `clang++`。
5. 依据 `optimizationLevel` 选 `-O0/-O2/-O3`。
6. 组装 `linkerArgs`（编译器 + 优化级 + 共享库选项 + IR + 运行时 + `-l` 库列表）。
7. 用 `llvm::sys::ExecuteAndWait` 执行外部编译器，检查退出码与错误。

## 关键结构体·枚举

- `AotArtifactKind`：区分 `Executable` / `SharedLibrary`（后者加 `-shared`/`-dynamiclib`）。



## 关键结构体·类·枚举

来源于 `AotLinker.h`：
- `enum class AotArtifactKind { Executable, SharedLibrary }`——决定链接产物类型；
- `struct AotLinkOptions`——包含 input/package 身份、链接库、runtime archive、
  compiler、完整输出路径、优化级别和 `AotArtifactKind`。

## 关键函数·方法

- `AotLinker::build(CodeGenerator&, AotLinkOptions)` —— 主入口（见上流程）。
- `printErrors(...)`：打印 `Diagnostic` 到 `stderr`。
- `quoteForDisplay(x)` —— 给含空格/引号的参数加转义（仅显示用）。
- `isLibraryPath(x)` —— 判断一个值是否是路径而非库名。

## 与周边文件·阶段关系

- **调用方**：`Driver.cpp` 的 `build`（Executable）与 `buildCffiLibrary`（SharedLibrary）。
- **依赖**：`codegen/CodeGenerator`、`diagnostics/Diagnostic`、LLVM `llvm/Support/Program.h`。

## 延伸阅读

- `AotLinker.h`、`Driver.cpp`、`CommandLine.cpp`（`--cc`/`--link`/`--runtime-lib`）。



---

---
title: driver/AotLinker.h
lang: zh-CN
source: src/driver/AotLinker.h
---

# src/driver/AotLinker.h

声明 Luna 的“提前编译”（AOT）链接阶段：把已生成的 LLVM IR 交给外部 C/C++ 编译器，链接成可执行文件或共享库。本文件只含契约（数据与接口声明），不含实现。

## 这个文件做什么

它是 `AotLinker` 的对外接口层。它回答“如何把一份 LLVM IR 变成最终程序”：需要哪些输入（源/包名/优化级/运行时库/编译器），以及产出什么（可执行文件还是共享库）。实现放在同名的 `.cpp` 里。

## 关键结构体·类·枚举

- `enum class AotArtifactKind { Executable, SharedLibrary }` —— 产物类型（可执行 / 共享库）。
- `struct AotLinkOptions` —— 一次 AOT 链接的完整参数。关键字段：
  - `inputPath`、`outputPath`：输入与输出路径。
  - `declaredPackageName`：已声明的包名。
  - `linkLibraries` (`std::vector<std::string>`)：额外链接库。
  - `runtimeLibrary`：Luna 运行时库 `libruntime` 路径。
  - `compiler`：后端 C/C++ 编译器（默认 `clang++`）。
  - `optimizationLevel`（`LunaOptimizationLevel`）：O0/O2/O3。
  - `artifactKind`（`AotArtifactKind`）：默认 `Executable`。
- `class AotLinker`：静态方法入口。

## 关键函数·方法

- `static int build(CodeGenerator&, AotLinkOptions)` —— 唯一公开接口，执行链接并返回退出码。

## 与周边文件·阶段关系

AOT 位于编译**最末阶段**（链接成可执行/共享库）。消费者是 `Driver.cpp` 的 `build` 与 `buildCffiLibrary`。依赖于 `codegen/CodeGenerator`（产出 IR）与 `diagnostics/Diagnostic.h`（报错）。

## 延伸阅读

- `AotLinker.cpp`（实现）。
- `Driver.cpp`（调用方）。
- `codegen/CodeGenerator.h`（IR 产出物）。



---

---
title: driver/CommandLine.cpp
lang: zh-CN
source: src/driver/CommandLine.cpp
---

# src/driver/CommandLine.cpp

实现命令行解析：把 `argc/argv` 翻译成 `CommandLineOptions`,并做组合合法性校验。

## 这个文件做什么

`parseCommandLine` 是手工写的中型参数解析器（无第三方库）。步骤：
1. 校验 `argv[1]` ∈ {`run`,`build`,`check`,`analyze`}，且至少再给一个输入文件。
2. 以 `argv[2]` 为输入文件，从 `argv[3]` 起循环解析其余选项。
3. 支持两种写法：`--opt value` 与 `--opt=value`。
4. `parseOptimizationLevel`：`-O0/-O2/-O3`（也接受无连字符写法）。
5. `parseGpuTargets`：`sim` / `cuda[:sm_*]` / `rocm[:gfx*]`，逗号分隔多目标。
6. 末尾做组合校验：例如 `--message-format=json` 仅限 `check`/`analyze`；`analyze` 强制 JSON；`--overlay` 仅 `analyze` 且不能与 `--overlays-from-stdin` 同用；`-t`/`-o` 仅 `build`；`-t moon` 不能带原生链接/GPU 选项。

## 关键结构体·类·枚举

本文件无自己的数据结构定义，全部复用于 `CommandLine.h`：
- `enum class MessageFormat { Human, Json }`；
- `enum class ArtifactTarget { Native, Moon, Cffi }`；
- `struct CommandLineOptions` 与 `CommandLineParseResult`，以及内部使用的 `LunaOptimizationLevel`/`LunaGpuTargetConfig`。

## 关键函数·方法

- `parseCommandLine(argc, argv)` —— 主入口。
- `parseOptimizationLevel(value, level)` —— 优化级字符串解析。
- `parseGpuTargets(spec, targets, error)` —— GPU 目标列表解析及相关约束。
- `failure(error, showUsage)` —— 构造失败结果。

## 与周边文件·阶段关系

驱动阶段的输入适配层，被 `Driver.cpp::run` 最先调用。

## 延伸阅读

- `CommandLine.h`、`Driver.cpp`、`codegen/CodeGenerator.h`。



---

---
title: driver/CommandLine.h
lang: zh-CN
source: src/driver/CommandLine.h
---

# src/driver/CommandLine.h

Luna 驱动（Driver）的命令行接口契约：定义如何把 `argv` 表示成结构化的 `CommandLineOptions`。

## 这个文件做什么

声明驱动层“选项模型”与解析入口。用户敲 `luna <command> <file> [flags]`，最终都要翻译成这里定义的结构，供 `Driver.cpp` 分发执行。本文件只声明，解析实现在 `CommandLine.cpp`。

## 关键结构体·类·枚举

- `enum class MessageFormat { Human, Json }` —— 错误/诊断输出格式。
- `enum class ArtifactTarget { Native, Moon, Cffi }` —— `build` 的目标产物。
- `struct CommandLineOptions` —— 一次调用的全部选项：
  - `command`（如 `run`/`build`/`check`/`analyze`）；
  - `inputPath`、`linkLibraries`、`runtimeLibrary`、`aotCompiler`、`outputPath`、`moonIrOutput`；
  - `overlayPath`、`overlaysFromStdin`（供 `analyze`）；
  - `gpuTargets`（`LunaGpuTargetConfig`）；
  - `messageFormat`、`artifactTarget`、`printMoonCostReport`、`reserveKernelRuntime`、`optimizationLevel`。
- `struct CommandLineParseResult` —— `{ optional<CommandLineOptions> options; string error; bool showUsage; }`。

## 关键函数·方法

- `CommandLineParseResult parseCommandLine(int argc, char* argv[])` —— 唯一的解析入口（声明）。

## 与周边文件·阶段关系

被 `Driver.cpp` 的 `run()` 在入口调用。解析出的 `optimizationLevel`、`gpuTargets` 等流向 `CompilerPipeline`；`artifactTarget`/`aotCompiler` 等着 `build` 分支使用。

## 延伸阅读

- `CommandLine.cpp`（解析实现）。
- `Driver.cpp`（消费方）。
- `codegen/CodeGenerator.h`（`LunaOptimizationLevel`/`LunaGpuTargetConfig`）。



---

---
title: driver/CompilerPipeline.cpp
lang: zh-CN
source: src/driver/CompilerPipeline.cpp
---

# src/driver/CompilerPipeline.cpp

实现 `CompilerPipeline`：把语义分析后的 `Program` 一路降成可执行、校验过的 canonical MoonIR，并可选继续到 LLVM codegen。

## 这个文件做什么

（1）`compileToMoonIR`/`compileSourceToMoonIR`：用 `AnalysisSnapshot::analyzePath/analyzeSource` 做分析，失败即返回（带 `errorStage`）。
（2）`lowerAnalyzedProgram`：核心流水线——
- `moon::LunaLowerer::lower`：Program+符号表 → `moon::Module`（失败阶段 `moon-lower`）；
- `moon::Verifier::verify`（`moon-verify`）；
- `moon::Sealer::sealFunctionBodies`：把函数体封存为 canonical CFG（`moon-seal`）；
- 再 verify；
- `moon::Optimizer::run`：按级别（None/Standard/Aggressive）+ AOT/JIT 目的（`moon-opt`）；
- 再 verify。
（3）`generateCode`：建 `CodeGenerator`、设优化级与 GPU target、调用 `generate(mMoonModule)`。
（4）`fail(errors, stage)`：记录错误加阶段并返回 `false`。

## 关键结构体·枚举

- `moon::OptimizationLevel`：`None`/`Standard`/`Aggressive`。
- `moon::OptimizationPurpose`：`AheadOfTime`/`JustInTime`。



## 关键结构体·类·枚举

本文件主要使用 `CompilerPipeline` 类（定义于 `CompilerPipeline.h`），其内部调用的 MoonIR 阶段类型：
- `moon::OptimizationLevel { O0, O1, O2, O3, Standard, Aggressive }`；
- `moon::OptimizationPurpose { AheadOfTime, JustInTime }`。

## 关键函数·方法

全部见头文件，本文件实现：`compileToMoonIR`、`compileSourceToMoonIR`、`lowerAnalyzedProgram`、`generateCode`、`fail`、`reset` 与各访问器。

## 与周边文件·阶段关系

调用 `moonir` 的 `Lowering`/`Verifier`/`Sealer`/`Optimizer`；上游是 `tooling/AnalysisSnapshot`；下游是 `codegen/CodeGenerator`。

## 延伸阅读

- `CompilerPipeline.h`、`moonir/` 目录、`codegen/CodeGenerator.h`。



---

---
title: driver/CompilerPipeline.h
lang: zh-CN
source: src/driver/CompilerPipeline.h
---

# src/driver/CompilerPipeline.h

定义一条“从源码（或内存源码）到 MoonIR / 代码生成”的核心编译流水线封装类 `CompilerPipeline`。

## 这个文件做什么

把多阶段流程（分析 → lowering → 校验 → seal → 优化 → codegen）封装成一个类。对外提供两段入口：先 `compileToMoonIR`/`compileSourceToMoonIR`，再 `generateCode`。持有中间产物（`moon::Module`、`CodeGenerator`、`AnalysisSnapshot`）与错误状态（`mErrors`/`mErrorStage`）。

## 关键结构体·类

- `struct CompilerPipelineOptions`：`inputPath`、`optimizationLevel`、`reserveKernelRuntime`、`aheadOfTime`。
- `class CompilerPipeline`：
  - `mOptimizationLevel`、`mModuleName`、`mDeclaredPackageName`；
  - `mAnalysisSnapshot`、`mMoonModule`、`mCodeGenerator`（`std::unique_ptr`）；
  - `mErrors`（`vector<Diagnostic>`）、`mErrorStage`。



## 关键结构体·类·枚举

- `class CompilerPipeline`——核心编译流水线，持有：
  - `mPackageName`（`std::string`）、`mModulePath`（`std::filesystem::path`）；
  - `mMoonModule`（`std::unique_ptr<moon::Module>`）、`mCodeGenerator`（`std::unique_ptr<CodeGenerator>`）；
  - `mErrors`（`std::vector<Diagnostic>`）、`mErrorStage`（`std::string`）。

## 关键函数·方法

- `compileToMoonIR(options)` —— 按路径分析并降低。
- `compileSourceToMoonIR(source, virtualPath, options)` —— 按内存源码分析。
- `generateCode(LunaGpuTargetConfig)` —— 触发 LLVM codegen。
- 查询：`moonModule()`/`codeGenerator()`/`declaredPackageName()`/`errors()`/`errorStage()`/`analysisSnapshot()`。
- 私有：`lowerAnalyzedProgram(...)`、`reset(...)`、`fail(...)`。

## 与周边文件·阶段关系

位于驱动层与 MoonIR/Codegen 层之间。被 `Driver.cpp`、`Repl.cpp` 使用；依赖 `tooling/AnalysisSnapshot`、`moonir/*`、`codegen/CodeGenerator`。

## 延伸阅读

- `CompilerPipeline.cpp`（实现）。
- `moonir/` 目录、`codegen/CodeGenerator.h`、`tooling/AnalysisSnapshot.h`。



---

---
title: driver/Driver.cpp
lang: zh-CN
source: src/driver/Driver.cpp
---

# src/driver/Driver.cpp

驱动核心：接受 `argv`，负责 `repl`、`--version`、`check`、`analyze`、`run`、`build`(native/Moon/CFFI) 的分发与编排。

## 这个文件做什么

`run` 是总调度器。先处理特殊命令（`repl`/`--version`），再 `parseCommandLine`；解析失败时按是否请求 JSON 输出不同协议流。随后构造 `CompilerPipeline`：`compileToMoonIR` →（可选写 MoonIR / 成本报告）→ `check` 提前返回 → `build` 分支 → `generateCode` → `run`(JIT) 或 AOT 链接。

还包括多个静态辅助子工具：
- `buildMoonContainer`：`-t moon`，编码 Moon 容器（`ContainerModelCodec`）并自校验。
- `buildCffiLibrary`/`collectCffiExports`：`-t cffi` 生成 C 头并链接共享库。
- JSON 协议输出：`printJsonHello/Diagnostics/Summary`、`printAnalysisHello/Symbol/Reference/Summary`。
- `analyze` 相关：`parseAnalysisOverlays`（stdin `luna.overlay` JSON）、`byteOffsetFromSource`、`analysisByteOffset`。
- `loadJITLibraries`：用 LLVM DynamicLibrary 加载 `--link` 库。
- `printUsage`：用法横幅。

## 关键结构体·枚举

- `SourceOverlays = std::vector<PackageRequest::SourceOverlay>`。
- `ArtifactTarget`/`MessageFormat`（来自 `CommandLine.h`）。



## 关键结构体·类·枚举

本文件使用的关键类型：
- `using SourceOverlays = std::vector<PackageRequest::SourceOverlay>`——用于 `analyze` 的源码覆盖层集合；
- `enum class ArtifactTarget { Native, Moon, Cffi }` 与 `enum class MessageFormat { Human, Json }`（来自 `CommandLine.h`）。

## 关键函数·方法

- `int run(int argc, char** argv)`：总入口。
- 静态辅助（见上清单）。

## 与周边文件·阶段关系

驱动层顶端，连接 `CommandLine`、`CompilerPipeline`、`AotLinker`、`Repl`、`tooling/AnalysisSnapshot`、`moonir/Printer`、`moonir/ContainerModel`、`runtime/Runtime`、`package/Package`、`diagnostics/Diagnostic` 与 LLVM ORC/Target。

## 延伸阅读

- `Driver.h`、`AotLinker.*`、`CompilerPipeline.*`、`CommandLine.*`、`Repl.*`、`moonir/ContainerModel.h`。



---

---
title: driver/Driver.h
lang: zh-CN
source: src/driver/Driver.h
---

# src/driver/Driver.h

Luna 驱动（Driver）的单一执行入口：`luna::driver::run(int argc, char* argv[])`。

## 这个文件做什么

极简的接口层，隐藏所有前端逻辑。实际实现（解析 → 分发到 `CompilerPipeline` → JIT/AOT/Repl）全在 `Driver.cpp`。

## 关键结构体·类·枚举

（仅一个命名空间与一个函数签名，无数据结构。）

## 关键函数·方法

- `int run(int argc, char* argv[])` —— 驱动主入口，返回进程退出码。

## 与周边文件·阶段关系

由可执行入口调用。内部依次调用 `parseCommandLine`、`CompilerPipeline`、`AotLinker`、`check`/`analyze` 等。

## 延伸阅读

- `Driver.cpp`、`CommandLine.h`、`CompilerPipeline.h`、`Repl.h`。



---

---
title: driver/Repl.cpp
lang: zh-CN
source: src/driver/Repl.cpp
---

# src/driver/Repl.cpp

实现交互式 REPL：逐行读取，支持求值表达式、记录声明、重置、退出等命令，并用 `CompilerPipeline` JIT 执行。

## 这个文件做什么

`runRepl` 主循环：
- `:help` 打印支持契约；
- `:quit`/`exit` 退出，`:reset` 清空累积声明；
- `:decl <decl>`：把候选声明追加到持久串 `declarations`，先编译校验，合法才保存；
- `= <expr>`：包进 `fn main() -> i32 { return expr; }` 求值并打印结果；
- 其它单行：`statementProgram()` 包成带 `return 0;` 的 `main` 执行，输出 `ok` 或 `exit <code>`。
“声明”持久化靠源码重编译（局部/堆/JIT 全局不保存）。

## 关键结构体·类·枚举

本文件未定义新的结构体或枚举，主要数据来自：`CompilerPipeline`（编译与 JIT 执行）、`Token`（词法结果）、`diagnostic::Diagnostic`（错误渲染）。累积声明的持久串是一个简单 `std::string declarations`。

## 关键函数·方法

- `runRepl(input, output, errors)` —— 主循环。
- 静态辅助：`printReplHelp`、`printPipelineErrors`、`compileAndRun(source,result,errors)`、`statementProgram(decls, stmt)`。

## 与周边文件·阶段关系

反复调用 `CompilerPipeline`（`compileSourceToMoonIR`+`generateCode`）与 `codeGenerator().jitRun()`。由 `Driver.cpp` 在 `repl` 时调用。

## 延伸阅读

- `Repl.h`、`Driver.cpp`、`CompilerPipeline.cpp`。



---

---
title: driver/Repl.h
lang: zh-CN
source: src/driver/Repl.h
---

# src/driver/Repl.h

Luna 交互式 REPL 的接口声明：`runRepl(input, output, errors)`。

## 这个文件做什么

极小的接口层，把 REPL 与具体 I/O 流解耦（便于用 `std::stringstream` 测试）。实现见 `Repl.cpp`。

## 关键结构体·类·枚举

（无数据结构，仅一个命名空间与一个函数。）

## 关键函数·方法

- `int runRepl(std::istream&, std::ostream&, std::ostream&)` —— REPL 主循环。

## 与周边文件·阶段关系

由 `Driver.cpp` 在 `repl` 命令时调用，绑定 `std::cin/out/err`。内部用 `CompilerPipeline` 做 JIT。

## 延伸阅读

- `Repl.cpp`、`Driver.cpp`、`CompilerPipeline.h`。



---
