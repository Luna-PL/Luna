# src/codegen/CodeGeneratorModule.cpp — generate() Main Flow

## What This File Does

Implements `CodeGenerator::generate(moon::Module*)` — the **top-level entry point / orchestrator** of the code generation stage. In order, it: initializes `mProgram` and `mTypeMaterializer` and clears the various maps; "declares" (`declareFunc`) an LLVM `Function` shell for every function and impl method in the module (including ABI visibility/linkage, template instantiation and selector filtering, and adding the NoReturn attribute for Never returns); emits runtime descriptors via `emitRuntimeDescriptors`; Pass A first generates the kernel function bodies (ensuring that device code objects exist before host emission); emits PTX/HSACO for kernels as needed; Pass B finally generates the host function bodies; verifies the host module and, when not at O0, runs the PassBuilder O2/O3 optimization pipeline followed by another verification; finally, success is judged by whether `mErrors` is empty.

## Key Structs, Classes, and Enums

This file defines no new types; the core consists of two local lambdas: `declareFunc` and `generateBodies(bool kernels)`. Types used: `moon::FunctionDecl`, `moon::ImplDecl` (MoonIR), plus LLVM's PassBuilder and analysis managers.

## Key Functions and Methods

**`bool CodeGenerator::generate(moon::Module* program)`**
- Initialization: `mProgram = program`; `mTypeMaterializer = new TypeMaterializer(*program)`; clears `mFunctions` / `mDropCallbacks` / `mKernelPTX` / `mKernelHSACO`.
- `declareFunc`: skips selectors; skips unreachable kernels; skips "type-parameterized but not a template instantiation". Uses `resolveType` to compute the parameter/return LLVM types, builds a `FunctionType`, and calls `Function::Create`. Visibility: `!program->isPackage || f->isExported || f->isExtern || f->name==main` → `ExternalLinkage`, otherwise `InternalLinkage`. Symbol names prefer `linkName`, then `generatedSymbolName` / `name`. Never returns get `Attribute::NoReturn`. Written into `mFunctions` (including name aliases).
- `generateBodies(kernels)`: iterates over the declarations — both `FunctionDecl` and `ImplDecl.methods` participate — filters (non-selector, `isKernel == kernels`, codegen-reachable, template-instantiated or no type parameters) and then calls `generateFunctionBody`.
- Pass 1: declares all functions/methods via `declareFunc` (resolving forward references).
- `emitRuntimeDescriptors()` (see RuntimeDescriptors.cpp).
- Pass 2 (kernels): `generateBodies(true)`; if `mGpuTargets.emitPTX`, calls `emitKernelPTX` for every reachable kernel (returns `false` on failure); likewise for `emitHSACO`.
- Pass 3 (host): `generateBodies(false)` — the host side first embeds the already-produced PTX/HSACO, avoiding AOT embedding of a temporary empty device module.
- Verification: `verifyHostModule(suffix)` uses `llvm::verifyModule(mModule, &stream)` to write errors to the diagnostics stream; if `mErrors` is empty but verification fails, returns `false`.
- Optimization: when `mErrors` is empty and `mOptimizationLevel != O0`, registers the analysis managers, constructs a PassBuilder, runs module optimization with `buildPerModuleDefaultPipeline` for O2 (O2/O3), then verifies again via `verifyHostModule(" after optimization")`.
- Returns `mErrors.empty()`.
- Callers: the upper compilation pipeline (after semantic analysis, `generate` is invoked with the module as the backend's first entry point). Callees: `declareFunc`, `emitRuntimeDescriptors`, `generateFunctionBody`, `emitKernelPTX` / `emitKernelHSACO`, `verifyModule`, and the modern PassBuilder.

## Relationship to Surrounding Files and Pipeline Stages

- It is the **main controller of the code generation stage**.
- Upstream: `generate()` is called after semantic analysis; execution may then continue with JIT (`jitRun`) or AOT (`emitObjectFile`).
- Downstream: `CodeGeneratorFunctions.cpp` (function bodies), `CodeGeneratorRuntimeDescriptors.cpp` (descriptors), `CodeGeneratorGpu.cpp` (kernel code objects), `CodeGeneratorControlFlow.cpp` / `Expressions` (inside function bodies).

## Further Reading

1. `CodeGenerator.h` — optimization level and GPU target configuration.
2. `CodeGeneratorRuntimeDescriptors.cpp` — the `emitRuntimeDescriptors` implementation.
3. LLVM Pass: `llvm::PassBuilder` and `buildPerModuleDefaultPipeline`.

---

---
title: src/codegen/CodeGeneratorRangeAnalysis.cpp
path: src/codegen/CodeGeneratorRangeAnalysis.cpp
stage: Code Generation (CodeGen)
role: Implementation of array index bounds analysis
language: C++
---
