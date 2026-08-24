# src/moonir/MoonIR.cpp

Implements the hardest parts declared in MoonIR.h: type registration and sealing (identity/layout canonicalization), module index rebuilding, the various find queries, and TypeMaterializer's recursive reconstruction of the Type graph.

## What This File Does

Besides the various Name() string mappings and isCompilerIntrinsicName (declared in MoonIR.h), this file is also responsible for:

- **registerType**: freezes a frontend Type object into a TypeRecord, computes the canonical type/shape/ABI layout strings and the identity hash, deduplicates, merges forward placeholders with completed records of the same ID, and recursively registers referenced subtypes.
- **sealTypeTable**: sorts the type table, re-materializes every record with TypeMaterializer to verify it (TypeId must not drift), recomputes shapes/layouts/resource contracts, and rewrites contract/identity.
- **rebuildIndexes**: rebuilds the declaration/type lookup maps from the data sequence — this is the boundary where "rebuilding is updating"; a deserialized module must not carry stale frontend pointers.
- **TypeMaterializer::materialize**: recursively rebuilds the Type graph from canonical records.

- Analogy for C++ readers: this is like a deep-copy/shape-stabilizing facility: first register (pointer→record), then "seal" to harden consistency, and finally the backend reconstructs the object graph with the materializer.

## Key Functions and Methods

| Function | Purpose |
| --- | --- |
| Module::registerType(const TypePtr&) | Freezes into a TypeRecord; merges duplicates away; recursively registers subtypes; throws logic_error if the table is already sealed. |
| Module::sealTypeTable() | Sorts + re-materializes for verification + recomputes identity/layout/contract; throws logic_error on TypeId drift or conflicting payload. |
| Module::rebuildIndexes() | Rebuilds the lookup indexes for types, declarationRecords, functions, fragments, etc. |
| Module::findType/findDeclaration(by symbol/ref/byId/byLinkage) | Multiple identity-based lookups for declarations/types; returns nullptr if not found. |
| TypeMaterializer::materialize(TypeRef) | Recursively rebuilds a Type (memoized); skeleton-first so recursive nominals are supported. |
| canonicalAbiLayout/canonicalContract | Produce stable strings prefixed with luna.abi-layout.v1 / luna.contract.v1. |
| isCompilerIntrinsicName | Determines whether a name is a compiler intrinsic (a long list including print, panic, type_of, is_ok, ...). |

## Relationship to Surrounding Files and Pipeline Stages

- Upstream: Lowering calls into it to produce/populate the Module and Types.
- Downstream: ControlFlowBuilder requires the typeTable to be sealed; ContainerModel serializes; Printer prints; Verifier validates.
- Stage: pivotal in the mid-construction phase — the type table is sealed as soon as registration completes, and all subsequent CFG construction and containerization operate against the frozen table.

## Further Reading

- src/moonir/MoonIR.h: data structure definitions.
- src/moonir/Lowering.cpp: lowers the frontend AST into a Module.
- src/moonir/Verifier.h: integrity checks on the frozen, sealed table.
- src/core/TypeSystem.h: the underlying capabilities for type identity/layout.


---

---
title: MoonIR — Core Definitions of the Luna Intermediate Representation
file: src/moonir/MoonIR.h
namespace: moon
stage: Frontend Lowering output / Backend and runtime input
---

# src/moonir/MoonIR.h

The Luna compiler's core intermediate representation: it defines the "frozen, pointer-free, serializable" Module, type table, declaration table, control-flow graph, and all of their node types.

## What This File Does

This is MoonIR's "constitution". It defines that every executable reference is a "stable table reference" (SymbolId/ContractId/TableRef) rather than a frontend object pointer; it defines the record structures for types and declarations; and it defines the dichotomy between the structured construction input (body) and the canonicalized CFG (controlFlow). Nearly every other moonir component (Lowering/ControlFlowBuilder/Verifier/ContainerModel/Printer/Sealer/Optimizer) builds on it as its type foundation.

Several design principles run throughout:

