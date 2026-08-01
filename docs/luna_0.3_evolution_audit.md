# Luna 0.3 Model Convergence and Slot/Fragment Refactoring Audit

English | [简体中文](luna_0.3_evolution_audit.zh-CN.md)

> Document category: RFC / implementation audit
> Applies to: Luna 0.2.1 as the audited baseline; candidate Luna 0.3.0
> Status: Draft
> Normative status: Non-normative; this document does not change the 0.2.1 contract
> Implementation checked: `8d461d4` (2026-08-01)

## 1. Scope and decision

This read-only audit covered Parser, AST, Sema, Ownership, MoonIR, Verifier, LLVM
lowering, Runtime Descriptors, and the external fragment plugin ABI. It changed
no implementation, test, version, roadmap, or existing specification.

The proposed direction is conditionally approved and is more coherent than the
current 0.2 model:

- converge phases to compile-time and runtime, removing a separate `dynamic`
  phase and source modifier;
- erase static entities by default and materialize only proven or explicit
  runtime needs;
- promote selection from a metadata-specific facility to typed Symbol Query;
- keep slots second-class while making only runtime fragment references values;
- make named types nominal by default while retaining explicit shape relations;
- move `rc`/`arc` from compiler-wide special cases toward Core resource containers.

Implementation must not start as a broad rewrite. The following are hard design
gates:

1. A current slot is a function-local `SlotDeclStmt`, not an exportable module declaration.
2. Current slot and fragment results are fixed to `unit`; `slot (...) -> Response` has no defined control semantics.
3. There is no formal effect contract, only ownership/control facts and scattered capability flags.
4. A current runtime fragment is not a storable typed reference and has no stable internal fragment entry.
5. The plugin contract is an ad-hoc string, not a complete verifiable ContractId.
6. Type, Resource, Query, and Composition should be language axes, not four mutually exclusive root object kinds.
7. Switching the type default is a version-level breaking change and needs a package language version or edition.

The decision is therefore: continue design, but do not begin a large-scale
implementation. The next one or two weeks remain focused on the 0.2.1
ecosystem. Only RFC, migration-corpus, and audit work should prepare 0.3.

## 2. Current implementation audit

### 2.1 AST and parser path

| Layer | Current path | Current fact |
|---|---|---|
| Tokens | `src/lexer/Token.h` | Runtime, Dynamic, Slot, Apply, and Select are separate tokens |
| Declarations | `src/parser/Parser.cpp:122` | retention is CompileTime/Runtime/Dynamic; interceptor/context form fragment declarations |
| Statements | `src/parser/Parser.cpp:520` | slot/apply are statements; dynamic slot/apply have separate parser branches |
| Slot parser | `src/parser/Parser.cpp:556` | requires interceptor/context and once/many; supports declaration or inline captured continuation |
| Apply parser | `src/parser/Parser.cpp:667` | static apply names one fragment; dynamic apply embeds a finite named candidate set |
| AST | `src/parser/AST.h:198` | SlotDeclStmt is a statement, not a Decl |
| AST | `src/parser/AST.h:211` | SlotInvokeStmt embeds its continuation block and dynamic dispatch state |
| AST | `src/parser/AST.h:242` | ApplyStmt stores names, isDynamic, and alternatives |
| AST | `src/parser/AST.h:525` | FragmentDecl is a top-level declaration |

There is no top-level SlotDecl today. A public library cannot export a slot and
the MoonIR declaration table cannot give it a stable package/module identity.

### 2.2 Sema representation

`src/core/TypeSystem.h` contains `TypeKind::Slot` and `TypeKind::Fragment` in the
general Type object. Both carry parameter/result types and ownership contracts,
multi-shot state, continuation kind, and control/resource sysmeta. All actual
constructors currently use a `unit` result. They also remain in the default
Value domain even though the language does not intend them to be ordinary data.

