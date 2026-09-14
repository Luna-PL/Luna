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

实现 AOT 链接：保留 `CodeGenerator` 的文本 LLVM IR，同时把其 native object 交给平台链接器生成可执行文件或共享库。

## 这个文件做什么

`AotLinker::build` 负责“最后一步”，流程如下：
1. 接收 Driver 已按 package target 选定的产物路径，并以同路径加 `.ll` 作为文本 IR 路径。
2. 调用 `codeGenerator.emitObjectFile(irPath)` 写出 `.ll`，再由 `emitNativeObjectFile(objectPath)` 生成 `.o`；失败则打印错误并返回 1。
   新目标以同目录临时文件 rename 提交，避免首次构建重复复制；已存在目标仍先做内容比较并安全覆盖。
3. 确定运行时库：可用 `LUNA_RUNTIME_LIB` 环境变量或默认 `BUILD_DIR/libruntime.a`；不存在则报错。
4. 确定后端编译器：`LUNA_CXX` 环境变量或默认 `clang++`。
5. 依据 `optimizationLevel` 选 `-O0/-O2/-O3`。
6. 组装 `linkerArgs`（编译器 + 优化级 + 共享库选项 + native object + Runtime + `-l` 库列表）。
7. 常规路径用 `llvm::sys::ExecuteAndWait` 执行外部编译器；识别到兼容 x86-64
   MinGW/Clang executable 布局时直接启动配套 `ld.lld`，把 stdout 产物写入 pending 文件并
   同步计算 SHA-256。两条路径都检查退出码；只有成功产物才会发布。

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

`run` 是总调度器。先处理 `--version`，再由 `parseCommandLine` 统一解析普通命令、`repl` 与内部 REPL worker；解析失败时按是否请求 JSON 输出不同协议流。随后构造 `CompilerPipeline`：`compileToMoonIR` →（可选写 MoonIR / 成本报告）→ `check` 提前返回 → `build` 分支 → `generateCode` → `run`(JIT) 或 AOT 链接。

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

实现交互式 REPL 的用户侧状态机：管理声明 cell，支持求值、类型查询、显式多行输入、撤销、重置和退出。进程与编译职责已经迁出本文件。

## 这个文件做什么

`ReplSession::run` 主循环：
- `:help` 打印支持契约；
- `:quit`/`exit` 退出，`:reset` 清空声明 cell，`:undo` 撤销最后一个 cell；
- `:decl <decl>`：候选声明连同合成 `main` 完成前端与 codegen 事务校验后才保存；
- `:type <expr>`：只分析表达式并读取前端记录的推导类型；
- `:paste decl|expr|stmt`：读取到 `:end` 为止的显式多行 cell；
- `:load <path>`：把文件内容作为一个声明 cell 提交，并保留真实诊断路径；
- `= <expr>`：包进 `fn main() -> i32 { return expr; }` 求值并打印结果；
- 其它单行：`statementProgram()` 包成带 `return 0;` 的 `main` 执行，输出 `ok` 或 `exit <code>`。
“声明”持久化靠源码重编译（局部/堆/JIT 全局不保存）。
声明校验、类型查询、表达式和语句均由 `ReplExecution.cpp` 提交给新 worker；
`ReplWorker.cpp` 在隔离进程中完成前端、codegen 与 JIT。
主会话等待输入时在隔离门后维持一个一次性预热 worker；cell 到达后才通过有界、
长度定界的数据通道提交操作、虚拟路径和源码。worker 只消费一个请求就退出，
ready、隔离门和完成通知使用原生 Windows Event 或继承的 POSIX socketpair；执行完成后
显式刷新输出并发布独立完成信号。最多两个 worker 进入后台回收，每个从
发布起获得 500 ms 正常清理宽限，之后强制终止进程树。主会话异步补充下一个，
因此启动和清理工作可与交互思考时间重叠，但 JIT 与链接库状态仍
不会跨 cell。空闲 worker 监视父进程并在会话消失时退出。
主会话通过执行期间持续排空的两阶段结果通道取得完整 `i32` 结果，并从共享预算的
stdout/stderr 通道取得输出；worker 崩溃、超时、内存或
输出越限只取消当前 cell，后代进程随 worker 一并清理。内部结果协议还回传 lexer、
parser、semantic/traits/ownership/indexing、MoonIR 各阶段、LLVM codegen、JIT
materialization/lookup/cleanup 和入口执行耗时；`--timings` 打开时由主会话补充缓存、
预热命中以及包含剩余启动、隔离、IPC、结果收集和补充调度的端到端提交耗时。Windows 只使用 REPL 自己的进程树 Job Object
实施内存上限和 kill-on-close，不再叠加 LLVM 的单进程内存 Job。

