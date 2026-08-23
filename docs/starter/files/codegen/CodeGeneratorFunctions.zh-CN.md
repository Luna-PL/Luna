# src/codegen/CodeGeneratorFunctions.cpp —— 函数体(含 main/内核初始化)的代码生成入口

## 这个文件做什么

实现 `CodeGenerator::generateFunctionBody(FunctionDecl*)`：为单个 Luna 函数生成函数体的 LLVM 代码。它负责四件事：确定该函数的 LLVM `Function` 与返回类型；清空「当前函数相关」的成员状态（局部表、规范局部、数组 drop 标志、物化迭代器、已知上界）；若是 `main` 则注入宿主应用服务初始化 `rt_install_application_host_services_v1`，并在存在内核时注入 GPU 初始化 `rt_gpu_initialize`（失败则调 `rt_gpu_report_initialization_error` 返回错误码）；随后调 `generateControlFlowBody` 生成规范 CFG 主体，最后补 void 返回。

对 C++ 读者：这是「函数级入口适配器」——把声明层信息（参数/返回类型/是否 main/是否有内核）换算成一次 `generateControlFlowBody` 调用，并负责 prologue（入口初始化、状态复位）。真正的语句/控制流生成在 ControlFlow 文件里。

## 关键函数·方法

**`void CodeGenerator::generateFunctionBody(FunctionDecl* decl)`**
- 逻辑顺序：(1) 以 generatedSymbolName 或 name 查 mFunctions/mModule 拿 LLVM Function，拿不到则返回；(2) 解析返回类型为 retLLVMType；(3) decl->isExtern 直接返回；若 decl->controlFlow 为空报错「without exclusive canonical CFG body」；(4) 设置 mCurrentFunc 与 mCurrentFunctionIsKernel（保存/恢复旧值），并清空 mLocals/mLocalTypes/mCanonicalLocals/mCanonicalLocalTypes/mArrayDropFlags/mMaterializedIterators/mLocalKnownUpperBounds；(5) 创建 entry 基本块并设置插入点；(6) 若是 main：插入对 rt_install_application_host_services_v1 的调用（JIT 与 AOT 共用入口策略）；(7) 若 name==main 且 mProgram->features.kernel：调 rt_gpu_initialize，按结果分支到 readyBB / failedBB，failedBB 调 rt_gpu_report_initialization_error 并按返回类型返回 1/空值/void（避免 AOT 在后端配置错误时空指针崩溃）；(8) generateControlFlowBody(*decl->controlFlow, func, entryBB)；(9) 若 void 返回且无 terminator 补 CreateRetVoid；恢复 mCurrentFunc/kernel 标志。
- 谁调用：`CodeGeneratorModule.cpp` 的 `generateBodies`（对所有非 selector、非模板形参、codegen reachable 的函数/impl 方法，分内核/宿主两遍）。谁被调：`generateControlFlowBody` 及下层各类 generate 方法。

## 与周边文件·阶段的关系

- 属**代码生成阶段**：函数级代码生成的统一入口。
- 上游：`CodeGeneratorModule.cpp`（遍历 declarations 并调用，先内核后宿主）。
- 下游：`CodeGeneratorControlFlow.cpp`（生成主体）；特殊 main 逻辑引用 runtime 的 rt_* 符号。
- 与 `CodeGeneratorGpu.cpp` 配合：GPU 初始化下行由 rt_gpu_* 运行时函数完成，mProgram->features.kernel 决定是否插入。
- 依赖 `CodeGenerator.h` 的成员状态与 `resolveType` 等工具。

## 延伸阅读

1. `CodeGeneratorModule.cpp`——声明/函数入口表与两遍调度。
2. `CodeGeneratorControlFlow.cpp`——规范 CFG 主体如何被本函数调用。
3. runtime：`rt_install_application_host_services_v1`/`rt_gpu_initialize`/`rt_gpu_report_initialization_error`。

---

---
title: src/codegen/CodeGeneratorGpu.cpp
path: src/codegen/CodeGeneratorGpu.cpp
阶段: 代码生成 (CodeGen)——GPU 内核码物与启动
语言: C++
---