`src/sema/SemanticAnalyzer.h:241` stores slots in a private lexical `SlotInfo`
with a local name, parameters/contracts, default fragment, control category,
implicit-capture state, `isDynamic`, and a structural TypePtr. It has no SlotId,
ContractId, formal effects, visibility, package/module owner, or ABI record.

The second-class restriction is not fully enforced: generic identifier analysis
can return a slot or fragment TypePtr because these entities share the general
symbol table and Value type domain. Backend fallback is not a valid value
representation. Luna 0.3 should forbid this leak rather than materialize slots.

### 2.3 Contract coverage

| Contract field | Current state |
|---|---|
| Identity | local string only; no package/module SlotId |
| Parameters | typed and ownership-checked |
| Result | represented structurally but fixed to Copy Unit |
| Effects | no formal effect set |
| Continuation | interceptor/context, once/many, forwarding, and abort are partially represented |
| ABI | provisional plugin string only |
| Retention | local isDynamic controls dispatch; no runtime slot descriptor |

A complete SlotContract is a 0.3 goal, not a description of the present model.

### 2.4 MoonIR representation

MoonIR remains a typed AST-like IR. `SlotDeclStmt`, `SlotInvokeStmt`, and
`ApplyStmt` mirror the frontend; a SlotInvoke owns a nested BlockStmt
continuation. `FragmentDecl` owns a body, while resume and abort are statement
nodes. There is no typed continuation region, frame contract, entry/exit, or
continuation ABI node. Slots do not enter DeclarationRecord; fragments do.

`src/moonir/Optimizer.cpp` only rebuilds indexes. It performs no handler
composition, inlining, devirtualization, or continuation optimization.

### 2.5 Retention and descriptor generation

The current path is:

```text
Parser RetentionKind {CompileTime, Runtime, Dynamic}
  -> Sema metadata promotion
  -> MoonIR Retention {CompileTime, Runtime, Dynamic}
  -> DeclarationRecord and runtimeRetained sysmeta
  -> module features/costs
  -> LLVM descriptor section and registry
```

Relevant locations are `src/parser/AST.h:21`, `src/parser/Parser.cpp:122`,
`src/sema/SemanticAnalyzer.cpp:558`, `src/moonir/Lowering.cpp:922`,
`src/moonir/Lowering.cpp:980`, and
`src/codegen/CodeGeneratorRuntimeDescriptors.cpp:43`.

The emitted generic descriptor contains identities, kind, retention, retained
metadata, and an optional entry. It does not serialize the full typed contract.
Only ordinary functions are present in the code generator's function map, so a
retained fragment descriptor has a null entry and is not callable.

### 2.6 sysmeta and meta

The current separation is directionally sound. `src/core/SysMeta.h` contains
compiler-authoritative typed facts that users cannot construct or override.
User `meta` declarations are typed schemas with constant, checked attachments.
Selectors can inspect user metadata, declaration identity, and callable
signature.

The missing piece is a unified typed query projection. Symbol kind, visibility,
package/module, and implementation facts are scattered across AST, SymbolTable,
and DeclarationRecord. Facts are also attached to both Type and declarations
without a sufficiently explicit Type/Symbol/Contract/ABI ownership boundary.
Luna 0.3 should preserve the authority split without putting every sysmeta fact
inside Type.

### 2.7 Static apply lowering

The real path is:

```text
ApplyStmt
  -> Sema lexical binding and partial contract checking
  -> per-slot fragment analysis
  -> OwnershipChecker simulates fragment/continuation exits
  -> MoonIR preserves the AST-like nodes
  -> MoonIR optimizer only canonicalizes
  -> LLVM codegen reconstructs apply scopes and composes CFG
  -> LLVM O2/O3 removes temporary artifacts
```

The static path correctly avoids generic runtime dispatch, but composition occurs
too late for MoonIR verification and future Moon Container reuse. It should
become a verified MoonIR-to-MoonIR transformation.

### 2.8 Current runtime fragment capability

Three facilities are currently conflated:

