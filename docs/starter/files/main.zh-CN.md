> Document category: implementation note
> Applies to: Luna 0.3.0 development
> Status: Implemented Experimental
> Normative status: non-normative
> 语言：中文权威版（英文版为占位）

# src/main/ —— 目录逐文件指南

本指南合并了 src/main/ 下全部源码文件的五段式导读，方便你一次读完整个目录。

---
title: src/main/main.cpp
source: src/main/main.cpp
language: zh-CN
audience: Luna 编译器贡献者 / C++ 开发者
---

# src/main/main.cpp

Luna 编译器的进程入口点，负责将命令行参数转发给驱动层。

## 这个文件做什么

`main.cpp` 是整个 Luna 编译器可执行文件的入口。它仅包含一个 `main` 函数，其全部职责是：

1. 接收操作系统传来的 `argc` / `argv` 命令行参数。
2. 调用 `luna::driver::run(argc, argv)` 并将返回值作为进程退出码返回。

该文件是编译器和宿主操作系统之间的最外层边界。它不包含任何业务逻辑、命令行解析、编译流水线调度或诊断输出——这些全部委托给 `luna::driver::run`。

## 关键结构体·类·枚举

本文件不定义任何结构体、类或枚举。所有类型定义均位于被引用的头文件 `driver/Driver.h` 中。

## 关键函数·方法

### `main(int argc, char* argv[]) -> int`

标准 C++ 入口点。签名固定为操作系统所要求的 `(int, char*[])` 形式。

- **参数**：
  - `argc` — 命令行参数个数（argument count）。
  - `argv` — 命令行参数字符串数组（argument vector）。
- **返回值**：`int`，作为进程退出码返回给操作系统。0 表示成功，非 0 表示错误。
- **逻辑**：直接委托给 `luna::driver::run(argc, argv)`。

### `luna::driver::run(int argc, char* argv[]) -> int`

定义于 `src/driver/Driver.h` 中的驱动层入口函数。其实现位于 `src/driver/Driver.cpp`，涵盖：

- 子命令分发（`check`、`run`、`build`、`analyze`、`repl`、`--version`）。
- 命令行解析（通过 `parseCommandLine`，定义于 `src/driver/CommandLine.h`）。
- 编译流水线编排（`CompilerPipeline::compileToMoonIR` → `generateCode`）。
- AOT 链接（通过 `AotLinker::build`，定义于 `src/driver/AotLinker.h`）。
- REPL 会话（`runRepl`，定义于 `src/driver/Repl.h`）。
- JSON 诊断协议输出（`--message-format=json`）。
- 分析快照（`AnalysisSnapshot`，定义于 `src/tooling/AnalysisSnapshot.h`）。
- Moon Container 构建（`buildMoonContainer`，静态函数）。
- CFFI 库构建（`buildCffiLibrary`，静态函数）。

## 与周边文件·阶段的关系

`main.cpp` 处于调用链的最顶层：

```
main.cpp (main) --> src/driver/Driver.cpp (luna::driver::run)
                      |
                      +---> src/driver/CommandLine.h (parseCommandLine)
                      +---> src/driver/CompilerPipeline.h (compileToMoonIR, generateCode)
                      +---> src/driver/AotLinker.h (AotLinker::build)
                      +---> src/driver/Repl.h (runRepl)
                      +---> src/tooling/AnalysisSnapshot.h (analyzePath)
```

- **上游**：操作系统。`main` 的 `argc`/`argv` 来自进程启动时的 shell 或加载器。
- **下游**：`src/driver/Driver.cpp` 中的 `luna::driver::run`。该函数是整个编译器的实际调度中枢。
- **同级**：`src/driver/CommandLine.h` 定义 `parseCommandLine` 和命令行选项结构体 `CommandLineOptions`；`src/driver/CompilerPipeline.h` 定义编译流水线类。
- **构建阶段**：`main.cpp` 在链接阶段被链接器作为入口点目标文件。Luna 的构建系统（CMake）会将此文件与 `driver/Driver.cpp` 及其他模块的目标文件链接成最终可执行文件。

## 延伸阅读

- [LLVM 编程入门：命令行参数与入口点设计](https://llvm.org/docs/ProgrammersManual.html)
- 相同目录下的驱动层实现：`src/driver/Driver.cpp`
- 命令行选项解析：`src/driver/CommandLine.h`
- 编译器版本宏：`src/Version.h`


---
