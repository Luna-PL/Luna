> Document category: implementation note / tutorial
> Applies to: Luna 0.3.0 development
> Status: Implemented Experimental
> Normative status: non-normative
> 语言：中文权威版（英文版为占位）

# 调试指南：观察、运行与验证 Luna（不改源码）

> 重要纪律：本仓库只读源码、只新增文档。本节教你在**不改源码**的前提下观察、运行、验证 Luna 的行为。

## 1. 构建与基础指令

仓库根目录已存在 build 目录。常规流程：
```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_C_COMPILER="$(command -v clang)" \
  -DCMAKE_CXX_COMPILER="$(command -v clang++)" \
  -DLLVM_DIR="$(llvm-config --cmakedir)"
cmake --build build -j4
```

跑全套测试：
```sh
ctest --test-dir build --output-on-failure
```

跑单个例子：
```sh
./build/luna check hello.luna
./build/luna run hello.luna -O2
./build/luna build hello.luna -O2
```

examples/*.luna 有很多可读示例（basic / closure / adt / ffi / trait_versioning 等），是观察整条流水线的廉价入口。

## 2. 观察 MoonIR 与定位阶段

编译失败时 Driver 会在诊断里带阶段名（如 moon-lower / moon-verify / moon-seal），帮你判断错在哪一段。MoonIR 的结构可用 src/moonir/Printer.cpp 打印；`check` 模式看语义是否通过，`run` 看执行结果。

## 3. 观察 LLVM IR

AOT 会生成文本 LLVM IR。可直接：
```sh
./build/luna build examples/basic.luna -O2
head examples/basic.luna.ll   # 示例自带 .ll
```

LLVM IR 使用带名字的值（%n / @func），对得上 codegen 里每条 CreateXxx 调用，正好验证手册讲的映射。

## 4. 用 gdb / lldb 断点

要在编译管线里断点：先用 Debug 构建（默认），然后：
```sh
gdb --args ./build/luna check examples/basic.luna
(gdb) break luna::driver::CompilerPipeline::lowerAnalyzedProgram
(gdb) run
(gdb) bt
```

实用切点顺序：AnalysisSnapshot::analyzePath -> lowerAnalyzedProgram -> moon::LunaLowerer::lower -> Verifier::verify -> Sealer::sealFunctionBodies -> CodeGenerator::generate -> jitRun / emitObjectFile。

## 5. 错误阶段对照表

| 看到的报错 | 大致阶段 | 提示 |
|---|---|---|
| error[ 语义 ] | Sema | 类型/所有权/借用问题 |
| error[moon-lower] | MoonIR lowering | 前端到 MoonIR 翻译 |
| error[moon-verify] | Verifier | IR 完整性被破坏 |
| error[moon-seal] | Sealer | 函数体密封失败 |
| error[codegen] | codegen | MoonIR→LLVM 映射 |
| 运行期崩溃 | JIT/AOT 运行时 | 看 backtrace |

## 6. 只读心态

本仓库纪律：**任何理由都不许改 src/ 源码**。调试全靠观察（build / run / print IR / gdb），而不是改源码打日志再还原。Doxygen 仅允许新增独立 Doxyfile，绝不写源码注释。

## 7. 继续阅读
- 回归测试规范：[docs/testing.md](../testing.md)
- 术语：[glossary.zh-CN.md](./glossary.zh-CN.md)
