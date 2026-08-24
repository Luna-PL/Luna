# src/codegen/CodeGenerator.cpp — Implementation of the CodeGenerator's Common Helper Methods

## What This File Does

This file centrally implements a set of "common / shared-service" methods of the `CodeGenerator` class that are repeatedly called by other codegen implementation files: argument type coercion `coerceCallArgument`, type resolution `resolveType`, declaration/function resolution `resolveDeclaration`/`resolveFunction`, allocation type inference for expressions `allocationTypeForExpr`, entry-block alloca `createEntryBlockAlloca`, error collection `error`, and field index lookup `fieldIndex`. It does not itself implement any specific AST node or stage; instead, it is the utility layer.

For C++ readers: it is the equivalent of consolidating a set of cross-module private helpers into a single .cpp — avoiding duplicate definitions and letting downstream code depend only on the declarations without worrying about implementation details.

## Key Functions and Methods

**`llvm::Value* CodeGenerator::coerceCallArgument(llvm::Value* value, llvm::Type* target)`**
- Normalizes an already-evaluated argument to the target LLVM type: returns it as-is if the types match; integer-to-integer goes through `CreateIntCast` (signed, named abiarg); pointer-to-pointer goes through `CreateBitCast`; everything else is returned as-is.
- Callers: almost everywhere values are fed into CreateCall/CreateStore/entry parameters; called by: LLVM IRBuilder base instructions.

**`TypePtr CodeGenerator::resolveType(const moon::TypeRef& reference)`**
- Materializes a type reference into a TypePtr using `mTypeMaterializer`; returns null if there is no materializer.
- Callers: all codegen files that depend on type information; called by: `moon::TypeMaterializer::materialize`.

**`resolveDeclaration` / `resolveFunction`**
- `resolveDeclaration` looks up the declaration record via `mProgram->findDeclaration`; `resolveFunction` first obtains the record through it, then looks up the generated LLVM function in the `mFunctions` hash table, falling back to `mModule->getFunction(linkageName)` otherwise. It is the single authority for fixed ABI name resolution.
- Callers: function calls in Expressions, the collect protocol of Iterator, parsing in ControlFlow, etc.

**`allocationTypeForExpr` / `createEntryBlockAlloca`**
- `allocationTypeForExpr` unwraps along Move/Borrow/Identifier to obtain the allocation type (recursively takes the operand for Move/Borrow, looks up mLocalTypes for Identifier, otherwise falls back to resolveType(expr->type)).
- `createEntryBlockAlloca` uses a temporary builder to insert the alloca at the top of the function's entry block and returns it — the single entry point for all local storage slots.
- Callers: ControlFlow/Expressions/Iterator/Gpu, etc.

**`error`** and **`fieldIndex`**
- `error(msg)` appends a codegen diagnostic (diagnostic::format) to `mErrors`, with the note that it is "usually caused by a previously invalid declaration or an unsupported construct".
- `fieldIndex` returns the index of a field name in `type->fields`; returns `(size_t)-1` if it does not exist. Used by field access/assign, record literals, etc.

## Relationship to Surrounding Files and Pipeline Stages

- A shared utility implementation belonging to the **code generation stage**, called by almost all other CodeGenerator* files.
- Depends on `../diagnostics/Diagnostic.h` and LLVM IR. Corresponds to the declarations in `CodeGenerator.h`.

## Further Reading

1. `CodeGenerator.h` — the utility method signatures and data structures.
2. `CodeGeneratorModule.cpp` — the main `generate` flow.
3. `../moonir/MoonIR.h` — the semantics of TypeRef/DeclarationRef/TypeMaterializer.

---

---
title: src/codegen/CodeGenerator.h
path: src/codegen/CodeGenerator.h
stage: Code Generation (CodeGen) — core class declaration
language: C++
---

# src/codegen/CodeGenerator.h — The Core Class Declaration and All Internal Data Structures of the Code Generator

## What This File Does

`CodeGenerator.h` is the **core header file** of the Luna backend. It declares:
- The enum `LunaOptimizationLevel` (O0/O2/O3), which controls the LLVM module-level optimization pipeline.
- The struct `LunaGpuTargetConfig`, which configures the kernel code-object targets for CUDA (sm_52) and ROCm (gfx1101).
- Four internal data structures related to iterator code generation: `IteratorStep`, `IteratorPlan`, `RuntimeIteratorStep`, and `MaterializedIterator`.
- All public interfaces and private members/methods of the `CodeGenerator` class, including all `generate` methods, member variables (the LLVM context, IRBuilder, the CGHelpers helper, and the various maps and state), as well as the concrete expression generators dispatched from `generateExpr`.

For C++ readers: this file is the "API boundary + implementation state definition" of the entire codegen subsystem. It lists all `generate*` methods (every Luna expression/statement/control-flow construct has a corresponding LLVM generation method), along with large amounts of intermediate state such as `mLocals`, `mLocalTypes`, `mCanonicalLocals`, `mFunctions`, `mMaterializedIterators`, and `mKernelPTX`/`mKernelHSACO`.

## Key Structs, Classes, and Enums

**`enum class LunaOptimizationLevel`**: O0 (default), O2, O3.

