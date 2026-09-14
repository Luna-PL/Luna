# src/codegen/CodeGeneratorExecution.cpp — Constructor / JIT Execution / Object File Output

## What This File Does

This file implements the lifecycle and execution core of `CodeGenerator`: construction, JIT execution, inspectable textual-IR output, and direct native-object output for AOT. Native targets are initialized normally; NVPTX/AMDGPU registries are initialized only when device output is requested.

For C++ readers: `jitRun` is the heaviest method in the entire file — it manually binds all of Luna's runtime `rt_*` helper symbols explicitly into JIT semantics (rather than relying on ELF `-rdynamic` / Mach-O exports / Windows `dllexport`), hands the `ThreadSafeModule` to LLJIT, then falls back to process symbols from libc/user libraries, and finally `lookup("main")` and invokes it.

## Key Structs, Classes, and Enums

Within the anonymous namespace:

- `initializeLunaLLVMTargets()` initializes the native target once; the separate device initializer registers all targets only for requested PTX/HSACO output.
- `using LunaJitEntry = int (*)()` and `int invokeLunaJitEntry(LunaJitEntry entry)` — wraps the call under `#if defined(__clang__) LLVM_NO_SANITIZE("function")` to avoid crashes from ORC-generated functions lacking UBSan metadata when probed at page boundaries.
- Under `_WIN32`, `void lunaJitMingwMain(){}` — a no-op symbol supplied because MinGW injects a `__main` call into functions named main.

## Key Functions and Methods

**`CodeGenerator::CodeGenerator(const string& moduleName)` / `~CodeGenerator()=default`**

- Initializes `mCtx` (`make_unique<LLVMContext>`), `mModule` (moduleName), `mBuilder`, `mHelpers`, then calls `initializeLLVM()`.

**`LunaJitRunResult CodeGenerator::jitRun()`**

- Creates the JIT with `LLJITBuilder()`.
- Binds the runtime helpers one by one via `bindRuntime(name, &func)` into a SymbolMap (Exported, mangleAndIntern), conditioned on that name already being referenced in the module. Coverage includes allocation, RC/ARC library storage, panic, host services, recoverable allocation, console/file I/O, runtime errors, array bounds, and GPU helpers. On Windows it additionally binds `lunaJitMingwMain` as `__main`.
- After defining into mainJITDylib (absoluteSymbols), calls `addIRModule(ThreadSafeModule(move(mModule), move(mCtx)))`.
- Adds the `EPCDynamicLibrarySearchGenerator::GetForTargetProcess` generator for falling back to libc/user libraries.
- `lookup("main")`, `toPtr<int()>()`, then `return invokeLunaJitEntry(mainFunction)`. All failure paths log and return 1.
- Who calls it: the upstream jitRun() layer; what it calls: the `rt_*` symbol addresses (from `../runtime/Runtime.h`).

**`bool CodeGenerator::emitObjectFile(const string& outputPath)`**

- Sets the triple to `getProcessTriple()`, opens the output with `raw_fd_ostream`, and writes textual IR via `mModule->print(dest)` (avoiding bitcode compatibility issues).
- Who calls it: the AOT path (the upstream compilation pipeline).

**`bool CodeGenerator::emitNativeObjectFile(const string& outputPath)`**

- Reuses the PIC host `TargetMachine` already configured for O2/O3 optimization (or creates it once for O0), then runs LLVM's object-file emission passes. The outer AOT driver gives this `.o` to the platform linker; recognized x86-64 MinGW/Clang executable builds invoke the companion `ld.lld` directly, while other layouts and shared libraries retain the selected clang driver.

## Relationship to Surrounding Files and Pipeline Stages

- Belongs to the **execution/output stage**: after generate, either `jitRun` executes immediately or the two emit methods retain `.ll` and produce the native linker input.
- Initializes native LLVM target support and lazily enables device registries used by the GPU implementation.

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
