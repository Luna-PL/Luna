# src/moonir/ContainerModel.cpp

Implements every encode/decode method declared in ContainerModel.h: it uses Encoder/Decoder state machines to translate MoonIR's Module/declaration tables/CFG into bytes across the 8 sections, and performs the verify-then-publish atomic load.

## What This File Does

This is the **primary implementation** of `.mooncontainer` serialization. It breaks the in-memory MoonIR model (type tables, declaration tables, sysmeta facts, imports/exports, control-flow graphs) into sections — Type/Manifest/Symbol/Contract/Imports/Exports/Sysmeta/Code — for writing, or reads them back out in reverse.

Core disciplines (woven through the file's comments):

1. **Bounds are failure**: every string/table/enum/length is constrained by ContainerLimits; out-of-bounds, illegal UTF-8, truncation, or enum out-of-range is rejected unconditionally — never trimmed.
2. **Atomic publishing**: decodeDeclarations/decodeCode/decodeContainer first decode the whole payload into temporary objects and run them through the Verifier; only after every step succeeds do they move the result into the target. Any mid-way failure leaves no half-built Module/Manifest behind.
3. **Concrete projection**: the container never carries uninstantiated generic templates/selectors or other recipes; both encodeContainer and decodeContainer first build a concrete projection that carries no generic recipe.

- C++ analogy: a narrow, type-safe serialization library (akin to a Google protobuf writer/reader), but with validate-before-serialize and zero-side-effect-on-failure as disciplines.

## Key Structs and Classes

| Member | Meaning |
| --- | --- |
| anon::Encoder | Forward byte writer: u32/u64/i64/boolean/enumeration/string/rows, all with limit and UTF-8 checks performed first. |
| anon::Decoder | Forward byte reader: u32/u64/boolean/enumeration/string/rowCount, all with truncation and range defenses. |
| buildConcreteProjection | Concretizes the Module into a projection that contains no generic recipe. |
| collectGraphReferences / collectFunctionReferences | Computes function/graph reference edges used for concrete-projection propagation. |

## Key Functions and Methods

| Function | Purpose |
| --- | --- |
| codeOperationOpcode / codeExpressionOpcode | Maps structured Stmt/Expr onto frozen opcodes (0 is illegal). |
| encodeTypes/decodeTypes ... each encodeXxx | One encode/decode method per section, internally using the Encoder/Decoder rows/string families. |
| decodeDeclarations | Merges the Symbol/Contract/Sysmeta sections into the Module, keyed by SymbolId. |
| decodeCode | Decodes all function rows into the declaration table first, then atomically replaces the executable declarations only after all succeed. |
| encodeContainer | Validates the Module, builds the concrete projection, validates the entry point, appends section by section, and hands off to ContainerWriter::encode to finish. |
| decodeContainer | Parses the envelope → decodes section by section → builds the concrete projection → validates the entry point is executable → runs the whole payload through the Verifier → only then publishes. |
| decodeContainerForTarget | Additionally binds expectedTargetTriple/dataLayout; any mismatch fails. |

## Relationship to Surrounding Files and Pipeline Stages

- Container.h: provides the outer ContainerSection/Writer/Reader envelope and ContainerLimits.
- ContainerModel.h: the interface definitions (including the 8-section contracts such as encodeManifest).
- Verifier.h: this file calls Verifier::verify in several places as the publish gate.
- Pipeline stage: sits after Lowering, Sealer, and Verifier; it is the semantic serialization layer for final freeze/load.

## Further Reading

- src/moonir/Container.h: the byte envelope.
- src/moonir/MoonIR.h: the serialized data such as TypeRecord, DeclarationRecord, and ControlFlowGraph.
- src/moonir/Verifier.h: model validation before loading/freezing.


---

---
title: Interface definitions for per-section encoding/decoding of the Moon container model
file: src/moonir/ContainerModel.h
namespace: moon
stage: MoonIR serialization / Container semantic layer
---

# src/moonir/ContainerModel.h

Defines the semantic model of "what the container holds": the Manifest, the encode/decode contracts for the 8 sections, and the ContainerModelCodec static interface that composes a whole Module and its manifest into a single verified container.

## What This File Does

Container.h handles only the outer envelope; this file governs **what each of the 8 sections inside the envelope holds**. It provides, as a family of static methods, the window onto a complete, Verifier-passing .mooncontainer:

- Defines the Manifest structure: package identity, target triple, entry point, feature flags, and other target-relevant facts.
- Provides the encode/decode contract for each of the 8 required sections (Type/Symbol/Contract/Imports/Exports/Sysmeta/Code, plus Manifest).
- Provides encode/decodeContainer to compose them into one whole, and decodeContainerForTarget for loading.

Unlike Container.cpp's "byte-level envelope", this file governs the "semantic-level patch". The interface comments stress repeatedly: no partial state is published until decoding completes (atomicity), and the whole model must pass the Verifier before it can be published.

- C++ reader analogy: a static codec toolkit, akin to protobuf's Message serialization entry point, but deliberately front-loading validation.

## Key Structs, Classes, and Enums

| Name | Meaning |
| --- | --- |
| enum ContainerPackageKind | Application=1 / Library=2 package kind, which constrains the entrypoint. |
| enum CodeOperationOpcode | The "operation" tags of the Code section: Let/Allocate/Expression/Free/Await (0 reserved as illegal). |
| enum CodeExpressionOpcode | The full set of expression tags for the Code section: integer/float/string/call/Move/Borrow, 28 entries in all. |
| struct ContainerManifest | The manifest: packageId, packageVersion, ContainerPackageKind, targetTriple, dataLayout, DeclarationRef entrypoint, FeatureFlags. |
| class ContainerModelCodec | encode/decode for all 8 sections + container compose/decompose; entirely static. |

Helper free functions: codeOperationOpcode(const Stmt&), codeExpressionOpcode(const Expr&) — tag structured Stmt/Expr onto frozen opcode enums (0 reserved as illegal, so a truncated value is never mistaken for an executable one).

## Key Functions and Methods

| Function | Purpose |
| --- | --- |
| encodeManifest / decodeManifest | Encode/decode the Manifest (does not serialize source file paths). |
| encodeTypes / decodeTypes | The type table; decode preserves non-type fields and only replaces the type table and indices. |
| encodeSymbols/Contracts/Sysmeta | Block-wise encoding of the declaration table, contracts, and sysmeta facts. |
| decodeDeclarations | Joins the three sections normalized by SymbolId; on failure it publishes no partial declaration table. |
| encodeImports/Exports / decodeInterfaces | Encode/decode the import/export interface layer and align them. |
| encodeCode / decodeCode | Executable declaration rows; decode atomically replaces them only after all function rows are successfully added. |
| encodeContainer / decodeContainer | Compose the 8 sections into a verified container; decode publishes nothing throughout until it passes the Verifier. |
| decodeContainerForTarget | Additionally binds the expected targetTriple/dataLayout; on mismatch it publishes nothing (for manifest-driven loading). |

## Relationship to Surrounding Files and Pipeline Stages

- Container.h domain mirror: provides the outer ContainerSection/Writer/Reader and ContainerLimits.
- ContainerModel.cpp: the concrete byte-encoding implementation of these interfaces.
- Verifier.h: used before and after encodeContainer/decodeContainer to validate the whole module — verify before publishing.
- Pipeline stage: sits after Lowering and Sealer, forming the semantic entry for .mooncontainer persistence/loading; decodeContainerForTarget is the runtime load path.

## Further Reading

- src/moonir/Container.cpp: the byte-level (envelope) implementation of the 8 sections.
- src/moonir/MoonIR.h: definitions of the serialized Module/DeclarationRecord/ControlFlowGraph.
- src/moonir/Verifier.h: model integrity validation.


---

---
title: ControlFlowBuilder implementation: expansion from structured to normalized CFG
file: src/moonir/ControlFlowBuilder.cpp
namespace: moon
stage: CFG construction before MoonIR sealing
---
