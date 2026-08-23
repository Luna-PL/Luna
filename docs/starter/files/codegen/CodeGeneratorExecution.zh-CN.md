# src/codegen/CodeGeneratorExecution.cpp —— 构造函数/JIT 运行/对象文件输出

## 这个文件做什么

实现 `CodeGenerator` 的生命周期与执行体：构造函数（初始化 LLVM 上下文/模块/IRBuilder/CGHelpers 并初始化 LLVM 目标支持）、析构函数、`jitRun()`（用 ORC LLJIT 即时编译并执行生成的 `main`，返回其退出码）与 `emitObjectFile()`（把模块以文本 IR 形式写盘，供 AOT 使用）。文件还含一个匿名命名空间：`initializeLLVM()` 一次性初始化各目标后端；`invokeLunaJitEntry` 作为唯一 JIT 边界并针对 UBSan function-check 豁免；`lunaJitMingwMain()` 是 MinGW 用的空 `__main` 占位。

对 C++ 读者：`jitRun` 是全文件最重的方法——手动把 Luna 运行时的全部 `rt_*` 辅助符号显式绑定进 JIT 语义（而不是依赖 ELF -rdynamic / Mach-O 导出 / Windows dllexport），把 ThreadSafeModule 送进 LLJIT，再按 libc/用户库的进程符号回退，最后 `lookup("main")` 并调用。

## 关键结构体·类·枚举

匿名命名空间内：
- `void initializeLLVM()`——static 标志位防重入，一次性 InitializeNativeTarget(AsmPrinter/AsmParser) 与 InitializeAllTargets/MCs/AsmPrinters。
- `using LunaJitEntry = int (*)()` 与 `int invokeLunaJitEntry(LunaJitEntry entry)`——在 `#if defined(__clang__) LLVM_NO_SANITIZE("function")` 下包装调用，规避 ORC 生成函数缺 UBSan 元数据在页边界探测崩溃。
- `_WIN32` 下 `void lunaJitMingwMain(){}`——MinGW 对名为 main 的函数插 `__main` 调用时补的 no-op 符号。

## 关键函数·方法

**`CodeGenerator::CodeGenerator(const string& moduleName)` / `~CodeGenerator()=default`**
- 初始化 `mCtx`(make_unique<LLVMContext>)、`mModule`(moduleName)、`mBuilder`、`mHelpers`，随后 `initializeLLVM()`。

**`int CodeGenerator::jitRun()`**
- 建 LLJITBuilder() 创建 JIT。
- 逐条 `bindRuntime(name, &func)` 把运行期 helper 绑进 SymbolMap（Exported，mangleAndIntern），条件为该名字已在 module 中引用。覆盖：alloc/realloc/dealloc、RC/ARC(rt_rc_* / rt_arc_*)、panic、host_services、checked_array_layout、try_alloc/realloc、console I/O、file I/O、path metadata、runtime_error、raw malloc/free、print_i32/cstr、0.2 兼容符号、array_index_or_abort、动态片段/插件(rt_dynamic_fragment_* / rt_fragment_plugin_*)、GPU(rt_gpu_*)。Windows 下额外把 luaJitMingwMain 作为 `__main` 绑定。
- define 进 mainJITDylib(absoluteSymbols) 后，addIRModule(ThreadSafeModule(move(mModule),move(mCtx)))。
- 加 EPCDynamicLibrarySearchGenerator::GetForTargetProcess 生成器供 libc/用户库回退。
- lookup("main")、toPtr<int()>()、return invokeLunaJitEntry(mainFunction)。失败均打日志返回 1。
- 谁调用：上层 jitRun()；谁被调：rt_* 符号地址（来自 `../runtime/Runtime.h`）。

**`bool CodeGenerator::emitObjectFile(const string& outputPath)`**
- 设 triple 为 `getProcessTriple()`，`raw_fd_ostream` 打开输出，`mModule->print(dest)` 写文本 IR（规避 bitcode 兼容问题）。
- 谁调用：AOT 路径（上层编译管线）。

## 与周边文件·阶段的关系

- 属**执行/输出阶段**：generate 后要么 jitRun 即时执行，要么 emitObjectFile 落 AOT 物。
- 只 include `CodeGenerator.h` 与 `../runtime/Runtime.h`，不依赖其他 codegen 实现文件。
- 初始化 LLVM 目标支持，供 Gpu 文件中的 NVPTX/AMDGPU 后端使用。

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
