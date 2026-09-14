# src/codegen/CodeGeneratorModule.cpp — generate() Main Flow

## What This File Does

Implements `CodeGenerator::generate(moon::Module*)` — the **top-level entry point / orchestrator** of the code generation stage. It declares LLVM function shells, emits runtime descriptors, generates kernel bodies and requested PTX/HSACO before host bodies, verifies the host module, and runs the target-aware O2/O3 pipeline. After optimization it inspects surviving Runtime calls and injects the full application host profile only for input/filesystem/direct-host/GPU use, then performs the final verification.

## Key Structs, Classes, and Enums

This file defines no new types; the core consists of two local lambdas: `declareFunc` and `generateBodies(bool kernels)`. Types used: `moon::FunctionDecl`, `moon::ImplDecl` (MoonIR), plus LLVM's PassBuilder and analysis managers.

## Key Functions and Methods

**`bool CodeGenerator::generate(moon::Module* program)`**
- Initialization: `mProgram = program`; resets the reusable host `TargetMachine`; constructs `mTypeMaterializer`; clears `mFunctions` / `mDropCallbacks` / `mKernelPTX` / `mKernelHSACO`.
- `declareFunc`: skips selectors; skips unreachable kernels; skips "type-parameterized but not a template instantiation". Uses `resolveType` to compute the parameter/return LLVM types, builds a `FunctionType`, and calls `Function::Create`. Visibility: `!program->isPackage || f->isExported || f->isExtern || f->name==main` → `ExternalLinkage`, otherwise `InternalLinkage`. Symbol names prefer `linkName`, then `generatedSymbolName` / `name`. Never returns get `Attribute::NoReturn`. Written into `mFunctions` (including name aliases).
- `generateBodies(kernels)`: iterates over the declarations — both `FunctionDecl` and `ImplDecl.methods` participate — filters (non-selector, `isKernel == kernels`, codegen-reachable, template-instantiated or no type parameters) and then calls `generateFunctionBody`.
- Pass 1: declares all functions/methods via `declareFunc` (resolving forward references).
- `emitRuntimeDescriptors()` (see RuntimeDescriptors.cpp).
- Pass 2 (kernels): `generateBodies(true)`; if `mGpuTargets.emitPTX`, calls `emitKernelPTX` for every reachable kernel (returns `false` on failure); likewise for `emitHSACO`.
- Pass 3 (host): `generateBodies(false)` — the host side first embeds the already-produced PTX/HSACO, avoiding AOT embedding of a temporary empty device module.
- Verification: `verifyHostModule(suffix)` uses `llvm::verifyModule(mModule, &stream)` to write errors to the diagnostics stream; if `mErrors` is empty but verification fails, returns `false`.
- Optimization: when `mErrors` is empty and `mOptimizationLevel != O0`, creates and retains one PIC host `TargetMachine`, registers the analysis managers, and runs the corresponding O2/O3 `buildPerModuleDefaultPipeline`.
- Host profile: scans used declarations after optimization. Console input, filesystem, direct host-service, or GPU use causes a call to `rt_install_application_host_services_v1` to be inserted at the start of `main`; pure/allocation/output-only programs keep Runtime's lightweight default profile. Optimized modules are then verified again.
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
