# src/codegen/CodeGeneratorCleanup.cpp — Complete LLVM IR Generation for Resource Cleanup (Drop/Dealloc/Result/Enum/Array/Record)

## What This File Does

This file implements the complete LLVM IR generation logic for resource cleanup (drop and deallocation) in the Luna runtime. It covers the full set of paths, from canonical CFG cleanup records (`emitCanonicalCleanup`) to resource contents cleanup (`emitResourceContentsCleanup`), owned payload cleanup (`emitOwnedPayloadCleanup`), heap deallocation (`emitLunaDeallocation`), tag-forked cleanup for Result/Enum, materialized iterator source cleanup (`emitMaterializedIteratorCleanup`), cleanup by name/action (`emitCleanup`), and dynamic drop callbacks (`getOrCreateDropCallback`). It also provides `packResultPayload`/`unpackResultPayload` to pack/unpack between a Result/Enum's tag and payload.

For C++ readers: this is Luna's "destructor generator". It does not rely on C++ RAII; instead, it emits explicit release/deconstruction code at every exit point of the CFG based on Luna's ownership analysis (`luna::ownership::CleanupAction`) — similar to Rust's Drop glue generator.

## Key Functions and Methods

**`llvm::Function* CodeGenerator::getOrCreateDropCallback(const TypePtr& type)`**
- For a given type, generates a function with the unique symbol `__luna_drop_callback_<hash>` derived via `typeId -> stableIdentityHash`, which internally calls `emitOwnedPayloadCleanup` to handle value cleanup. Cached in `mDropCallbacks`.
- Called by: `generateCall` when handling the `drop_callback` builtin. Calls: `emitOwnedPayloadCleanup`.

**`void emitLunaDeallocation(Value* pointer, const TypePtr& type)`**
- Calls `rt_dealloc` (ptr, size, alignment); the size comes from `typeSize`/`typeAlignment`.
- Called by: `emitOwnedPayloadCleanup` when releasing pointer-typed types.

**`Value* packResultPayload(Value* value, const TypePtr& type, const TypePtr& resultType)`** / **`Value* unpackResultPayload(Value* bits, const TypePtr& type, uint64_t byteOffset)`**
- `packResultPayload`: in a Result structure's payload slot (`[N x i64]`), copies the value into aligned temporary storage via MemCpy according to `lua::layout::valueSize`, then extracts it.
- `unpackResultPayload`: reads the target type's value from the payload slot at `byteOffset`.
- Called by: `generateResultConstruct`/`generateVariantConstruct`/`generateTry`/`generateCall` (Ok/Err/unwrap) and switch cleanup.

**`void emitResourceContentsCleanup(Value* value, const TypePtr& type, const string& label)`**
- Recursively cleans up resource contents: calls the `type->dropGlue` function for resources with `needsDrop`; for `Struct`/`Array`/`Record`/`Closure`/`Result`/`Enum`, expands their fields/elements/variants respectively and recursively calls `emitOwnedPayloadCleanup` on each. Enums use a `switch` to dispatch variant cleanup by tag.
- Called by: `emitOwnedPayloadCleanup`. Calls: `resolveFunction` (dropGlue), `emitOwnedPayloadCleanup` (recursively).

**`void emitOwnedPayloadCleanup(Value* value, const TypePtr& type, const string& label)`**
- Entry point: returns immediately if `typeRequiresCleanup` returns false. `String`/`CStr` are no-ops (immutable global constants; no heap-allocated text currently). `DeviceBuffer` calls `rt_gpu_free`. `Array`/`Record`/`Result`/`Enum`/`Closure` delegate to `emitResourceContentsCleanup`. Other pointer-typed values delegate to `emitResourceContentsCleanup` plus `emitLunaDeallocation` to free them.
- Called by: `emitCleanup`/`emitCanonicalCleanup`/`emitResourceContentsCleanup`/`getOrCreateDropCallback`, etc.

**`void emitMaterializedIteratorCleanup(const string& name)`**
- For a materialized iterator's source array, checks element by element against `sourceDropFlags` whether it is still initialized; if so, sets its flag to `false` and releases that element.
- Called by: `emitIteratorPipeline` (after exit) and `emitCleanup`.

**`void emitCleanup(const string& place, CleanupAction action)`** / **`void emitCanonicalCleanup(const CleanupRecord& cleanup)`**
- `emitCleanup`: looks up `mLocals`/`mMaterializedIterators` by name, loads the value, and dispatches by action (ResultDrop/EnumDrop/ArrayDrop/RecordDrop/DeviceRelease/Drop/Deallocate/None).
- `emitCanonicalCleanup`: the canonical CFG version — locates storage via PlaceRef projections (field/index/deref), then performs cleanup by action, with support for guarded cleanup (checking via cursor whether an element is out of bounds).
- Called by: the cleanups in `generateControlFlowBody` and its `free` statements, as well as `generateTry`'s cleanup lists. Calls: `emitOwnedPayloadCleanup`/`emitLunaDeallocation`.

## Relationship to Surrounding Files and Pipeline Stages

- It is the cleanup-generation submodule of the **Code Generation (CodeGen)** stage.
- Dependencies: `../core/TypeLayout.h` (layout/offsets), `../core/TypeRelations.h` (typeId/typeRequiresCleanup), `../runtime/RuntimeABI.h` (LUNA_DEFAULT_HOST_ALIGNMENT), `CodeGenerator.h`, `CGHelpers.h`.
- Callers: `CodeGeneratorControlFlow.cpp` (emitCleanups closures, emitCanonicalFree), `CodeGeneratorExpressions.cpp` (generateTry's cleanups), `CodeGeneratorIterator.cpp` (emitIteratorPipeline's exit/cleanup), `CodeGeneratorFunctions.cpp` (indirectly through ControlFlow).

## Further Reading

1. `CodeGeneratorControlFlow.cpp` — how CleanupRecord is referenced in the canonical CFG.
2. `../core/TypeRelations.h` — `typeRequiresCleanup` and `typeId`.
3. `../runtime/RuntimeABI.h` — `LUNA_DEFAULT_HOST_ALIGNMENT`.

---

---
title: src/codegen/CodeGeneratorControlFlow.cpp
path: src/codegen/CodeGeneratorControlFlow.cpp
stage: Code Generation (CodeGen) — canonical CFG body LLVM generation
language: C++
---