- **Stable references over pointers**: TypeRef=TypeId, SymbolRef=SymbolId, BlockId/RegionId/ScopeId/LocalId/CleanupId=TableRef. A null reference expresses "absent" and never points to an "unresolved object".
- **Frozen and pointer-free**: a TypeRecord is a complete, pointer-free description, accompanied by a referencedTypeIds traversal index, so an independent backend can rebuild the type graph without frontend Type pointers.
- **The two body forms never coexist**: a FunctionDecl carries either the construction-time body or the post-seal controlFlow, never both (Sealer handles the switch).
- **Non-SSA, local-oriented**: the CFG is based on a LocalId table rather than SSA value numbering; the table index is the serialization identity.

## Key Structs, Classes, and Enums (Essential)

| Name | Meaning |
| --- | --- |
| TableRef<Tag> and the Block/Region/Scope/Local/Cleanup Ids | Generic stable IDs based on uint32 indexes; InvalidTableIndex denotes "absent". |
| struct DeclarationRef | Dual alias of symbol + contract; the minimal identity of an executable reference. |
| struct TypeRecord | The complete type payload: kind/domain/identity, fields/params/return, abiLayout, sysmeta, referencedTypeIds. |
| struct DeclarationRecord | All key information about a declaration: type reference, contract, sysmeta, dropGlue, etc. |
| struct ControlFlowGraph | The five tables for blocks/regions/scopes/locals/cleanups, plus the sealed flag and find* queries. |
| struct Terminator/BasicBlock | Terminators (Jump/Branch/Switch/Return/Resume/Abort...) and basic blocks. |
| struct Module | The top level: types/declarations/imports/exports/Costs, plus index maps and register/seal/find methods. |
| class TypeMaterializer | Rebuilds the Type graph from canonical records; its caches live outside the Module. |

Enum families: RegionKind, LocalKind, CleanupKind, TerminatorKind, ProjectionKind, DeclarationKind, FragmentKind/Cardinality, Operator, CostKind, ImportKind, Retention, ContinuationKind.

## Key Functions and Methods

| Function/Method | Purpose |
| --- | --- |
| Module::registerType / sealTypeTable | Registers types (deduplicating, preserving structure, collisions are errors); after sealing, sorts and canonicalizes identity/layout. |
| Module::rebuildIndexes / findDecl/findType/findDeclaration* | Rebuilds the lookup tables; multiple find variants by id/symbol/contract/linkage. |
| ControlFlowGraph::findBlock/findRegion/findScope/findLocal/findCleanup | Fetches the corresponding record by table reference. |
| TypeMaterializer::materialize | Recursively rebuilds the Type graph from canonical records (skeleton-first to support recursive nominals). |
| canonicalAbiLayout / canonicalContract | Produce stable canonical strings (Layout/Contract) used for the identity hash. |
| isCompilerIntrinsicName | Determines whether a name is a compiler intrinsic with no declaration-table row. |
| A family of *Name(enum) functions | Map each enum to a stable string (for debugging/serialization). |

## Relationship to Surrounding Files and Pipeline Stages

- It is the type foundation for the moonir components (Lowering, ControlFlowBuilder, Verifier, Printer, Sealer, Optimizer, ContainerModel, Container).
- Stage: it is the output of frontend Lowering; after CFG construction and Verifier validation, it is ultimately persisted through ContainerModel.

## Further Reading

- Concrete implementation: src/moonir/MoonIR.cpp (canonicalAbiLayout, registerType, find*, TypeMaterializer).
- src/moonir/Lowering.h/.cpp: the entry point that produces this IR.
- src/moonir/ControlFlowBuilder.h: builds the first CFG from a structured body.
- src/moonir/ContainerModel.h: serializes the data defined here into 8 sections.
- ContainerModel.h: serializes the data defined here into 8 sections.

---

---
title: Optimizer implementation (currently canonicalization)
file: src/moonir/Optimizer.cpp
namespace: moon
stage: MoonIR post-processing
---
