# src/codegen/CodeGeneratorRuntimeDescriptors.cpp — LLVM IR Generation for the Runtime Declaration Descriptors and Registry

## What This File Does

Implements `CodeGenerator::emitRuntimeDescriptors()` — iterates over `mProgram->declarationTable` and, for every runtime-visible declaration, emits a "declaration descriptor" as an LLVM global constant (a `moon.declaration.descriptor` struct) containing the declaration ID, family ID, linkage name, kind, retention, metadata array, and entry function pointer; places the descriptor array and the registry in the appropriate segment (which differs between Mach-O, COFF, and ELF); and finally calls `llvm::appendToCompilerUsed` to prevent GC from discarding them. Returns immediately when `mProgram->features.runtime` is false.

> For C++ readers: this is the generator of "runtime reflection metadata". It turns Luna's compile-time-known declaration list into C-compatible global structs, embeds them into dedicated segments such as ELF `.moon.runtime.descriptor`, and makes them enumerable by a future MoonRuntime loader. The essence of this technique is a "compiler-generated self-describing segment".

## Key Structs, Classes, and Enums

In the anonymous namespace:
- `struct MoonRuntimeSectionNames`: contains two `const char*` members, `descriptors` and `registry`, used to select section names per target format.
- `MoonRuntimeSectionNames moonRuntimeSectionNames()`: dispatches on the format determined by `llvm::Triple(getProcessTriple())`: for Mach-O returns `{"__DATA,__moon_desc", "__DATA,__moon_registry"}`; for COFF returns `{".moon$D", ".moon$R"}` (the `$` suffix is the canonical COFF subsection naming convention); by default (ELF) returns `{".moon.runtime.descriptor", ".moon.runtime.registry"}`.
- `uint64_t stableRuntimeId(const string& text)`: an FNV-1a 64-bit hash that produces stable IDs used for naming globals.

LLVM struct types (created dynamically inside `emitRuntimeDescriptors`): `moon.metadata.value` (`{i8, i64, ptr}`), `moon.metadata.instance` (`{ptr, i64, ptr, i8}`), `moon.declaration.descriptor` (`{i32, ptr, ptr, ptr, i8, i8, i64, ptr, ptr}`).

## Key Functions and Methods

**`void CodeGenerator::emitRuntimeDescriptors()`**
- Precondition: returns immediately when `mProgram==nullptr || !mProgram->features.runtime`.
- Creates the three LLVM struct types (metadata value/instance and declaration descriptor).
- A local `cString(text)` closure: for each string constant, creates a `GlobalVariable` (`ConstantDataArray::getString`, read-only with `true`, `PrivateLinkage`, named `__moon_string_<hash>`, `UnnamedAddr::Global`) and returns a constant i8* expression obtained by GEP-ing to the first character. Results are cached in an `unordered_map`.
- Iterates over `mProgram->declarationTable`: for each `record`, filters out declarations whose retention is `CompileTime` and which carry no runtime metadata. Retained metadata (`retention != CompileTime`) is expanded into a `moon.metadata.instance` array, and each instance's values are expanded into a `moon.metadata.value` array (distinguishing four payload kinds: integer/float/boolean/string).
- Builds the descriptor: `{version=1, id, familyId, linkageName, kind, retention, metadataCount, metadataPointer, entry}`, where `entry` is taken from `mFunctions[linkageName]` if present, otherwise null.
- Names the InternalLinkage global constant `__moon_descriptor_<hash>` and calls `setSection(runtimeSections.descriptors)`.
- Collects all descriptor pointers into a `descriptorPointers` array, builds the registry struct `{count, [descriptorPointers]}`, names it `__moon_runtime_registry_<hash>` with `ExternalLinkage`, and calls `setSection(runtimeSections.registry)`.
- Finally calls `llvm::appendToCompilerUsed(*mModule, retainedGlobals)` to prevent GC from discarding them.
- Callers: `generate()` in `CodeGeneratorModule.cpp`, between Pass1 and Pass2. Callees: LLVM's Module/GlobalVariable/ConstantExpr constructors.

## Relationship to Surrounding Files and Pipeline Stages

- A metadata-emission submodule of the **code generation stage**, invoked at an intermediate step of the `generate()` main flow.
- Depends on `CodeGenerator.h`, `../moonir/MoonIR.h` (mProgram's `declarationTable`, `MetadataInstance`, and the `Retention` enum), and LLVM's ModuleUtils (`appendToCompilerUsed`).
- Called by `CodeGeneratorModule.cpp`; it does not itself call other codegen implementation files.

## Further Reading

1. `generate()` in `CodeGeneratorModule.cpp` — where this function is called.
2. `DeclarationRecord`, `MetadataInstance`, and `Retention` in `../moonir/MoonIR.h`.
3. LLVM's Section/GlobalVariable documentation, and `appendToCompilerUsed`.

---
