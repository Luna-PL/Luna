# src/codegen/CodeGeneratorModule.cpp —— generate() 主流程

## 这个文件做什么

实现 `CodeGenerator::generate(moon::Module*)`——代码生成阶段的**总入口/编排者**。按序完成：初始化 `mProgram` 与 `mTypeMaterializer` 并清空各映射；对 module 内所有函数与 impl 方法「声明」（declareFunc）建立 LLVM `Function` 壳（含 ABI 可见性/链接、模板实例与 selector 过滤、Never 返回加 NoReturn 属性）；发射运行时描述符 `emitRuntimeDescriptors`；Pass A 先生成内核函数体（保证设备码对象先于宿主发射存在）；按需对内核做 PTX/HSACO 发射；Pass B 最后生成宿主函数体；校验宿主模块，并在非 O0 时走 PassBuilder O2/O3 优化管线后再次校验；最终以 `mErrors` 是否为空判定成败。

## 关键结构体·类·枚举

本文件无新增类型；核心是两个局部 lambda：`declareFunc` 与 `generateBodies(bool kernels)`。使用的类型：`moon::FunctionDecl`、`moon::ImplDecl`（MoonIR），以及 LLVM 的 PassBuilder 与分析管理器。

## 关键函数·方法

**`bool CodeGenerator::generate(moon::Module* program)`**
- 初始化：mProgram=program；mTypeMaterializer=new TypeMaterializer(*program)；清空 mFunctions/mDropCallbacks/mKernelPTX/mKernelHSACO。
- `declareFunc`：跳过 selector；跳过不可达内核；跳过「带类型形参却非模板实例」。用 resolveType 求参数/返回 LLVM 类型构造 FunctionType 并 Function::Create。可见性：`!program->isPackage || f->isExported || f->isExtern || f->name==main` → ExternalLinkage，否则 InternalLinkage。符号名优先 linkName，其次 generatedSymbolName / name。Never 返回加 Attribute::NoReturn。写入 mFunctions（含名字别名）。
- `generateBodies(kernels)`：遍历 declarations，其中 FunctionDecl 与 ImplDecl.methods 都参与，过滤（非 selector、isKernel==kernels、codegen reachable、模板实例化或无形参）后调 generateFunctionBody。
- Pass1：为所有函数/方法 declareFunc（解决前向引用）。
- emitRuntimeDescriptors()（见 RuntimeDescriptors.cpp）。
- Pass2（内核）：generateBodies(true)；若 mGpuTargets.emitPTX 对每个 reachable 内核调 emitKernelPTX（失败返回 false）；emitHSACO 同理。
- Pass3（宿主）：generateBodies(false)——宿主侧先嵌入已产出的 PTX/HSACO，避免 AOT 嵌入临时空设备模块。
- 校验：verifyHostModule(suffix) 用 llvm::verifyModule(mModule,&stream) 写错误到诊断；mErrors 空但校验失败则返回 false。
- 优化：mErrors 空且 mOptimizationLevel != O0 时注册分析管理器、构造 PassBuilder，对 O2（O2/O3）用 buildPerModuleDefaultPipeline 跑 module 优化，再 verifyHostModule(" after optimization")。
- 返回 mErrors.empty()。
- 谁调用：上层编译管线（语义分析后以 module 调 generate 作为后端第一入口）。谁被调：declareFunc、emitRuntimeDescriptors、generateFunctionBody、emitKernelPTX/emitKernelHSACO、verifyModule 与现代 PassBuilder。

## 与周边文件·阶段的关系

- 属**代码生成阶段**的主控器。
- 上游：语义分析之后调用 generate();接着可进 JIT(jitRun) 或 AOT(emitObjectFile)。
- 下游：CodeGeneratorFunctions.cpp(函数体)、CodeGeneratorRuntimeDescriptors.cpp(描述符)、CodeGeneratorGpu.cpp(内核码物)、CodeGeneratorControlFlow.cpp/Expressions(体内部)。

## 延伸阅读

1. `CodeGenerator.h`——optimization level 与 GPU target 配置。
2. `CodeGeneratorRuntimeDescriptors.cpp`——emitRuntimeDescriptors 实现。
3. LLVM Pass: `llvm::PassBuilder` 与 buildPerModuleDefaultPipeline。

---

---
title: src/codegen/CodeGeneratorRangeAnalysis.cpp
path: src/codegen/CodeGeneratorRangeAnalysis.cpp
阶段: 代码生成 (CodeGen)
角色: 数组索引界限分析的实现
语言: C++
---
