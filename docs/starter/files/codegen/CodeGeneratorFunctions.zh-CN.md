# src/codegen/CodeGeneratorFunctions.cpp —— 函数体(含 main/内核初始化)的代码生成入口

## 这个文件做什么

实现 `CodeGenerator::generateFunctionBody(FunctionDecl*)`：为单个 Luna 函数生成 LLVM 函数体。它确定 `Function` 与返回类型、清空当前函数状态、为使用 kernel 的 `main` 注入 GPU 初始化，随后调用 `generateControlFlowBody` 并补齐 void 返回。application host service 改由 `CodeGeneratorModule.cpp` 在优化后根据仍存活的 input/filesystem 调用选择。

对 C++ 读者：这是「函数级入口适配器」——把声明层信息（参数/返回类型/是否 main/是否有内核）换算成一次 `generateControlFlowBody` 调用，并负责 prologue（入口初始化、状态复位）。真正的语句/控制流生成在 ControlFlow 文件里。

## 关键函数·方法

**`void CodeGenerator::generateFunctionBody(FunctionDecl* decl)`**
- 逻辑顺序：(1) 查找 LLVM Function；(2) 解析 retLLVMType；(3) 跳过 extern，并拒绝缺少规范 CFG 的定义；(4) 保存并重置当前函数状态；(5) 创建 entry；(6) 对使用 kernel 的 `main` 调用 rt_gpu_initialize，失败时通过 rt_gpu_report_initialization_error 返回合适失败值；(7) 调 generateControlFlowBody；(8) 补 void 返回并恢复状态。随后 `CodeGeneratorModule.cpp` 完成优化，只为仍存活的 input/filesystem/直接 host/GPU 使用注入 rt_install_application_host_services_v1。
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
3. runtime：`rt_gpu_initialize`/`rt_gpu_report_initialization_error`；条件化 host profile 注入位于 `CodeGeneratorModule.cpp`。

---

---
title: src/codegen/CodeGeneratorGpu.cpp
path: src/codegen/CodeGeneratorGpu.cpp
阶段: 代码生成 (CodeGen)——GPU 内核码物与启动
语言: C++
---
