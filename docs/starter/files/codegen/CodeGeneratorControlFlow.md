# src/codegen/CodeGeneratorControlFlow.cpp — LLVM Basic Block and Instruction Generation for the Canonical CFG

## What This File Does

Implements `CodeGenerator::generateControlFlowBody` — lowering the canonical control flow graph of the lower Luna compiler layers (`moon::ControlFlowGraph`) into a set of LLVM basic blocks and instructions. It is responsible for: (1) allocating an alloca in the function entry block for each canonical local (`graph.locals`), deciding the LLVM type (pointer or value) based on LocalKind/Allocation/pointerBacked; (2) copying parameters from the LLVM function arguments into the corresponding allocas (closure environment parameters are loaded from pointers); (3) creating LLVM basic blocks (`cfg.<id>`) and jumping from the entry block to the starting block; (4) emitting LLVM instructions for each block's operations (`LetStmt`/`FreeStmt`/`AllocateStmt`/`ExprStmt`/`AwaitStmt`); (5) emitting the corresponding LLVM terminator actions for terminators (Jump/Branch/Return/Resume/Abort/Unreachable/Switch), including cleanup injection and conditional edges; (6) attempting to set the `llvm.loop.unroll.count` metadata on simple loop latches under -O3.

For C++ readers: this is the heart of "structured CFG lowering" — translating a set of basic blocks with linear operation sequences (each block has several operations plus one terminator) into LLVM's basic block structure. It does not generate expression values (delegated via `generateExpr` calls), but it is responsible for all storage allocation, argument passing, cleanup along branch edges, and switch tag dispatch.

## Key Structs, Classes, and Enums

Helper functions in the anonymous namespace:
- `std::string localName(const moon::LocalRecord& local)` — returns the alloca name in the `"local." + id + "." + name` format.
- `bool shouldUnrollCanonicalLatch(const BasicBlock* body, const BranchInst* latch)` — checks whether the latch belongs to the target body and has an instruction count between 24 and 48, no volatile/atomic operations, and no call instructions, to decide whether to mark the loop for unrolling.
- `void setCanonicalLoopUnrollCount(BranchInst* latch, LLVMContext&, unsigned count)` — attaches the `llvm.loop.unroll.count` metadata to the latch branch.

## Key Functions and Methods

**`void CodeGenerator::generateControlFlowBody(moon::ControlFlowGraph& graph, llvm::Function* func, llvm::BasicBlock* abiEntry)`**
- Overall flow:
  1. **Allocate canonical local storage**: `mCanonicalLocals.assign(graph.locals.size(), nullptr)`, and likewise for `mCanonicalLocalTypes`. First scan to find the `pointerBackedLocals` (locals initialized by `InitAllocationExpr`). Then, for each local: use `ptrTy()` for `LocalKind::Allocation` or pointerBacked locals, otherwise `toLLVMType(type)`; void-typed locals report an error and are skipped. Allocate with `createEntryBlockAlloca`.
  2. **Parameter injection**: iterate over locals with `LocalKind::Parameter`, take the value from the LLVM function argument `func->getArg(index)`, load the struct from the pointer for closure environment parameters, pass it through `coerceCallArgument`, and store it into the corresponding alloca.
  3. **Create basic blocks**: create an LLVM block for each entry in graph.blocks, and jump from the entry block.
  4. **Define local closures**: `emitCleanups` (iterates over CleanupId calling emitCanonicalCleanup), `emitEdge` (direct jump plus cleanup), `edgeTarget` (creates a bridge block when the cleanup edge is non-empty), `placeOf` (recursively resolves an Expr to a PlaceRef), `emitCanonicalFree` (matches a cleanup from a FreeStmt and calls emitCanonicalCleanup).
  5. **Handle each block's operations**: `LetStmt`→`generateExpr` initial value→`coerceCallArgument`→`CreateStore`; `FreeStmt`→`emitCanonicalFree`; `AllocateStmt`→call `rt_alloc` to allocate by size/alignment→store; `ExprStmt`→`generateExpr` discarding the result; `AwaitStmt`→`rt_gpu_await_event` plus `emitGpuOperationFailureCheck`.
  6. **Handle terminators**: Jump→emitEdge; Branch→evaluate the condition value→edgeTarget for both branches→CreateCondBr; Return→generate the value for void or non-void→emitCleanups→CreateRet; Resume/Abort→emitEdge; Unreachable→CreateUnreachable; Switch→extract the tag and payload→lay out per the Enum/Result variant layout→build the switch instruction (case blocks containing unpackResultPayload for bindings and emitCleanups).
  7. **-O3 loop unroll hint**: for each Jump(target>block.id) with empty cleanups and a qualifying latch, set `setCanonicalLoopUnrollCount(latch, 4)`.
  8. Restore `mBuilder->SetInsertPoint(abiEntry)`.
- Callers: `generateFunctionBody`, `generateLambda`, `generateMakeClosure`. Called by: `generateExpr`, `emitCanonicalCleanup`, `emitGpuOperationFailureCheck`, `createEntryBlockAlloca`, etc.

## Relationship to Surrounding Files and Pipeline Stages

- Core of the **code generation stage** — the only implementation that lowers a CFG to LLVM block structure.
- Upstream: `CodeGeneratorFunctions.cpp` / `CodeGeneratorExpressions.cpp` (controlFlow generation for Lambda/MakeClosure).
- Downstream: `CodeGeneratorCleanup.cpp` (emitCanonicalCleanup cleanup emission), `CodeGeneratorExpressions.cpp` (generateExpr expression evaluation), `CodeGeneratorGpu.cpp` (emitGpuOperationFailureCheck).
- Depends on `../core/TypeLayout.h` (layout/offsets) and the member methods of `CodeGenerator.h`.

## Further Reading

1. `CodeGeneratorCleanup.cpp` — the cleanup implementation of `emitCanonicalCleanup`.
2. `CodeGeneratorGpu.cpp` — `emitGpuOperationFailureCheck`.
3. LLVM documentation for llvm::BasicBlock, BranchInst, SwitchInst.

---

---
title: src/codegen/CodeGeneratorExecution.cpp
path: src/codegen/CodeGeneratorExecution.cpp
stage: Code Generation (CodeGen) — JIT execution and AOT emission
language: C++
lang: en
---