**`struct LunaGpuTargetConfig`**:
- `bool emitPTX`: whether to emit CUDA PTX for kernels.
- `std::string cudaArchitecture`: defaults to `sm_52`.
- `bool emitHSACO`: whether to emit ROCm HSACO for kernels.
- `std::string rocmArchitecture`: defaults to `gfx1101`.

**`struct IteratorStep` / `IteratorPlan` / `RuntimeIteratorStep` / `MaterializedIterator`** (private, used for iterator unrolling):
- `IteratorStep`: `{op, argument, inputType, outputType}` describes a single adapter operation (Map/Filter/Take).
- `IteratorPlan`: `{source, sourceType, itemType, mode, rangeStart, rangeEnd, ownedStateName, materializedName, steps}` describes a complete iterator (from the source to the chain of adapters).
- `RuntimeIteratorStep`: `{description, value, remaining}` the evaluated arguments of a runtime step and the Take remaining counter.
- `MaterializedIterator`: `{plan, sourceData, limit, indexStorage, sourceDropFlags, ownsSource, steps}` all the IR values after the iterator is materialized into LLVM state.

**`class CodeGenerator`** (public):
- Public methods: `CodeGenerator(moduleName)`, `~CodeGenerator()`, `generate(Module*)`, `setOptimizationLevel`, `setGpuTargets`, `jitRun()`, `emitObjectFile()`, `errors()` const.
- Private methods (~70 of them, see lines 52-176 of the header): grouped by function — `generateFunctionBody`, `generateControlFlowBody`, `generateExpr` (the main entry point); the `generate*` methods for literals/access/arithmetic/construction/calls/control flow/ownership/closures/iterators/GPU; `emitRuntimeDescriptors`, `emitKernelPTX/HSACO`, the `emitCleanup` family, and so on.
- Private member variables (lines 177-218): `mCtx` (LLVMContext), `mModule`, `mBuilder`, `mHelpers` (CGHelpers); `mProgram` (moon::Module*); `mTypeMaterializer`; `mLocals`/`mLocalTypes` (name->alloca/type mapping); `mCanonicalLocals`/`mCanonicalLocalTypes` (LocalId->alloca/type); `mArrayDropFlags` (name->drop bit); `mMaterializedIterators`; `mLocalKnownUpperBounds` (name->exclusive upper bound); `mCurrentFunc`/`mCurrentFunctionIsKernel`; `mFunctions`/`mDropCallbacks`; `mKernelPTX`/`mKernelHSACO`; `mErrors`; `mOptimizationLevel`; `mGpuTargets`.

## Key Functions and Methods

The signatures and roles of all `generate*` methods are commented line by line in the header. Brief overview:
- `generateExpr(Expr*)`: the master dispatcher for expression generation; it dynamic_casts the AST subclass and dispatches to the corresponding `generateXxx`.
- `generateIntLiteral/FloatLiteral/StringLiteral/BoolLiteral/UnitLiteral/ArrayLiteral`: literals.
- `generateIdentifier/DynamicSelect/FieldAccess/SliceLength/Index`: access.
- `generateBinary/Unary`: arithmetic.
- `generateVariantConstruct/ResultConstruct/RecordLiteral/InitAllocation/HeapAlloc`: construction.
- `generateCall/generateLaunch`: calls / GPU launches.
- `generateTry/Assign/Move/Borrow/Deref/AddrOf/Lambda/EnvLoad/MakeClosure`: control flow / ownership / closures.
- `buildIteratorPlan/materializeIteratorBinding/emitIteratorPipeline/generateIteratorTerminal/emitCallableInvocation`: iterators.
- `emitKernelPTX/emitKernelHSACO/generateDeviceBufferPointer/generateHostRawPointer/emitGpuOperationFailureCheck`: GPU.
- `emitRuntimeDescriptors/emitLunaDeallocation/packResultPayload/unpackResultPayload/emitResourceContentsCleanup/emitOwnedPayloadCleanup/emitMaterializedIteratorCleanup/emitCleanup/emitCanonicalCleanup/getOrCreateDropCallback`: cleanup / runtime metadata.
- Utility methods: `coerceCallArgument/resolveType/resolveDeclaration/resolveFunction/allocationTypeForExpr/createEntryBlockAlloca/fieldIndex/error`.

## Relationship to Surrounding Files and Pipeline Stages

- The core header file of the **code generation stage** — every `CodeGenerator*.cpp` implementation file begins with `#include "CodeGenerator.h"`, making it the ABI boundary of the entire backend.
- Dependencies: `../moonir/MoonIR.h` (moon::Module, Expr, FunctionDecl, etc.), `CGHelpers.h`, the LLVM IR headers, and `../diagnostics/Diagnostic.h`.
- Included by the implementation files: CodeGenerator.cpp, Module.cpp, Functions.cpp, ControlFlow.cpp, Expressions.cpp, Cleanup.cpp, Iterator.cpp, Gpu.cpp, Execution.cpp, and RuntimeDescriptors.cpp all include this header.

## Further Reading

1. The various `CodeGenerator*.cpp` files — each one corresponds to a set of method implementations from this header.
2. `CGHelpers.h` — the type-mapping utility for mHelpers.
3. `../moonir/MoonIR.h` — the MoonIR intermediate representation.

---

---
title: src/codegen/CodeGeneratorCleanup.cpp
path: src/codegen/CodeGeneratorCleanup.cpp
stage: Code Generation (CodeGen) — resource cleanup / ownership release
language: C++
---