## 关键结构体·类·枚举

`ReplOptions` 保存优化级别、prompt、计时开关、worker 路径、链接库和
时间/内存/输出策略；`ReplSession` 保存有序声明 cell、增量声明源码、累计字节数、
单调 cell 编号和有界 `:type` 结果缓存。
`PreparedWorker`、`WorkerPreparer` 与 `WorkerReaper` 的实现位于 `ReplExecution.cpp`。

## 关键函数·方法

- `runRepl(input, output, errors, options)` —— 构造会话并进入主循环。
- `ReplSession::{storeDeclaration,evaluateExpression,executeStatement,inspectExpressionType,compileAndRun}` —— cell 事务与用户侧执行边界。
- `ReplSession::runInWorker`（`ReplExecution.cpp`）—— 调度待用 worker、限制资源并收集结果。
- `runReplWorker`（`ReplWorker.cpp`）—— 等待隔离门、读取一个长度定界请求并完成单次编译/JIT/执行。
- 静态辅助：`printReplHelp`、`statementProgram`。

## 与周边文件·阶段关系

依赖方向为 `Repl.cpp` → `ReplExecution.cpp` → `ReplTiming`/`ReplTransport`/`ReplProcess`/`ReplProtocol`；
`ReplWorker.cpp` 单独依赖 `CompilerPipeline` 完成编译和执行。`Driver.cpp` 只负责公开
`repl` 命令与隐藏 worker 命令的分派。

## 延伸阅读

- `Repl.h`、`ReplExecution.cpp`、`ReplTiming.cpp`、`ReplWorker.cpp`、`ReplProtocol.h`、`Driver.cpp`。



---

---
title: driver/Repl.h
lang: zh-CN
source: src/driver/Repl.h
---

# src/driver/Repl.h

Luna 交互式 REPL 的接口声明与会话状态模型。

## 这个文件做什么

把 REPL 与具体 I/O 流解耦，并公开优化级别、prompt、worker/超时配置和声明 cell 会话对象。交互实现见 `Repl.cpp`，执行与 worker 实现分别见 `ReplExecution.cpp` 和 `ReplWorker.cpp`。

## 关键结构体·类·枚举

- `ReplOptions`：优化级别、prompt、计时开关、worker 可执行文件、链接库与资源限制；
- `ReplSession`：有序声明 cell、容量记账、cell 编号和提交/查询/执行操作。

## 关键函数·方法

- `int runRepl(..., ReplOptions)` —— 构造一次会话并运行；
- `runReplWorker(...)` —— 运行一个内部隔离 worker；
- `printReplUsage(std::ostream&)` —— CLI 选项和交互命令帮助。

## 与周边文件·阶段关系

由 `Driver.cpp` 在 `repl` 命令时调用，绑定 `std::cin/out/err`；同一驱动的隐藏
worker 命令负责有超时边界的 `CompilerPipeline` JIT。

## 延伸阅读

- `Repl.cpp`、`ReplExecution.cpp`、`ReplWorker.cpp`、`Driver.cpp`、`CompilerPipeline.h`。



---
