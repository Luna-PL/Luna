# src/moonir/Lowering.cpp

Complete implementation of LunaLowerer: recursively lowers the frontend (AST + SymbolTable) into a MoonIR Module, including type sealing, declaration-reference resolution, and module import/export construction.

## What This File Does

Builds the module model in a single pass:

1. lower() initializes the Module (package name, packageUses, features) and calls lowerDecl on each declaration;
2. when needed, appends a canonical declaration row to the declaration table for the compiler built-in Drop/From traits (traits absent from source are still presented as rows);
3. iterates over kernel declarations, deciding isCodegenReachable based on --reserve-kernel-runtime or on whether they are referenced, and records the kernel cost;
4. sealTypeTable() (unifies identity/layout) → resolveDeclarationReferences() (resolves deferred references into symbol + contract) → buildModuleInterfaces() (imports/exports).

## Key Functions and Methods

| Function | Purpose |
| --- | --- |
| lower() | Top-level flow: build Module, lower declarations, inject built-in traits, compute kernel cost, seal, resolve, buildInterfaces |
| lowerDecl/lowerFunction/lowerStmt/lowerExpr/lowerBlock | Lowering of each construct. |
| lowerCommonDeclaration | Fills shared declaration fields (packageId/identity/sysmeta, etc.). |
| deferDeclarationRef / resolveDeclarationReferences | Defers declaration references and resolves them uniformly at the end of the stage. |
| buildModuleInterfaces | Generates imports (package/host) and exports, and sorts them. |
| lowerRetention/lowerOperator/lowerParam | Retention/operator/parameter mapping. |
| inferredExprType / addDeclarationRecord | Type inference; registers canonical declaration records. |

## Relationship to Surrounding Files and Pipeline Stages

- Upstream: the Program + SymbolTable from parser/sema.
- Downstream: produces the MoonIR Module, handed to Sealer/ControlFlowBuilder/Verifier up through containerization.
- Stage: the first concrete output from frontend semantics → MoonIR.

## Further Reading
- src/moonir/Lowering.h: declares the interface.
- src/moonir/MoonIR.h: Module/DeclarationRecord definitions.
- src/moonir/Sealer.cpp: seals function bodies after lowering.


---

---
title: LunaLowerer — the lowering interface from frontend AST to MoonIR Module
file: src/moonir/Lowering.h
namespace: moon
stage: Frontend → MoonIR lowering (frontend lowering)
---

# src/moonir/Lowering.h

Declares LunaLowerer: the lowerer that compiles the frontend's semantically analyzed Program (AST + SymbolTable) into a MoonIR Module in a single pass.

## What This File Does

Luna's translation entry point from the "source AST" to a "verifiable/serializable MoonIR Module". Its job is to bring types, declarations, expressions, statements, and functions one by one into MoonIR's naming, collect declaration references that need deferred resolution (PendingDeclarationRef), then resolve them uniformly at the end of the stage and build the module interface (imports/exports).

- Output: std::unique_ptr<Module> (types and the declaration table already registered, so the CFG can subsequently be built by the Sealer).
- Errors: accumulated into mErrors (diagnostic::Diagnostic) for the diagnostics layer.

Analogy for C++ readers: a suite of recursive translation functions from the grammar (AST classes) to the intermediate representation (IR classes), backed by a symbol table of pending resolutions (similar to deferred name resolution).

## Key Structs and Classes

| Member | Meaning |
| --- | --- |
| class LunaLowerer | The lowerer; lower() is the main entry point. |
| struct PendingDeclarationRef | A declaration reference resolved later: the DeclarationRef* target to be written + the lookup name + the originating AST node. |

State members: mProgram/mSymbols/mModule pointers, mReserveKernelRuntime, mRequiredKernelSymbols, mPendingDeclarationRefs, mErrors.

## Key Functions and Methods

| Function | Purpose |
| --- | --- |
| lower(program, symbols, reserveKernelRuntime) | Top-level entry: build Module, lower each declaration, inject the built-in Drop/From trait rows, handle kernel reachability, sealTypeTable, resolveReferences, buildInterfaces. |
| lowerType/lowerExpr/lowerStmt/lowerBlock/lowerDecl/lowerFunction | Lowering of the various language-level constructs. |
| lowerParam/lowerOperator/lowerRetention | Mapping of parameter/operator/retention semantics. |
| addDeclarationRecord | Writes a canonical DeclarationRecord for the lowered Decl. |
| deferDeclarationRef/resolveDeclarationReferences | Defers and uniformly resolves declaration references (including built-in traits looked up byId, etc.). |
| buildModuleInterfaces | Fills module->imports/exports (package imports, host imports, exports), and sorts them. |
| inferredExprType / typeRef / typeRefs | Type inference and TypeRef transcription helpers.
| error | Records a diagnostic with source position. |

## Relationship to Surrounding Files and Pipeline Stages

- Upstream: the Program + SymbolTable produced by parser/sema.
- Downstream: produces the Module, handed to the Sealer (which builds the CFG and verifies).
- Dependencies: core/TypeSystem, MoonIR.h, diagnostics.
- Stage: the first link mapping frontend semantics → MoonIR.

## Further Reading

- src/moonir/Lowering.cpp: the full implementation.
- src/moonir/MoonIR.h: Module and its structures.
- src/moonir/Sealer.cpp: how function bodies are sealed after lowering.


---

---
title: MoonIR core execution: type registration, sealing, lookup, and materialization
file: src/moonir/MoonIR.cpp
namespace: moon
stage: Frontend lowering output / backend and runtime input
---
