# src/codegen/CodeGeneratorFunctions.cpp — Code Generation Entry Point for Function Bodies (Including main/Kernel Initialization)

## What This File Does

Implements `CodeGenerator::generateFunctionBody(FunctionDecl*)`: generates the LLVM code for a single Luna function's body. It determines the LLVM `Function` and return type, clears current-function state, injects GPU initialization for a kernel-using `main`, calls `generateControlFlowBody`, and finally adds a void return. Application host-service installation is selected later in `CodeGeneratorModule.cpp`, after optimization has exposed which input/filesystem calls survive.

For C++ readers: this is a "function-level entry adapter" — it translates declaration-level information (parameters/return type/whether it is `main`/whether a kernel exists) into a single `generateControlFlowBody` call and handles the prologue (entry initialization, state reset). The actual statement/control-flow generation lives in the ControlFlow file.

## Key Functions and Methods

**`void CodeGenerator::generateFunctionBody(FunctionDecl* decl)`**
- Logical order: (1) look up the LLVM `Function` in `mFunctions`/`mModule`; (2) resolve `retLLVMType`; (3) skip externs and reject a missing canonical CFG; (4) save/reset current-function state; (5) create `entry`; (6) for a kernel-using `main`, call `rt_gpu_initialize` and return an appropriate failure value after `rt_gpu_report_initialization_error`; (7) call `generateControlFlowBody`; (8) add a missing void return and restore state. `CodeGeneratorModule.cpp` subsequently optimizes the module and injects `rt_install_application_host_services_v1` only for surviving input/filesystem/direct-host/GPU use.
- Who calls it: `generateBodies` in `CodeGeneratorModule.cpp` (for all non-selector, non-template-parameter, codegen-reachable functions/impl methods, in two passes: kernel first, then host). What it calls: `generateControlFlowBody` and the various lower-level generate methods.

## Relationship to Surrounding Files and Pipeline Stages

- Belongs to the **code generation stage**: the unified entry point for function-level code generation.
- Upstream: `CodeGeneratorModule.cpp` (iterates over declarations and calls it, kernel first, then host).
- Downstream: `CodeGeneratorControlFlow.cpp` (generates the body); the special `main` logic references the runtime's `rt_*` symbols.
- Works with `CodeGeneratorGpu.cpp`: the GPU initialization downcall is performed by the `rt_gpu_*` runtime functions; `mProgram->features.kernel` decides whether it is inserted.
- Depends on the member state in `CodeGenerator.h` and utilities such as `resolveType`.

## Further Reading

1. `CodeGeneratorModule.cpp` — the declaration/function entry table and the two-pass dispatch.
2. `CodeGeneratorControlFlow.cpp` — how the canonical CFG body is invoked by this function.
3. runtime: `rt_gpu_initialize`/`rt_gpu_report_initialization_error`; conditional host-profile injection lives in `CodeGeneratorModule.cpp`.

---

---
title: src/codegen/CodeGeneratorGpu.cpp
path: src/codegen/CodeGeneratorGpu.cpp
stage: Code Generation (CodeGen) — GPU kernel code emission and launch
language: C++
---
