# src/codegen/CodeGeneratorControlFlow.cpp —— 规范 CFG 的 LLVM 基本块与指令生成

## 这个文件做什么

实现 `CodeGenerator::generateControlFlowBody`——把 Luna 编译器下层的规范控制流图（`moon::ControlFlowGraph`）转换为一组 LLVM 基本块和指令。它负责：(1) 为每个规范局部（`graph.locals`）在函数 entry block 分配 alloca，按 LocalKind/Allocation/pointerBacked 决定 LLVM 类型（指针或值）；(2) 将参数从 LLVM 函数参数复制到对应的 alloca 中（闭包环境参数从指针加载）；(3) 创建 LLVM 基本块（`cfg.<id>`）并将 entry 块跳转到起始块；(4) 对每个块的操作（`LetStmt`/`FreeStmt`/`AllocateStmt`/`ExprStmt`/`AwaitStmt`）发射 LLVM 指令；(5) 对终结器（Jump/Branch/Return/Resume/Abort/Unreachable/Switch）发射对应的 LLVM 终结动作，包括清理（cleanups）的竹节与条件边；(6) 在 O3 下对简单的循环 latch 尝试设置 `llvm.loop.unroll.count` 元数据。

对 C++ 读者：这是「structured CFG lowering」的核心——把一批具有线性操作序列的基本块（每个块有若干操作+一个终结器）翻译成 LLVM 的 basic block 结构。它不生成表达式值（通过 `generateExpr` 调用），但负责所有存储分配、参数传递、分支边的清理以及 switch 的 tag 分派。

## 关键结构体·类·枚举

匿名命名空间内的辅助函数：
- `std::string localName(const moon::LocalRecord& local)`——返回 `"local." + id + "." + name` 格式的 alloca 名。
- `bool shouldUnrollCanonicalLatch(const BasicBlock* body, const BranchInst* latch)`——检查 latch 是否属于目标 body 且指令数在 24-48 之间、无 volatile/atomic 操作、无 call 指令，决定是否标记循环展开。
- `void setCanonicalLoopUnrollCount(BranchInst* latch, LLVMContext&, unsigned count)`——给 latch 分支添加 `llvm.loop.unroll.count` 元数据。

## 关键函数·方法

**`void CodeGenerator::generateControlFlowBody(moon::ControlFlowGraph& graph, llvm::Function* func, llvm::BasicBlock* abiEntry)`**
- 整体流程：
  1. **分配规范局部存储**：`mCanonicalLocals.assign(graph.locals.size(), nullptr)`，`mCanonicalLocalTypes` 同理。先扫描找出 `pointerBackedLocals`（由 `InitAllocationExpr` 初始化的 local）。然后对每个 local：按 `LocalKind::Allocation` 或 pointerBacked 用 `ptrTy()`，否则 `toLLVMType(type)`，void 型报错跳过。用 `createEntryBlockAlloca` 分配。
  2. **参数注入**：遍历 `LocalKind::Parameter` 的 local，从 LLVM 函数参数 `func->getArg(index)` 中取值，闭包环境参数从指针 load 出 struct，经 `coerceCallArgument` 后 store 到对应 alloca。
  3. **创建基本块**：为每个 graph.blocks 创建 LLVM 块，entry 跳转。
  4. **定义局部闭包**：`emitCleanups`（遍历 CleanupId 调 emitCanonicalCleanup）、`emitEdge`（直接跳转+清理）、`edgeTarget`（清理边若非空则创建 bridge 块）、`placeOf`（递归解析 Expr 到 PlaceRef）、`emitCanonicalFree`（从 FreeStmt 匹配到 cleanup 并调 emitCanonicalCleanup）。
  5. **处理每个块的操作**：`LetStmt`→`generateExpr` 初始值→`coerceCallArgument`→`CreateStore`；`FreeStmt`→`emitCanonicalFree`；`AllocateStmt`→调 `rt_alloc` 按 size/alignment 分配→store；`ExprStmt`→`generateExpr` 丢弃结果；`AwaitStmt`→`rt_gpu_await_event` 并 `emitGpuOperationFailureCheck`。
  6. **处理终结器**：Jump→emitEdge；Branch→求条件值→edgeTarget 两分支→CreateCondBr；Return→void 或非 void 生成值→emitCleanups→CreateRet；Resume/Abort→emitEdge；Unreachable→CreateUnreachable；Switch→提取 tag 与 payload→按 Enum/Result 变体布局→构建 switch 指令（case 块含 bindings 的 unpackResultPayload 与 emitCleanups）。
  7. **O3 循环展开提示**：对每个 Jump(target>block.id) 且 cleanups 为空、latch 符合条件者，设置 `setCanonicalLoopUnrollCount(latch, 4)`。
  8. 恢复 `mBuilder->SetInsertPoint(abiEntry)`。
- 谁调用：`generateFunctionBody`、`generateLambda`、`generateMakeClosure`。谁被调：`generateExpr`、`emitCanonicalCleanup`、`emitGpuOperationFailureCheck`、`createEntryBlockAlloca` 等。

## 与周边文件·阶段的关系

- 属**代码生成阶段**的核心——CFG lowering 到 LLVM 块结构的唯一实现。
- 上游：`CodeGeneratorFunctions.cpp` / `CodeGeneratorExpressions.cpp`（Lambda/MakeClosure 的 controlFlow 生成）。
- 下游：`CodeGeneratorCleanup.cpp`（emitCanonicalCleanup 清理发射）、`CodeGeneratorExpressions.cpp`（generateExpr 表达式求值）、`CodeGeneratorGpu.cpp`（emitGpuOperationFailureCheck）。
- 依赖 `../core/TypeLayout.h`（布局/偏移）与 `CodeGenerator.h` 的成员方法。

## 延伸阅读

1. `CodeGeneratorCleanup.cpp`——`emitCanonicalCleanup` 的清理实现。
2. `CodeGeneratorGpu.cpp`——`emitGpuOperationFailureCheck`。
3. llvm::BasicBlock、BranchInst、SwitchInst 的 LLVM 文档。

---

---
title: src/codegen/CodeGeneratorExecution.cpp
path: src/codegen/CodeGeneratorExecution.cpp
阶段: 代码生成 (CodeGen)——JIT 执行与 AOT 落盘
语言: C++
---
