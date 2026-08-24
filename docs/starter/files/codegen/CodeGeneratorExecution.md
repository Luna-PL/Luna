# src/codegen/CodeGeneratorExecution.cpp — Constructor / JIT Execution / Object File Output

## What This File Does

This file implements the lifecycle and execution core of `CodeGenerator`: the constructor (which initializes the LLVM context/module/IRBuilder/CGHelpers and initializes LLVM target support), the destructor, `jitRun()` (which JIT-compiles and executes the generated `main` via ORC LLJIT and returns its exit code), and `emitObjectFile()` (which writes the module to disk as textual IR for AOT use). The file also contains an anonymous namespace: `initializeLLVM()` performs one-time initialization of the target backends; `invokeLunaJitEntry` is the sole JIT boundary and is exempted from the UBSan function check; `lunaJitMingwMain()` is an empty `__main` placeholder for MinGW.

For C++ readers: `jitRun` is the heaviest method in the entire file — it manually binds all of Luna's runtime `rt_*` helper symbols explicitly into JIT semantics (rather than relying on ELF `-rdynamic` / Mach-O exports / Windows `dllexport`), hands the `ThreadSafeModule` to LLJIT, then falls back to process symbols from libc/user libraries, and finally `lookup("main")` and invokes it.

## Key Structs, Classes, and Enums

Within the anonymous namespace:

- `void initializeLLVM()` — a static flag guards against re-entry; performs a one-time `InitializeNativeTarget(AsmPrinter/AsmParser)` and `InitializeAllTargets/MCs/AsmPrinters`.
- `using LunaJitEntry = int (*)()` and `int invokeLunaJitEntry(LunaJitEntry entry)` — wraps the call under `#if defined(__clang__) LLVM_NO_SANITIZE("function")` to avoid crashes from ORC-generated functions lacking UBSan metadata when probed at page boundaries.
- Under `_WIN32`, `void lunaJitMingwMain(){}` — a no-op symbol supplied because MinGW injects a `__main` call into functions named main.

## Key Functions and Methods

**`CodeGenerator::CodeGenerator(const string& moduleName)` / `~CodeGenerator()=default`**

- Initializes `mCtx` (`make_unique<LLVMContext>`), `mModule` (moduleName), `mBuilder`, `mHelpers`, then calls `initializeLLVM()`.

**`int CodeGenerator::jitRun()`**

- Creates the JIT with `LLJITBuilder()`.
- Binds the runtime helpers one by one via `bindRuntime(name, &func)` into a SymbolMap (Exported, mangleAndIntern), conditioned on that name already being referenced in the module. Coverage: alloc/realloc/dealloc, RC/ARC (rt_rc_* / rt_arc_*), panic, host_services, checked_array_layout, try_alloc/realloc, console I/O, file I/O, path metadata, runtime_error, raw malloc/free, print_i32/cstr, 0.2-compat symbols, array_index_or_abort, dynamic fragments/plugins (rt_dynamic_fragment_* / rt_fragment_plugin_*), GPU (rt_gpu_*). On Windows it additionally binds luaJitMingwMain as `__main`.
- After defining into mainJITDylib (absoluteSymbols), calls `addIRModule(ThreadSafeModule(move(mModule), move(mCtx)))`.
- Adds the `EPCDynamicLibrarySearchGenerator::GetForTargetProcess` generator for falling back to libc/user libraries.
- `lookup("main")`, `toPtr<int()>()`, then `return invokeLunaJitEntry(mainFunction)`. All failure paths log and return 1.
- Who calls it: the upstream jitRun() layer; what it calls: the `rt_*` symbol addresses (from `../runtime/Runtime.h`).

**`bool CodeGenerator::emitObjectFile(const string& outputPath)`**

- Sets the triple to `getProcessTriple()`, opens the output with `raw_fd_ostream`, and writes textual IR via `mModule->print(dest)` (avoiding bitcode compatibility issues).
- Who calls it: the AOT path (the upstream compilation pipeline).

## Relationship to Surrounding Files and Pipeline Stages

- Belongs to the **execution/output stage**: after generate, either `jitRun` executes immediately or `emitObjectFile` writes AOT artifacts.
- Only includes `CodeGenerator.h` and `../runtime/Runtime.h`; does not depend on other codegen implementation files.
- Initializes LLVM target support, used by the NVPTX/AMDGPU backends in the Gpu file.

## Further Reading

1. `../runtime/Runtime.h` — the list of `rt_*` symbols.
2. LLVM ORC/LLJIT documentation — lookup/binding/search generators.
3. `CodeGeneratorGpu.cpp` — GPU calls are backed by the `rt_gpu_*` symbols bound in this file.

---

---
title: src/codegen/CodeGeneratorExpressions.cpp
path: src/codegen/CodeGeneratorExpressions.cpp
stage: Code generation (CodeGen) — LLVM lowering for all expression nodes
language: C++
---
