# src/codegen/CodeGeneratorFunctions.cpp — Code Generation Entry Point for Function Bodies (Including main/Kernel Initialization)

## What This File Does

Implements `CodeGenerator::generateFunctionBody(FunctionDecl*)`: generates the LLVM code for a single Luna function's body. It is responsible for four things: determining the function's LLVM `Function` and return type; clearing the "current-function" member state (local table, canonical locals, array drop flags, materialized iterators, known upper bounds); if the function is `main`, injecting the host application service initialization `rt_install_application_host_services_v1`, and, when a kernel is present, injecting GPU initialization `rt_gpu_initialize` (on failure, calling `rt_gpu_report_initialization_error` to return an error code); then calling `generateControlFlowBody` to generate the canonical CFG body, and finally adding a void return.

For C++ readers: this is a "function-level entry adapter" — it translates declaration-level information (parameters/return type/whether it is `main`/whether a kernel exists) into a single `generateControlFlowBody` call and handles the prologue (entry initialization, state reset). The actual statement/control-flow generation lives in the ControlFlow file.

## Key Functions and Methods

**`void CodeGenerator::generateFunctionBody(FunctionDecl* decl)`**
- Logical order: (1) look up the LLVM `Function` in `mFunctions`/`mModule` by `generatedSymbolName` or `name`, and return if it cannot be found; (2) resolve the return type to `retLLVMType`; (3) return directly if `decl->isExtern`; if `decl->controlFlow` is empty, report the error "without exclusive canonical CFG body"; (4) set `mCurrentFunc` and `mCurrentFunctionIsKernel` (saving/restoring the old values) and clear `mLocals`/`mLocalTypes`/`mCanonicalLocals`/`mCanonicalLocalTypes`/`mArrayDropFlags`/`mMaterializedIterators`/`mLocalKnownUpperBounds`; (5) create the `entry` basic block and set the insertion point; (6) if it is `main`: insert a call to `rt_install_application_host_services_v1` (JIT and AOT share the entry strategy); (7) if `name==main` and `mProgram->features.kernel`: call `rt_gpu_initialize` and branch on the result to `readyBB`/`failedBB`, where `failedBB` calls `rt_gpu_report_initialization_error` and returns 1/null/void according to the return type (avoiding a null-pointer crash in AOT when the backend is misconfigured); (8) `generateControlFlowBody(*decl->controlFlow, func, entryBB)`; (9) if the return is void and there is no terminator, add `CreateRetVoid`; restore the `mCurrentFunc`/kernel flag.
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
3. runtime: `rt_install_application_host_services_v1`/`rt_gpu_initialize`/`rt_gpu_report_initialization_error`.

---

---
title: src/codegen/CodeGeneratorGpu.cpp
path: src/codegen/CodeGeneratorGpu.cpp
stage: Code Generation (CodeGen) — GPU kernel code emission and launch
language: C++
---
