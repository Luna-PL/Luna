# src/codegen/CodeGeneratorIterator.cpp —— 迭代器管线（plan/build/pipeline/terminal）的全部 LLVM 生成

## 这个文件做什么

本文件实现 `CodeGenerator` 中迭代器相关的全部方法：对 Luna 的迭代器表达式（从数组/切片/range 源加上 Map/Filter/Take 适配器链，最终以 Fold/ForEach/Count/Collect 终结）从「构建迭代器计划」（`buildIteratorPlan`）到「物化绑定」（`materializeIteratorBinding`）再到「发射管线」（`emitIteratorPipeline`）与「终止器」（`generateIteratorTerminal`）的完整 LLVM 生成。此外还包含 `emitCallableInvocation`（对闭包与函数指针的通用调用发射）。

对 C++ 读者：迭代器是 Luna 的「零开销抽象」之一——`buildIteratorPlan` 只做类型分析（不产生 IR），`emitIteratorPipeline` 直接把适配器链展开为循环（condition→body→next→exit 四块），每个适配器（map/filter/take）在循环体中用内联代码处理。这也是「表达式模板 + 控制流」的混合后端模式。

## 关键函数·方法

**`bool CodeGenerator::buildIteratorPlan(Expr* expr, IteratorPlan& plan)`**
- 解析给定表达式是否构成一个有效的迭代器源。处理三种情形：
  - 直接标识符（数组/切片）：识别为 `Shared`（Slice）或 `Consuming`（Array）模式，从 `mMaterializedIterators` 或 `mLocalTypes` 取类型。
  - `Range(low,high)` 调用：`IteratorOp::Range`，`itemType=TyI32`，`mode=Range`。
  - 成员方法调用 `obj.iter()`/`iter_mut()`/`into_iter()`：识别为 `Shared`/`Mutable`/`Consuming` 模式。
  - 链式 `.map(f)`/`.filter(f)`/`.take(n)`：递归 `buildIteratorPlan` 取前缀，然后在 `plan.steps` 尾部追加适配器。
- 谁调用：`generateIteratorTerminal`、`materializeIteratorBinding`、`emitIteratorPipeline`。谁被调：只读 MoonIR AST。

**`bool CodeGenerator::materializeIteratorBinding(const string& name, const IteratorPlan& plan)`**
- 为可复用的迭代器变量（如 `for x in iter` 中提前计算的源）预先分配并求值源数据、limit、index、drop flags。对每个 Map/Filter/Take 适配器预求值参数（closure 或 count）。
- 谁调用：`CodeGeneratorControlFlow.cpp` 中处理迭代器变量时。谁被调：`generateExpr`、`createEntryBlockAlloca`。

**`void CodeGenerator::emitIteratorPipeline(const IteratorPlan& plan, const std::function<void(Value*)>& consume, const std::function<void()>& prepareTerminal)`**
- 生成 LLVM 循环：condition→body→next→exit 四块。在 body 中：从源取元素（数组 GEP/切片 GEP/range 索引），消费源 drop flag 标记，然后对每个适配器步依次处理：`Map` 调 `emitCallableInvocation(f, item)`→`Filter` 条件判断（拒绝时清理 move-only 项）→`Take` 递减剩余计数（耗尽时清理 move-only 项并跳 exit）。最后调用 `consume(item)` 回调，跳 next 递增 index，回 condition。exit 后清理物化迭代器。
- 谁调用：`generateIteratorTerminal`。谁被调：`emitCallableInvocation`、`emitOwnedPayloadCleanup`、`emitMaterializedIteratorCleanup`。

**`llvm::Value* CodeGenerator::generateIteratorTerminal(CallExpr* call)`**
- 分派四种终端操作：`Fold`（accumulator + reducer closure 的管线闭包→尾折叠）、`ForEach`（action closure 逐个调用）、`Count`（计数器递增+清理 move-only 项）、`Collect`（FromIterator 三函数协议 begin/push/finish 调用）。每个终端先设 `plan.ownedStateName` 用于管线末尾清理，最后调用 `finishOwnedRecipe` 清理并 `erase` 相关状态。
- 谁调用：`generateCall` 在检测到 `IteratorOp::Fold/ForEach/Count/Collect` 时。谁被调：`emitIteratorPipeline`、`resolveFunction`(collect 协议)。

**`llvm::Value* CodeGenerator::emitCallableInvocation(Value* callable, const TypePtr& callableType, ArrayRef<Value*> arguments, Type* returnType, const string& name)`**
- 对 **Closure 类型**：在 entry block 分配 closure 结构存储，load 首字段（code pointer），构造 `{env_ptr, args...}` 参数列表，通过 `FunctionType` 调用 code pointer。对 **非闭包**（函数指针）：直接按参数类型构造 `FunctionType` 并 `CreateCall`。
- 谁调用：`emitIteratorPipeline`（map/filter 回调）、`generateCall`（间接调用闭包/函数）。

## 与周边文件·阶段的关系

- 属**代码生成阶段**的迭代器专有生成模块。
- 上游：`CodeGeneratorExpressions.cpp` 的 `generateCall`（检测并派发到 `generateIteratorTerminal`）。
- 下游：`CodeGeneratorCleanup.cpp`（`emitOwnedPayloadCleanup`/`emitMaterializedIteratorCleanup`）。
- 依赖 `CodeGenerator.h` 的 IteratorPlan/RuntimeIteratorStep/MaterializedIterator 结构体。

## 延伸阅读

1. `CodeGeneratorExpressions.cpp`——`generateCall` 中 iterator terminal 的检测。
2. `CodeGeneratorCleanup.cpp`——`emitOwnedPayloadCleanup`（move-only 项清理）与 `emitMaterializedIteratorCleanup`。
3. `CodeGenerator.h`——`IteratorStep`/`IteratorPlan`/`RuntimeIteratorStep`/`MaterializedIterator` 四个结构体的完整定义。

---

---
title: src/codegen/CodeGeneratorModule.cpp
path: src/codegen/CodeGeneratorModule.cpp
阶段: 代码生成 (CodeGen)——模块级主流程
语言: C++
---