1. `runtime` retention creates a generic declaration descriptor but not a callable fragment reference;
2. dynamic apply embeds a finite set of linked fragments and branches by a runtime-selected name, with every body still statically generated;
3. plugin ABI v1 invokes a C entry that receives explicit argument pointers and returns continue/abort/error.

Plugin v1 is host-only and single-shot, cannot receive a Luna continuation,
cannot resume, capture, or implement context/many, and keeps its library loaded
for process lifetime. Its contract string omits SlotId, ownership, result,
formal effects, layout ABI, and continuation ABI. It is a useful compatibility
adapter, not the proposed RuntimeFragmentRef model.

## 3. Recommended converged model

### 3.1 Keep four language axes with a SymbolRecord spine

```text
SymbolRecord
├── SymbolId, kind, package/module, visibility
├── TypeRef or ContractRef
├── typed sysmeta
├── user meta attachments
└── runtime retention policy

TypeRecord: TypeId, ShapeId, nominal identity, traits, constraints, semantic shape
ResourceContract: relation, usage, cleanup, lifetime, allocator domain
ControlContract: SlotContract, fragment capability, continuation rules
```

Type, Resource, Symbol Query, and Composition remain the four central user-facing
ideas, while Symbol connects facts that are not themselves types.

### 3.2 Type and Resource

Named structs/enums should become nominal by default in a package that explicitly
selects the 0.3 language mode. Anonymous records, callable shapes, shape
constraints, and explicit `same_shape` relations can provide structural use
without immediately adding a `structural` keyword. A 0.2 package must retain its
old TypeIds under a newer compiler.

Relation and usage remain orthogonal and do not change TypeId. Box/Rc/Arc should
be Core nominal containers behind a minimal compiler-recognized resource
protocol. Generic Drop glue, recursive layout, Clone/retain, allocator domain,
thread-safety facts, and a compatibility lowering for `rc new`/`arc new` must
exist before deleting intrinsic TypeKinds.

### 3.3 Symbol Query

Query should produce a typed set before applying an explicit cardinality
terminal:

```text
compile_symbols -> SymbolSet<CompileTime, K, C>
runtime_symbols -> SymbolSet<Runtime, K, C>
terminal         -> all / one / optional / explicitly ordered first
```

Reuse the existing `select` keyword instead of adding `query`. The view chooses
the phase; no dynamic select modifier is required. sysmeta predicates are
compiler-typed, meta predicates are schema-typed, compile-time references erase,
and runtime references must be backed by validated descriptors. Query ordering
must never depend on registration or link order.

### 3.4 Composition and service/provider

Slots/fragments express restricted control and handlers. They should not absorb
ordinary dependency injection, object construction, service lifetime, or global
replacement.

Do not add service/provider keywords. Use trait as the typed service contract,
impl as the provider implementation fact, user `@provider(...)` metadata for
policy, and Symbol Query over compiler-owned `implements<Trait>` sysmeta plus
metadata. A fragment participates only when the provider changes continuation
control. Runtime service references require a separate trait/service ABI and
must not be smuggled through RuntimeFragmentRef.

## 4. Candidate Slot/Fragment boundary

The design must first freeze slot result semantics, control source spelling,
nominal versus structural fragment targeting, a formal effect set, all
resume/abort/return/cleanup interactions, RuntimeFragmentRef ownership, and the
continuation ABI. The initial runtime ABI should support only host-only,
single-shot interceptors.

Recommended entities:

- module-level, second-class SlotDecl with nominal SlotId and complete SlotContract;
- function-body SlotInvoke referencing a fixed SlotId and lexical continuation region;
- static FragmentDecl with no runtime identity by default;
- RuntimeFragmentDescriptor only for retained fragments;
- ordinary RuntimeFragmentRef<S> value holding a compatible descriptor and module lease;
- compiler-builtin apply accepting either a static fragment symbol or runtime fragment reference;
- no ordinary slot value and no transferable RuntimeSlotRef.

