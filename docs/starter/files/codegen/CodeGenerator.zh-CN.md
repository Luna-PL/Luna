# src/codegen/CodeGenerator.cpp —— CodeGenerator 的通用辅助方法实现

## 这个文件做什么

本文件集中实现 `CodeGenerator` 类中偏「通用/公共服务」的一组方法，被其他 codegen 实现文件反复调用：实参类型转换 `coerceCallArgument`、类型解析 `resolveType`、声明/函数解析 `resolveDeclaration`/`resolveFunction`、表达式分配类型推断 `allocationTypeForExpr`、入口块 alloca `createEntryBlockAlloca`、错误收集 `error`、字段下标 `fieldIndex`。它本身不实现某个具体 AST 节点或阶段，而是工具层。

对 C++ 读者：相当于把一组跨模块公用的 private 帮手集中到一个 .cpp——避免重复定义，也让下游只依赖声明、无需关心实现细节。

## 关键函数·方法

**`llvm::Value* CodeGenerator::coerceCallArgument(llvm::Value* value, llvm::Type* target)`**
- 把已求值参数规范化到目标 LLVM 类型：类型相同直接返回；整型对整型走 `CreateIntCast`（带符号，命名 abiarg）；指针对指针走 `CreateBitCast`；其余原样返回。
- 谁调用：几乎所有喂给 CreateCall/CreateStore/入口参数的地方；谁被调：LLVM IRBuilder 基础指令。

**`TypePtr CodeGenerator::resolveType(const moon::TypeRef& reference)`**
- 借助 `mTypeMaterializer` 把类型引用物化成 TypePtr；无 materializer 返回空。
- 谁调用：全部依赖类型信息的 codegen 文件；谁被调：`moon::TypeMaterializer::materialize`。

**`resolveDeclaration` / `resolveFunction`**
- `resolveDeclaration` 在 `mProgram->findDeclaration` 查声明记录；`resolveFunction` 先由此拿到记录，再在 `mFunctions` 哈希表查生成的 LLVM 函数，否则回退 `mModule->getFunction(linkageName)`。固定 ABI 名称解析的唯一权威。
- 谁调用：Expressions 的函数调用、Iterator 的 collect 协议、ControlFlow 解析等。

**`allocationTypeForExpr` / `createEntryBlockAlloca`**
- `allocationTypeForExpr` 沿 Move/Borrow/Identifier 解包取分配类型（Move/Borrow 递归取 operand，Identifier 查 mLocalTypes，否则回退 resolveType(expr->type)）。
- `createEntryBlockAlloca` 用临时 builder 在函数 entry block 顶部插入 alloca 并返回——一切局部存储槽的统一入口。
- 谁调用：ControlFlow/Expressions/Iterator/Gpu 等。

**`error`** 与 **`fieldIndex`**
- `error(msg)` 向 `mErrors` 追加 codegen 诊断（diagnostic::format），附带说明“通常由先前无效声明或不支持结构导致”。
- `fieldIndex` 返回字段名在 `type->fields` 的下标；不存在返回 `(size_t)-1`。被 field access/assign、record literal 等使用。

## 与周边文件·阶段的关系

- 属**代码生成阶段**的共享工具实现，被几乎所有其他 CodeGenerator* 文件调用。
- 依赖 `../diagnostics/Diagnostic.h` 与 LLVM IR。对应 `CodeGenerator.h` 中的声明。

## 延伸阅读

1. `CodeGenerator.h`——工具方法签名与数据结构。
2. `CodeGeneratorModule.cpp`——generate 主流程。
3. `../moonir/MoonIR.h`——TypeRef/DeclarationRef/TypeMaterializer 语义。

---

---
title: src/codegen/CodeGenerator.h
path: src/codegen/CodeGenerator.h
阶段: 代码生成 (CodeGen)——核心类声明
语言: C++
---

# src/codegen/CodeGenerator.h —— 代码生成器的核心类声明与全部内部数据结构

## 这个文件做什么

`CodeGenerator.h` 是 Luna 后端的**核心头文件**。它声明了：
- 枚举 `LunaOptimizationLevel`（O0/O2/O3），控制 LLVM 模块级优化管线。
- 结构体 `LunaGpuTargetConfig`，配置 CUDA (sm_52) 与 ROCm (gfx1101) 的内核码物目标。
- 四个与迭代器代码生成相关的内部数据结构：`IteratorStep`、`IteratorPlan`、`RuntimeIteratorStep`、`MaterializedIterator`。
- 类 `CodeGenerator` 的全部公开接口与私有成员/方法，包括所有 `generate` 类方法、成员变量（LLVM 上下文、IRBuilder、CGHelpers 助手、各种映射表与状态）、以及从 `generateExpr` 分派出的各具体表达式生成器。

对 C++ 读者：这个文件相当于整个 codegen 子系统的「API 边界 + 实现状态定义」。它列出了所有 `generate*` 方法（每个 Luna 表达式/语句/控制流都有对应的 LLVM 生成方法），以及 `mLocals`、`mLocalTypes`、`mCanonicalLocals`、`mFunctions`、`mMaterializedIterators`、`mKernelPTX`/`mKernelHSACO` 等大量中间状态。

## 关键结构体·类·枚举

**`enum class LunaOptimizationLevel`**：O0（默认）、O2、O3。

