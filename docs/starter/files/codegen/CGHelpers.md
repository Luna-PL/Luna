# src/codegen/CGHelpers.cpp — Luna Type to LLVM Type Conversion and Memory Size Calculation Implementation

## What This File Does

This file implements everything declared in `CGHelpers.h`: `CGHelpers::toLLVMType` (translating Luna types into LLVM types), and the file-level free functions `typeSize` (byte size) and `typeAlignment` (alignment). It contains no IR-generation logic; it is a pure "type-to-LLVM-type plus memory layout" mapping layer that the entire code generation backend calls as a common utility.

For C++ readers: this file is like a "reflective mapping table" — it uses `switch(type->kind)` to flatten the frontend kinds (TypeKind) into their corresponding LLVM types, and maintains a hand-written size table of "how many bytes each type occupies and at what byte alignment," which the backend uses to convert offsets when allocating, freeing, and copying.

## Key Structs, Classes, and Enums

The objects implemented by this file all come from the header or from external sources:
- The `CGHelpers` class and its private member `llvm::LLVMContext& mCtx` (this file implements only its constructor and `toLLVMType`).
- The two free functions `typeSize` and `typeAlignment`.
- The `TypeKind` enum it depends on (Luna frontend type kinds, from `../core/TypeSystem.h`).

## Key Functions and Methods

**`void* CGHelpers::toLLVMType`** (see the header for the signature)
- Dispatches on `type->kind`. Key implementation details:
- For `Result`, takes the larger of `lua::layout::valueSize` of `typeArgs[0]` (the value type) and `typeArgs[1]` (the error type), rounds up to 8-byte words, and builds `{ bool tag, [N x i64] payload }`.
- For `Enum`, uses `lua::layout::enumPayloadSize(type)` to fix the payload width and builds `{ i32 tag, [N x i64] payload }`.
- For `Array`, recurses on the element type to get `[arrayLength x inner]`; `Slice` is `{ptrTy, sizeTy}`; `Record` calls `toLLVMType` per field and then `StructType::get`; `Closure` has the code pointer as its first field and the captured fields as the rest.
- Who calls / who is called: it is used by every codegen file to obtain LLVM types; internally it only recurses into itself and into LLVM type construction.

**`uint64_t typeSize(const TypePtr& type)`**
- Fundamental integers/floats/bools return 1/2/4/8 by width; `String`/`CStr`/all kinds of pointers/`Reference`/`Iterator` return 8 (pointer size); `Slice` returns 16; `Event` returns 4; `Array` is `arrayLength * typeSize(inner)`; `Struct`/`Record`/`Closure`/`Enum`/`Result` defer to `lua::layout::*`; `Never` returns 0.
- Who calls it: `generateHeapAlloc` (the size argument of `rt_alloc`), `emitLunaDeallocation` and parts of the deallocation logic in `CodeGeneratorCleanup.cpp`, and `AllocateStmt` in `CodeGeneratorControlFlow.cpp`.

**`uint64_t typeAlignment(const TypePtr& type)`**
- Scalars get 1/2/4/8 by width; `Array` takes the element alignment; `Record` takes the maximum field alignment; `Closure` takes the maximum of the captured fields on a base of 8; `Result`/`Enum`/the default all align to 8; `Unit`/`Never` align to 1.
- Who calls it: the same as `typeSize`; it is paired with `typeSize` and passed to the allocating rt functions.
- Who calls it (upper layer): the entire codegen backend; who it calls (lower layer): `lua::layout::*` (`../core/TypeLayout.h`).

## Relationship to Surrounding Files and Pipeline Stages

- It is a type/layout utility implementation belonging to the **code generation stage**.
- It depends on the layout functions of `../core/TypeLayout.h`, such as `valueSize`/`enumPayloadSize`/`productStorageSize`; these functions determine the concrete in-memory layout of Luna values, and this file merely exposes "size/alignment" to the backend.
- It corresponds one-to-one with `CGHelpers.h` (declarations in the former, implementation in the latter). It sits at a lower level and is indirectly depended on by all of Module/Functions/ControlFlow/Expressions/Cleanup/Gpu.

## Further Reading

1. The layout semantics of `../core/TypeLayout.h` (value size / field offsets / enum payload size).
2. `CGHelpers.h` — the corresponding interface declarations and member documentation.
3. `emitLunaDeallocation` in `CodeGeneratorCleanup.cpp` — one of the actual consumers of the size calculation results from this file.

---

---
title: src/codegen/CGHelpers.h
path: src/codegen/CGHelpers.h
stage: Code Generation (CodeGen)
role: LLVM type-mapping helper header file
language: C++
---

# src/codegen/CGHelpers.h — The Single Authoritative Helper Class for Mapping Luna Types to LLVM Types

## What This File Does