The slot name in `RuntimeFragmentRef<request>` is a type-position symbol/contract
projection, not a value.

### 4.1 Minimal RuntimeSlotDescriptor

```text
DescriptorHeader { magic, abi_major, abi_minor, struct_size }

RuntimeSlotDescriptor
├── SlotId
├── ContractId
├── owner ModuleId
├── flags
├── parameter_count and RuntimeParameterContract[]
│   └── TypeId, AbiLayoutId, ownership relation, usage
├── RuntimeResultContract
├── EffectSetId / typed effect bits
├── control form, cardinality, abort and forwarding rules
├── CallAbiId
└── ContinuationAbiId

RuntimeFragmentDescriptor
├── FragmentId
├── target SlotId
├── ContractId
├── stable entry
├── capability/effect flags
├── CallAbiId / ContinuationAbiId
└── owner ModuleId and lifetime policy

RuntimeFragmentRef<S> { descriptor handle, ModuleLease }
```

ContractId must cover parameter/result TypeIds, ownership, effects, control,
cardinality, and ABI projections. SlotId and ContractId are both checked: one
proves target identity, the other compatibility. A short hash alone is not a
trust boundary; loaders must compare or recompute canonical payloads.

### 4.2 Inference versus explicit runtime export

| Scenario | Rule |
|---|---|
| Private closed-world slot with static fragments only | no runtime slot descriptor |
| Private slot receives RuntimeFragmentRef | infer descriptor retention |
| Private slot targeted by a reachable retained fragment/plugin export | infer and record the cost reason |
| Exported/public slot | only explicit runtime slot/export sysmeta creates stable ABI |
| Exported runtime fragment targets public static-only slot | reject instead of silently changing ABI |
| Public/generic API accepts RuntimeFragmentRef | explicitly declare the runtime slot contract |
| Dependency slot | trust descriptor/manifest, never cross-package closed-world inference |

Closed-world inference may reduce private artifacts; it must never decide public
ABI existence.

### 4.3 Unified apply lowering

MoonIR may use distinct verified operations while source keeps one semantic
builtin:

```text
apply.static  SlotId, FragmentId, continuation.region
apply.runtime SlotId, RuntimeFragmentRef, continuation.region
```

Static and runtime forms share SlotContract, ownership, effect, and cleanup
verification. The operand type determines lowering; there is no dynamic source
modifier. Runtime apply validates SlotId, ContractId, and ABI, establishes a
scoped frame, executes the continuation, performs deterministic cleanup, and
releases its module lease. The existing blockless 0.2 form may desugar during
migration but must not evolve into global remove/replace.

## 5. Luna 0.3 delivery boundary

Luna 0.3 should deliver:

1. compile-time/runtime phase model with no Dynamic retention in source or core IR;
2. unified Symbol Catalog and typed query, complete for compile-time symbols;
3. descriptor-backed runtime_symbols typed references;
4. formal sysmeta/meta authority separation;
5. package language version isolation for nominal-by-default types;
6. unified ResourceContract and a compatible Box/Rc/Arc container migration;
7. module SlotDecl, complete SlotContract, SlotId, and ContractId;
8. verified MoonIR static composition with zero runtime retention cost;
9. descriptor v2, RuntimeFragmentRef, and lexical runtime apply;
10. a stable host-only single-shot interceptor runtime boundary;
11. diagnostics and migration for 0.2 dynamic syntax;
12. JIT/AOT, plugin, ownership, cost, and ABI regression gates.

The following are not 0.3 release gates: complete Moon Container optimization,
external contexts or multi-shot continuation ABI, global weaving/replacement,
arbitrary native pointers, full unload/reload policy, service/provider keywords,
general runtime trait objects, or open runtime reflection over every symbol kind.
Slots never become ordinary values.

## 6. Before and after

