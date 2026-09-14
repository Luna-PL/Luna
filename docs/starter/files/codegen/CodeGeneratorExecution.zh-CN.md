# src/codegen/CodeGeneratorExecution.cpp —— 构造函数/JIT 运行/对象文件输出

## 这个文件做什么

实现 `CodeGenerator` 的生命周期与执行体：构造、JIT 执行、可检查文本 IR 输出，以及 AOT 所需的直接 native-object 输出。native target 正常初始化；NVPTX/AMDGPU registry 只在请求设备产物时初始化。

对 C++ 读者：`jitRun` 是全文件最重的方法——手动把 Luna 运行时的全部 `rt_*` 辅助符号显式绑定进 JIT 语义（而不是依赖 ELF -rdynamic / Mach-O 导出 / Windows dllexport），把 ThreadSafeModule 送进 LLJIT，再按 libc/用户库的进程符号回退，最后 `lookup("main")` 并调用。

## 关键结构体·类·枚举

匿名命名空间内：
- `initializeLunaLLVMTargets()` 一次性初始化 native target；独立的 device initializer 只在请求 PTX/HSACO 时注册所有目标。
- `using LunaJitEntry = int (*)()` 与 `int invokeLunaJitEntry(LunaJitEntry entry)`——在 `#if defined(__clang__) LLVM_NO_SANITIZE("function")` 下包装调用，规避 ORC 生成函数缺 UBSan 元数据在页边界探测崩溃。
- `_WIN32` 下 `void lunaJitMingwMain(){}`——MinGW 对名为 main 的函数插 `__main` 调用时补的 no-op 符号。

## 关键函数·方法

**`CodeGenerator::CodeGenerator(const string& moduleName)` / `~CodeGenerator()=default`**
- 初始化 `mCtx`(make_unique<LLVMContext>)、`mModule`(moduleName)、`mBuilder`、`mHelpers`，随后 `initializeLLVM()`。

**`LunaJitRunResult CodeGenerator::jitRun()`**
- 建 LLJITBuilder() 创建 JIT。
- 逐条 `bindRuntime(name, &func)` 把运行期 helper 绑进 SymbolMap（Exported，mangleAndIntern），条件为该名字已在 module 中引用。覆盖 allocation、RC/ARC 库存储、panic、host service、可恢复分配、console/file I/O、runtime error、数组边界与 GPU helper。Windows 下额外把 `lunaJitMingwMain` 作为 `__main` 绑定。
- define 进 mainJITDylib(absoluteSymbols) 后，addIRModule(ThreadSafeModule(move(mModule),move(mCtx)))。
- 加 EPCDynamicLibrarySearchGenerator::GetForTargetProcess 生成器供 libc/用户库回退。
- lookup("main")、toPtr<int()>()、return invokeLunaJitEntry(mainFunction)。失败均打日志返回 1。
- 谁调用：上层 jitRun()；谁被调：rt_* 符号地址（来自 `../runtime/Runtime.h`）。

**`bool CodeGenerator::emitObjectFile(const string& outputPath)`**
- 设 triple 为 `getProcessTriple()`，`raw_fd_ostream` 打开输出，`mModule->print(dest)` 写文本 IR（规避 bitcode 兼容问题）。
- 谁调用：AOT 路径（上层编译管线）。

**`bool CodeGenerator::emitNativeObjectFile(const string& outputPath)`**
- O2/O3 复用优化阶段已配置的 PIC host TargetMachine（O0 则只创建一次），再运行 LLVM object emission pass；外层 AOT driver 把 `.o` 交给平台 linker。识别到 x86-64 MinGW/Clang executable 工具链时直接调用配套 `ld.lld`，其他布局与 shared library 保留所选 clang driver。

## 与周边文件·阶段的关系

- 属**执行/输出阶段**：generate 后要么 jitRun 即时执行，要么两个 emit 方法保留 `.ll` 并生成 native linker 输入。
- 初始化 native LLVM target，并为 GPU 实现按需启用 device registry。

## 延伸阅读

1. `../runtime/Runtime.h`——rt_* 符号清单。
2. LLVM ORC/LLJIT 文档——lookup/绑定/搜索生成器。
3. `CodeGeneratorGpu.cpp`——GPU 调用由本文件绑定的 rt_gpu_* 支撑。

---

---
title: src/codegen/CodeGeneratorExpressions.cpp
path: src/codegen/CodeGeneratorExpressions.cpp
阶段: 代码生成 (CodeGen)——所有表达式节点的 LLVM 生成
语言: C++
---