**`struct LunaGpuTargetConfig`**：
- `bool emitPTX`：是否对内核生成 CUDA PTX。
- `std::string cudaArchitecture`：默认 `sm_52`。
- `bool emitHSACO`：是否对内核生成 ROCm HSACO。
- `std::string rocmArchitecture`：默认 `gfx1101`。

**`struct IteratorStep` / `IteratorPlan` / `RuntimeIteratorStep` / `MaterializedIterator`**（私有，用于迭代器 d 展开）：
- `IteratorStep`：`{op, argument, inputType, outputType}` 描述一个适配器操作（Map/Filter/Take）。
- `IteratorPlan`：`{source, sourceType, itemType, mode, rangeStart, rangeEnd, ownedStateName, materializedName, steps}` 描述一个完整的迭代器（从源到适配器链）。
- `RuntimeIteratorStep`：`{description, value, remaining}` 运行期步的已求值参数与 Take 剩余计数器。
- `MaterializedIterator`：`{plan, sourceData, limit, indexStorage, sourceDropFlags, ownsSource, steps}` 迭代器物化到 LLVM 状态后的全部 IR 值。

**`class CodeGenerator`**（public）：
- 公开方法：`CodeGenerator(moduleName)`、`~CodeGenerator()`、`generate(Module*)`、`setOptimizationLevel`、`setGpuTargets`、`jitRun()`、`emitObjectFile()`、`errors()` const。
- 私有方法（~70 个，见头文件 52-176 行）：按功能分组：`generateFunctionBody`、`generateControlFlowBody`、`generateExpr`（总入口）；各类字面量/访问/算术/构造/调用/控制流/所有权/闭包/迭代器/GPU 的 `generate*` 方法；`emitRuntimeDescriptors`、`emitKernelPTX/HSACO`、`emitCleanup` 系列等。
- 私有成员变量（177-218 行）：`mCtx`(LLVMContext)、`mModule`、`mBuilder`、`mHelpers`(CGHelpers)；`mProgram`(moon::Module*)；`mTypeMaterializer`；`mLocals`/`mLocalTypes`(name->alloca/type 映射)；`mCanonicalLocals`/`mCanonicalLocalTypes`(LocalId->alloca/type)；`mArrayDropFlags`(name->drop bit)；`mMaterializedIterators`；`mLocalKnownUpperBounds`(name->exclusive upper bound)；`mCurrentFunc`/`mCurrentFunctionIsKernel`；`mFunctions`/`mDropCallbacks`；`mKernelPTX`/`mKernelHSACO`；`mErrors`；`mOptimizationLevel`；`mGpuTargets`。

## 关键函数·方法

所有 `generate*` 方法签名与作用已在头文件逐行注释。简览：
- `generateExpr(Expr*)`：表达式生成的总调度器，对各 AST 子类做 dynamic_cast 分派到对应 `generateXxx`。
- `generateIntLiteral/FloatLiteral/StringLiteral/BoolLiteral/UnitLiteral/ArrayLiteral`：字面量。
- `generateIdentifier/DynamicSelect/FieldAccess/SliceLength/Index`：访问。
- `generateBinary/Unary`：算术。
- `generateVariantConstruct/ResultConstruct/RecordLiteral/InitAllocation/HeapAlloc`：构造。
- `generateCall/generateLaunch`：调用/GPU 启动。
- `generateTry/Assign/Move/Borrow/Deref/AddrOf/Lambda/EnvLoad/MakeClosure`：控制流/所有权/闭包。
- `buildIteratorPlan/materializeIteratorBinding/emitIteratorPipeline/generateIteratorTerminal/emitCallableInvocation`：迭代器。
- `emitKernelPTX/emitKernelHSACO/generateDeviceBufferPointer/generateHostRawPointer/emitGpuOperationFailureCheck`：GPU。
- `emitRuntimeDescriptors/emitLunaDeallocation/packResultPayload/unpackResultPayload/emitResourceContentsCleanup/emitOwnedPayloadCleanup/emitMaterializedIteratorCleanup/emitCleanup/emitCanonicalCleanup/getOrCreateDropCallback`：清理/运行时元数据。
- 工具方法：`coerceCallArgument/resolveType/resolveDeclaration/resolveFunction/allocationTypeForExpr/createEntryBlockAlloca/fieldIndex/error`。

## 与周边文件·阶段的关系

- 属**代码生成阶段**的核心头文件——所有 `CodeGenerator*.cpp` 实现文件都以 `#include "CodeGenerator.h"` 开头，是整个后端的 ABI 边界。
- 依赖：`../moonir/MoonIR.h`（moon::Module, Expr, FunctionDecl 等）、`CGHelpers.h`、LLVM 各 IR 头文件、`../diagnostics/Diagnostic.h`。
- 被实现文件：CodeGenerator.cpp、Module.cpp、Functions.cpp、ControlFlow.cpp、Expressions.cpp、Cleanup.cpp、Iterator.cpp、Gpu.cpp、Execution.cpp、RuntimeDescriptors.cpp 全部 include 本头文件。

## 延伸阅读

1. 各个 `CodeGenerator*.cpp` 文件——每个 spread 对应本头文件中的一组方法实现。
2. `CGHelpers.h`——mHelpers 的类型映射工具。
3. `../moonir/MoonIR.h`——MoonIR 中间表示。

---

---
title: src/codegen/CodeGeneratorCleanup.cpp
path: src/codegen/CodeGeneratorCleanup.cpp
阶段: 代码生成 (CodeGen)——资源清理/所有权释放
语言: C++
---