| Dimension | Current 0.2.1 | Candidate 0.3.0 |
|---|---|---|
| Phases | CompileTime/Runtime/Dynamic retention | Compile-time/Runtime; values, descriptors, and builtins determine runtime behavior |
| Type default | named structs/enums structural | named structs/enums nominal; shape relations on demand |
| Rc/Arc | parser/Sema/TypeKind/codegen intrinsics | Core nominal containers plus resource protocol |
| Query | function family and metadata selector | typed symbol sets over all symbol kinds |
| Cardinality | selector must return one | explicit one/optional/all |
| sysmeta | typed but scattered and mostly internal | queryable typed Symbol/Type/Contract/ABI projections |
| meta | user schemas mainly for selector | user extension only, never a safety contract |
| Slot | local statement, local name, unit result | module symbol and full contract, still second-class |
| Fragment | static expansion; retention is not callable | static handler plus separate runtime materialization |
| Static apply | LLVM codegen performs composition | verified MoonIR composition pass |
| Runtime apply | finite linked names and branches | typed RuntimeFragmentRef and scoped handler frame |
| Plugin | v1 C interceptor, process lifetime | v1 adapter; v2 descriptor/ref/lease; context later |
| Descriptor | incomplete generic declaration record | common header plus kind-specific typed descriptors |
| Moon Container | not implemented | separate follow-up consuming verified MoonIR/descriptors |

## 7. Reversible implementation sequence

Each group must be independently reviewable and keep the previous path until
parity tests pass.

- **R0, design freeze:** decide all control, effect, lifetime, identity, and language-version questions; documentation only.
- **R1, shadow identities/contracts:** derive SlotId/ContractId/SlotContract alongside current fields and verify canonical payloads; no behavior change.
- **R2, second-class enforcement:** prevent Slot/Fragment contracts from entering ordinary values; add RuntimeFragmentRef as a separate value kind without execution.
- **R3, MoonIR static composition:** add SlotDecl records, continuation regions, static apply ops, and a composition pass; run old/new codegen in parity.
- **R4, compile_symbols:** build a unified catalog and typed sets; desugar old static select; preserve full erasure.
- **R5, language version and nominal default:** keep 0.2 structural behavior under 0.2 mode and enable nominal default only under explicit 0.3 mode.
- **R6, resource bridge:** introduce Core Box/Rc/Arc and desugar old syntax before deleting intrinsic special cases.
- **R7, descriptor v2/runtime_symbols:** emit and validate typed descriptors and module leases without enabling runtime apply; retain v1 in parallel.
- **R8, runtime apply:** initially enable only host-only single-shot interceptors; adapt plugin v1 through the restricted capability.
- **R9, compatibility removal:** remove Dynamic syntax/fields only after the migration window; update baselines and versioned rejection paths.
- **R10, Moon Container:** separately add serialization, loading/reverification, devirtualization, recompilation, deoptimization, versioning, and reclamation.

## 8. Stop conditions

Pause implementation if slot result semantics are not specified; identity and
compatibility IDs remain conflated; RuntimeFragmentRef lacks a module-lifetime
proof; descriptors rely on Type::toString or user metadata for safety; public
slot ABI is inferred from closed-world use; runtime ownership correctness relies
on the current candidate set instead of a contract effect; static apply needs a
runtime descriptor; 0.2 packages change identity without selecting 0.3 mode;
resource containers lack Drop/allocator/thread-safety proofs; or service/provider
work requires first-class slots or global replacement state.

## 9. Near-term 0.2.1 work

For the next one or two weeks, continue ecosystem work and do not implement 0.3.
Only review the open semantic decisions, collect migration fixtures, document
read-only symbol inventory needs for formatter/LSP tooling, and continue 0.2
diagnostics, package, build/test, release, and editor work. Any future 0.3 code
belongs in an explicit later branch or milestone rather than mixed into 0.2 fixes.

The recommended scope for 0.3 is a semantic-kernel and boundary convergence
release, not a simultaneous delivery of a complete Moon Container, general
runtime reflection, and every continuation ABI.