`CGHelpers.h` is a lightweight "type utility helper" header file of the code generation stage. It declares the `CGHelpers` class, which translates Luna's frontend type system (`TypePtr`, from `../core/TypeSystem.h`) into LLVM types (`llvm::Type*`), and provides a set of convenience accessors for common scalar LLVM types (i32/i64/f32/f64/bool/void/ptr/size). It also declares two file-level free functions, `typeSize` and `typeAlignment`, which return the byte size and alignment of a Luna type in memory.

For C++ readers: you can think of `CGHelpers` as a "type factory + context holder". It borrows an `llvm::LLVMContext&` (borrowed, never owned); all its methods are read-only queries that emit no IR, so it can be safely shared across the entire backend — `CodeGenerator` holds it via the member `std::unique_ptr<CGHelpers> mHelpers`.

## Key Structs, Classes, and Enums

**class `CGHelpers`**
- Constructor `explicit CGHelpers(llvm::LLVMContext& ctx)`: only stores the passed-in LLVM context reference.
- Private member `llvm::LLVMContext& mCtx`: a borrowed reference to the LLVM global context; all type queries go through it.
- Read-only public interface (inline const): the core is `llvm::Type* toLLVMType(const TypePtr& type) const`; the convenience accessors are `i32Ty()/i64Ty()/f32Ty()/f64Ty()/boolTy()/voidTy()/ptrTy()/sizeTy()` (sizeTy is equivalent to i64Ty on 64-bit targets, and ptrTy is the generic pointer in addrspace 0); there is also the non-const `context()`, which returns the context reference.

**Free functions (defined in CGHelpers.cpp)**
- `uint64_t typeSize(const TypePtr&)`: returns the byte size of the given Luna type.
- `uint64_t typeAlignment(const TypePtr&)`: returns the alignment requirement of the given Luna type.

## Key Functions and Methods

**`llvm::Type* CGHelpers::toLLVMType(const TypePtr& type) const`**
What it does: dispatches on `type->kind` and maps a Luna type to an LLVM type. Key points:
- Integers/floats/bools map directly: I8 to i8, U32 to i32, F32 to f32, Bool to i1.
- `String`/`CStr`/`RawPointer`/`DeviceBuffer`/`Metadata`/`MetadataView`/`DeclarationView`/`DeclarationRef`/`Iterator` all map to the generic pointer `ptrTy()`.
- `Result` and `Enum` map to a tagged structure `{ tag, [N x i64] }` (tag is i32 or bool; the payload is padded to 8-byte words per `lua::layout::valueSize`/`enumPayloadSize`).
- `Array` maps to `[len x inner]`; `Slice` maps to `{ptr, i64}`; `Record` builds a `StructType` recursively field by field; `Closure` maps to `{code_ptr, captured...}`; `Reference`/`Struct`/`Function` map to pointers; `Unit`/`Never` map to void; `Event` maps to i32; unknown kinds fall back to i32.
- Who calls it: throughout codegen — `declareFunc` in `CodeGeneratorModule.cpp`, `CodeGeneratorFunctions.cpp`, `CodeGeneratorControlFlow.cpp`, `CodeGeneratorExpressions.cpp`, etc.
- Who it calls: at the bottom it calls LLVM type constructors (`llvm::Type::get*Ty`, `StructType::get`, `ArrayType::get`, `PointerType::get`).

**`typeSize` / `typeAlignment` (free functions)**
Base types return constants directly; `Record`/`Struct`/`Closure`/`Enum`/`Result` defer to `lua::layout::*`; `Array` recurses, multiplying by the element count. They are called by `CodeGeneratorCleanup.cpp` (the deallocation arguments), `CodeGeneratorControlFlow.cpp` (the size/align arguments of rt_alloc/rt_dealloc), and `CodeGeneratorExpressions.cpp` (`generateHeapAlloc`).

## Relationship to Surrounding Files and Pipeline Stages

- It is low-level infrastructure of the **code generation (Codegen) stage**.
- It depends on `../core/TypeSystem.h` (TypePtr, TypeKind) and `../core/TypeLayout.h` (lua::layout layout functions).
- It is used by `CodeGenerator.h/.cpp` (held through the mHelpers member) and upstream consumers such as Module/Functions/ControlFlow/Expressions/Cleanup.
- Unlike `CodeGeneratorRangeAnalysis` and `CodeGeneratorGpu`: it only does type translation and does not emit IR instructions.

## Further Reading

1. Type layout and sizes: `../core/TypeLayout.h` / `.cpp` (valueSize, productFieldOffset, variantFieldOffset, enumPayloadSize).
2. `CodeGenerator.h` — how CodeGenerator holds mHelpers and orchestrates the backend.
3. LLVM documentation: `llvm::Type`, `llvm::StructType`, `llvm::ArrayType`, `llvm::PointerType`.

---

---
title: src/codegen/CodeGenerator.cpp
path: src/codegen/CodeGenerator.cpp
stage: Code Generation (CodeGen)
role: Implementation of shared small utility methods for CodeGenerator
language: C++
---
