# src/moonir/Verifier.cpp

Full implementation of Verifier.h (about 3600 lines): runs hundreds of frozen invariant checks against Module and ControlFlowGraph; any violation is recorded as a diagnostic and the check returns false.

## What This File Does

Two entry points:

**verify(Module)** (starting around line 530):
- format version, module name, sourceModule deduplication, no conflicting packageUses aliases;
- type table: field completeness for each TypeRecord (id/kind/domain/identity/sysmeta), shape/abiLayout consistency, referencedTypeIds references resolvable;
- declaration table: symbolId/contractId/type references valid for each DeclarationRecord, DeclarationRef resolvable across tables, field names match types;
- import/export/declaration symbol table consistency, extern and entry point constraints.

**verify(CFG, module)** (starting around line 289):
- requires that both the typeTable and the graph are sealed;
- verifyCanonicalTables: complete indices for the blocks/regions/scopes/locals/cleanups tables, acyclic parent/scope chains;
- verifyRegions: region hierarchy, root anchors, block ownership (a block's region and scope match);
- then validates each block: stmt operand types, Expr type references, cleanup actions matching local types, and terminator jump targets having valid target blocks;
- recursive verifyStmt/verifyExpr covers all node types.

## Key Functions and Methods

| Function | Purpose |
| --- | --- |
| verify(const Module&) | 500+ checks across the whole module. |
| verify(const CFG&, const Module&) | Structure/type/cleanup checks for a single graph. |
| verifyDeclaration/Function/Block/Stmt/Expr | Recursive descent. |
| verifyCanonicalTables / verifyRegions | CFG table completeness and region structure. |
| verifyCleanupAction / verifyType / verifyDeclarationRef | Simple semantic checks. |

## Relationship to Surrounding Files and Pipeline Stages

- Called by the Sealer (before sealing), ContainerModel (before and after containerization), and the loading path.
- Reads all structures in MoonIR.h; depends on core/ownership, core/types, and diagnostics.
- Pipeline: an invariant gate spanning sealing, containerization, and loading.

## Further Reading

- Interface: src/moonir/Verifier.h.
- Called by: src/moonir/Sealer.cpp, src/moonir/ContainerModel.cpp.
- Structures: src/moonir/MoonIR.h.


---

---
title: Verifier Interface: MoonIR's Integrity Verifier
file: src/moonir/Verifier.h
namespace: moon
stage: gate validation before sealing/containerization/loading
---

# src/moonir/Verifier.h

Declares MoonIR's verifier: validates whether a Module or a ControlFlowGraph satisfies all frozen invariants, reporting diagnostics on failure.

## What This File Does

Provides two verification entry points: verify(Module) for whole-module validation, verify(graph, module) for single-graph validation, plus an errors() accessor. The private method families decompose the various checks:

- Level-by-level validation of declarations/functions/blocks/statements/expressions (verifyDeclaration/Function/Block/Stmt/Expr);
- Type and reference validation (verifyType/verifyDeclarationRef);
- Cleanup action validation (verifyCleanupAction) and CFG structure validation (verifyCanonicalTables/verifyRegions).

This "validate before publishing" discipline makes it a common gate for containerization, loading, and sealing.

## Key Structs and Classes

| Class | Purpose |
| --- | --- |
| class Verifier | verify() ×2, errors(), and the private *verify* family. |

## Key Functions and Methods

| Function | Purpose |
| --- | --- |
| verify(const Module&) | Whole-module validation (type table, declaration table, resolvable type references, sysmeta consistency, etc.). |
| verify(const ControlFlowGraph&, const Module&) | Graph structure validation (region structure, block ownership, cleanup invariants). |
| verifyDeclaration/Function/Block/Stmt/Expr | Recursive routine decomposition. |
| verifyCanonicalTables / verifyRegions | CFG canonical tables and region/block ownership structure checks. |
| verifyCleanupAction / verifyType / verifyDeclarationRef | Single-responsibility local checks. |
| error(location, message) | Records a diagnostic into mErrors. |

## Relationship to Surrounding Files and Pipeline Stages

- Called by the Sealer (validates the graph before sealing), ContainerModel (validates the Module before and after containerization), and the downstream loading path.
- Depends on the MoonIR.h structures and diagnostics::Diagnostic; involves core/ownership and core/types.

## Further Reading

- Implementation: src/moonir/Verifier.cpp.
- Called by: src/moonir/Sealer.cpp, src/moonir/ContainerModel.cpp.
- Structures: src/moonir/MoonIR.h.


---
