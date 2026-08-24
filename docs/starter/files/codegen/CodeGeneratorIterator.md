# src/codegen/CodeGeneratorIterator.cpp — All LLVM generation for the iterator pipeline (plan/build/pipeline/terminal)

## What This File Does

This file implements all iterator-related methods in `CodeGenerator`: the full LLVM generation for Luna’s iterator expressions — from array/slice/range sources plus chains of Map/Filter/Take adapters, terminated by Fold/ForEach/Count/Collect — from building the iterator plan (`buildIteratorPlan`), through materializing bindings (`materializeIteratorBinding`), to emitting the pipeline (`emitIteratorPipeline`) and the terminals (`generateIteratorTerminal`). It also contains `emitCallableInvocation` (generic invocation emission for closures and function pointers).

For C++ readers: iterators are one of Luna’s “zero-overhead abstractions” — `buildIteratorPlan` only performs type analysis (producing no IR), whereas `emitIteratorPipeline` unrolls the adapter chain directly into a loop (the four blocks condition→body→next→exit), with each adapter (map/filter/take) handled by inline code inside the loop body. This is also the hybrid backend pattern of “expression templates + control flow”.

## Key Functions and Methods

**`bool CodeGenerator::buildIteratorPlan(Expr* expr, IteratorPlan& plan)`**
- Parses whether the given expression constitutes a valid iterator source. Handles three cases:
  - Direct identifier (array/slice): recognized as `Shared` (Slice) or `Consuming` (Array) mode, with the type taken from `mMaterializedIterators` or `mLocalTypes`.
  - `Range(low, high)` call: `IteratorOp::Range`, `itemType=TyI32`, `mode=Range`.
  - Member method calls `obj.iter()`/`iter_mut()`/`into_iter()`: recognized as `Shared`/`Mutable`/`Consuming` modes.
  - Chained `.map(f)`/`.filter(f)`/`.take(n)`: recursively calls `buildIteratorPlan` on the prefix, then appends the adapter to the end of `plan.steps`.
- Callers: `generateIteratorTerminal`, `materializeIteratorBinding`, `emitIteratorPipeline`. Callees: the read-only MoonIR AST.

**`bool CodeGenerator::materializeIteratorBinding(const string& name, const IteratorPlan& plan)`**
- Pre-allocates and evaluates the source data, limit, index, and drop flags for reusable iterator variables (such as a source computed early in `for x in iter`). Pre-evaluates the arguments (closure or count) of each Map/Filter/Take adapter.
- Callers: `CodeGeneratorControlFlow.cpp` when handling iterator variables. Callees: `generateExpr`, `createEntryBlockAlloca`.

**`void CodeGenerator::emitIteratorPipeline(const IteratorPlan& plan, const std::function<void(Value*)>& consume, const std::function<void()>& prepareTerminal)`**
- Generates the LLVM loop: the four blocks condition→body→next→exit. In the body: fetches an element from the source (array GEP/slice GEP/range index), consumes the source drop-flag marker, then processes each adapter step in turn: `Map` calls `emitCallableInvocation(f, item)`, `Filter` performs a condition check (cleaning up move-only items on rejection), `Take` decrements the remaining count (cleaning up move-only items and jumping to exit when exhausted). Finally invokes the `consume(item)` callback, jumps to next to increment the index, and returns to condition. After exit, cleans up the materialized iterator.
- Callers: `generateIteratorTerminal`. Callees: `emitCallableInvocation`, `emitOwnedPayloadCleanup`, `emitMaterializedIteratorCleanup`.

**`llvm::Value* CodeGenerator::generateIteratorTerminal(CallExpr* call)`**
- Dispatches the four kinds of terminal operations: `Fold` (pipeline closure of the accumulator plus a reducer closure → tail fold), `ForEach` (invokes the action closure once per item), `Count` (increments the counter and cleans up move-only items), `Collect` (the FromIterator three-function protocol begin/push/finish invocations). Each terminal first sets `plan.ownedStateName` for cleanup at the end of the pipeline, then finally calls `finishOwnedRecipe` to clean up and `erase` the associated state.
- Callers: `generateCall` when it detects `IteratorOp::Fold/ForEach/Count/Collect`. Callees: `emitIteratorPipeline`, `resolveFunction` (collect protocol).

**`llvm::Value* CodeGenerator::emitCallableInvocation(Value* callable, const TypePtr& callableType, ArrayRef<Value*> arguments, Type* returnType, const string& name)`**
- For **Closure types**: allocates a closure struct slot in the entry block, loads the first field (the code pointer), builds the argument list `{env_ptr, args...}`, and calls the code pointer through a `FunctionType`. For **non-closures** (function pointers): directly constructs a `FunctionType` from the argument types and emits a `CreateCall`.
- Callers: `emitIteratorPipeline` (map/filter callbacks), `generateCall` (indirect calls to closures/functions).

## Relationship to Surrounding Files and Pipeline Stages

- An iterator-specific generation module belonging to the **code generation stage**.
- Upstream: `generateCall` in `CodeGeneratorExpressions.cpp` (detects and dispatches to `generateIteratorTerminal`).
- Downstream: `CodeGeneratorCleanup.cpp` (`emitOwnedPayloadCleanup` / `emitMaterializedIteratorCleanup`).
- Depends on the IteratorPlan/RuntimeIteratorStep/MaterializedIterator structs in `CodeGenerator.h`.

## Further Reading

1. `CodeGeneratorExpressions.cpp` — iterator terminal detection in `generateCall`.
2. `CodeGeneratorCleanup.cpp` — `emitOwnedPayloadCleanup` (move-only item cleanup) and `emitMaterializedIteratorCleanup`.
3. `CodeGenerator.h` — full definitions of the four structs `IteratorStep`/`IteratorPlan`/`RuntimeIteratorStep`/`MaterializedIterator`.

---

---
title: src/codegen/CodeGeneratorModule.cpp
path: src/codegen/CodeGeneratorModule.cpp
stage: Code Generation (CodeGen) — module-level main flow
language: C++
lang: en
---
