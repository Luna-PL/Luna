# Luna 0.3 Overall Design Draft

English | [简体中文](luna_0.3_design.zh-CN.md)

> Document category: RFC / overall design
> Applies to: candidate Luna 0.3.0
> Status: Draft
> Normative status: non-normative; this records confirmed directions and pending placeholders and does not change the current 0.2.1 contract
> Implementation audit: design record based on `bf8d73e` (2026-08-09)

This document is the umbrella design for Luna 0.3. The existing
[Slot/Fragment refactoring audit](luna_0.3_evolution_audit.md) is a topic audit governed by
this document, not the complete 0.3 specification.

The document uses these states:

- **Confirmed**: a direction explicitly confirmed by the project owner;
- **Proposed**: a concrete recommendation that is not frozen;
- **TBD-xxx**: a stable placeholder that must be resolved before implementing its capability.

All example syntax is Draft. This document does not claim compiler support until the
implementation, tests, references, and changelog have been updated together.

## 1. Position and fundamental principles

### C001: Language position (Confirmed)

Luna is a static-first, safety-verified, high-performance systems programming language.
Ordinary programs pay only for capabilities they use. The language supports introducing new
implementations and code through explicit runtime capabilities and supports controlled
evolution through Moon Containers and MoonRuntime. Heterogeneous and parallel computation are
native optional execution capabilities, but expanding them is not a 0.3.0 priority.

### C002: An explicit clean break (Confirmed)

Luna 0.3 is an explicitly breaking update from the prerelease 0.2 compiler:

- no `language = "0.2"`, edition, or compatibility mode is added;
- the 0.3 compiler does not retain old syntax, retention, type defaults, or lowering paths;
- users who need to compile 0.2 source use the 0.2 compiler;
- migration uses release notes, migration tables, and an optional separate migration tool,
  rather than two semantic tracks in the main compiler;
- the 0.3 release must explicitly enumerate every breaking change.

The project has not had a formal language release and has no ecosystem that justifies a
permanent compatibility burden. Keeping both semantics now would permanently expand Parser,
Sema, MoonIR, Runtime, test, and documentation surfaces and would conflict with a lightweight,
focused compiler.

### C003: Static first and pay for use (Confirmed)

- work decidable at compile time must not be deferred to runtime;
- compile-time entities are erased by default;
- programs that do not use Runtime, registries, the Moon loader, GPU, or executable-memory
  capabilities must not carry their artifacts or initialization cost;
- safety validation happens at compile, install, load, or binding boundaries, not on ordinary
  call hot paths;
- artifact, symbol, descriptor, and benchmark evidence must explain each cost.

## 2. Phases, sysmeta, and user metadata

### C004: Only compile-time and runtime phases (Confirmed)

0.3 removes independent `dynamic` retention and its source modifier. Runtime discovery,
references, loading, and switching are represented by concrete runtime values, descriptors,
and builtin operations, not by a third language phase. “Dynamic” may remain explanatory prose,
but is not a type domain or retention class.

### C005: No effect mechanism (Confirmed)

Luna does not introduce explicit effect annotations, effect rows, effect sets, or
user-declared effect contracts. Slot/Fragment exists for control flow and extensibility, not
to add an algebraic-effect system to the language. Its implementation may borrow control ideas
from algebraic effects without exposing an effect language mechanism.

The compiler derives the facts needed for safety and lowering and records the public subset as
read-only, strongly typed sysmeta:

- users may at most read or query sysmeta and cannot construct, override, or forge it;
- control, resource, host/device, FFI, runtime retention, suspension, and ABI facts are
  compiler-derived sysmeta, not effects;
- loaders and verifiers check that descriptor/MoonIR sysmeta agrees with code facts;
- user `meta` continues to carry policies such as versions, labels, and routing and does not
  participate in safety decisions;
- host capability is Runtime authorization policy, not a source-language effect system.

`SM001` (Confirmed): sysmeta uses a closed, compiler-owned typed schema with at least identity,
resource, control, ABI/target, and retention namespaces. Fields are classified as
compiler-internal, container-stable, or user-queryable, and artifacts include only facts needed
for verification, loading, and binding. Users cannot extend this schema or project user meta
into safety facts.

## 3. Artifacts and trust boundaries

### C006: One `-t` artifact-target option (Confirmed)

`luna build` uses `-t` (target) to choose an artifact level. `-t` selects output only; it does
not change source semantics and cannot bypass type or ownership checking.

`T001` (Confirmed): `-t native` is the default, with `-t moon` and `-t cffi` as the other
targets. A `cffi` artifact carries no Luna Native trust proof and is treated like any other
Foreign C FFI artifact when Luna consumes it.

`T002` (Confirmed): the package manifest decides application/library kind, while `-t` decides
only the artifact level and does not encode static/shared linkage. `main` and the export surface
participate in validity checks and never silently change `-t`.

`TBD-T003`: freeze suffixes, manifest package-kind spelling, and platform linkage details for
the three targets. These are artifact-format details and do not change the language or trust
semantics above.

### C007: Three trust boundaries (Confirmed)

| Boundary | Position | Source of safety |
|---|---|---|
| Moon Container | Safe / verified | local Moon verifier revalidates the container |
| Luna Native | Trusted | trusted Luna compilation plus proof bound to machine code |
| Foreign C FFI | Unsafe classification (not syntax) | Luna checks only the declared ABI; callers accept implementation risk |

- Luna will not design isolated Native, a Native sandbox, or an IPC execution layer;
- a Luna Native artifact cannot acquire trust through a self-asserted header; proof must bind
  the actual code and data;
- a native library with missing or damaged proof must be rejected by the module loader and
  must not be silently downgraded;
- the same file may still be used explicitly as an unsafe foreign library through C FFI;
- C FFI libraries produced by other languages are unsafe, proven Luna Native is trusted, and
  a locally verified Moon Container is safe.

`TR001` (Confirmed): a Luna Native proof covers loadable code/data digests, exported typed
descriptors, ContractIds, target ABI, compiler identity, and a dynamic foreign-dependency list.
The proof binds descriptions to the actual artifact; trust comes from an installation record or
explicit trust store, not from an artifact's self-assertion. Caches are content-addressed and
cannot survive dependency or trust changes.

`TR002` (Confirmed): Luna has no general `unsafe {}` capable of disabling internal type,
ownership, or Moon verification. An `extern "C"` declaration is itself the Foreign C FFI
boundary: the compiler still checks its declared ABI and static call types but does not claim
to verify the foreign implementation. If a future primitive needs to omit one particular
dynamic check, it uses a narrow `unchecked_*` operation rather than a propagating unsafe scope.
Foreign C FFI results cannot be wrapped as safe `ModuleRef`s. A `-t moon` artifact cannot resolve
arbitrary `extern "C"` symbols: it must reject that dependency or lower it to a manifest-declared
typed import explicitly authorized by host policy. Optimizers treat foreign calls as having
unknown side effects by default and infer no pure, noalias, nothrow, or lifetime guarantees from
user annotations beyond the ABI and explicit foreign contract.

## 4. Types and resources

### C008: Named types are nominal by default (Confirmed)

- named `struct` and `enum` declarations form nominal TypeIds by default;
- traits, metadata schemas, and named runtime contracts always have declaration identity;
- anonymous records, tuples, function shapes, and explicit shape relations may remain
  structural;
- TypeId, ShapeId, AbiLayoutId, and ContractId remain distinct;
- 0.3 has no compiler mode for the 0.2 structural default.

`TY001` (Confirmed): anonymous records, tuples, and function shapes remain structural. Named
types do not convert structurally merely because layouts match. Shape constraints may test
structural relations but never erase TypeId; conversion between named types requires explicit
construction or projection.

`TBD-TY002`: freeze exact source spelling for anonymous records, shape constraints, and explicit
construction/projection.

### C009: Relation and usage remain orthogonal (Confirmed)

Ownership relation continues to describe owned/shared borrow/mutable borrow, while usage
continues to describe Copy/Affine/Linear. `affine` and `linear` modify a binding contract and
do not change TypeId.

### C010: `linear {}` and `affine {}` (Confirmed direction)

Block syntax is pure sugar: variables declared in the block default to the corresponding
usage. Sema fixes the final usage of every binding, and MoonIR carries no usage-block node, so
the construct has no runtime cost.

Draft example:

```luna
linear {
    let transaction = begin_transaction();
    let token = acquire_token();

    affine let cache = rc(data);
}
```

`US001` (Confirmed): explicit overrides are `copy let`, `affine let`, and `linear let`. An
explicit binding contract replaces the block default and may select any usage permitted by the
type/Resource contract, but it cannot weaken that contract's inherent requirement.

`US002` (Confirmed): ordinary nested blocks inherit the current usage default and nested
`linear {}`/`affine {}` blocks override it. The default applies to new local, pattern, and loop
bindings. Borrow relation remains orthogonal to usage, so a borrowed binding receives the
default while still obeying the borrow checker. Lambda and local-function parameters restart
from the Copy default; captures retain the captured binding's existing contract.

### C011: Rc/Arc migrate to containers (Confirmed direction)

`Rc<T>` and `Arc<T>` migrate to ordinary nominal library containers. A minimal Resource/Drop
protocol expresses reference counting, cloning, cleanup, allocator domains, and required
thread-safety facts. The 0.3 compiler removes corresponding TypeKind, Parser, Sema, and
codegen special cases and retains no 0.2 intrinsic lowering.

`RC001` (Confirmed): the core surface is ordinary `Rc::new(value)`/`Arc::new(value)`, while the
prelude may provide equally ordinary `rc(value)`/`arc(value)` functions. Luna adds no
`rc {}`/`arc {}` language syntax.

`RC002` (Confirmed): Rc/Arc handles are Affine by default and ownership duplication requires an
explicit clone. `Weak` is an ordinary library container; programs break cycles with Weak and no
tracing cycle collector is provided. Drop is infallible. Rc uses non-atomic counting and is not
shared across threads; Arc uses atomic counting and requires compiler-derived thread-safety
sysmeta for its payload.

## 5. One-layer MoonIR

### C012: MoonIR is the single backend IR (Confirmed)

```text
source/package
    -> Lexer / Parser
    -> semantic / ownership analysis
    -> MoonIR
    -> MoonIR verification and transformation
    -> LLVM JIT/AOT
```

Luna does not introduce separate MoonHIR and MoonCore IRs. A Moon Container serializes sealed,
canonical MoonIR, and the LLVM backend consumes the same MoonIR.

The one-layer MoonIR must:

- use stable type/symbol/contract table references and never serialize process-local `TypePtr`
  identity;
- exclude frontend lookups, caches, and derived indexes from the format;
- perform composition, canonicalization, and optimization on the same MoonIR;
- permit container emission only for sealed, verified canonical modules;
- let the verifier check types, ownership, cleanup, control, sysmeta, imports/exports, and
  host/device boundaries using only MoonIR and its manifest;
- avoid separate semantic IRs for JIT, AOT, and the Moon loader;
- have one authoritative format version, serializer, parser, and verifier.

### Why two layers were considered, and why the design now uses one

The two-layer proposal addressed an engineering risk: current MoonIR is a trusted in-process,
AST-like structure containing frontend pointers, while an untrusted container needs a
canonical, serializable, independently verifiable representation. Separating the layers can
avoid immediately freezing a compiler-friendly IR as a wire format.

Two layers would also create two node systems, lowerings, verifiers, printers, test matrices,
and a permanent synchronization cost. Luna has not published a Moon format, so this is the
right time to refactor MoonIR itself to serve both the compiler and the container. The
one-layer design better matches the lightweight and focused goals, provided it satisfies the
pointer-free, sealed, and independently verifiable requirements above. Construction-time
objects may exist in a builder, but they must not form a second semantic IR.

`M001` (Confirmed): canonical MoonIR uses CFG basic blocks as its sole execution semantics and
uses explicit lexical region/scope/cleanup tables in that same IR for continuations and resource
boundaries. Region tables are not a second control IR and have no competing execution semantics.

`M002` (Confirmed): a 0.3 Moon Container accepts only fully instantiated MoonIR. Generic recipes
remain on the compiler-input side and do not enter the first untrusted container format.

`M003` (Confirmed): the 0.3 MVP Moon Container is host-specific and its manifest declares the
target triple and data layout. Cross-target portable containers and target-specific device code
are deferred to later format versions.

`M004` (Confirmed): the container is a deterministic sectioned binary with explicit format
version, section lengths, and parser resource limits. Manifest, type, symbol, contract, code,
import/export, and required sysmeta sections are mandatory; debug/source/device data sections
are optional. Content digests cover canonical non-signature sections, and parser/verifier work
requires a fuzz corpus.

`TBD-M005`: freeze wire details such as binary magic, integer encoding, section numbers,
alignment, compression, and signature algorithms without changing the M001-M004 semantic
boundaries.

## 6. Runtime validation and evolution

### C013: Validation stays off ordinary call hot paths (Confirmed principle)

Moon validation, Native proof checks, and ContractId/ABI matching happen at install, load, or
binding construction. Once binding succeeds, calls use direct calls or the declared minimum
indirection and do not revalidate types, ownership, or container signatures per call.

`V001` (Confirmed): structural-verification cache keys include content digest, verifier version,
target, and validation policy. Import/ContractId/ABI matching against a generation is checked and
cached separately when a binding is established; ordinary calls do not recheck it. Any key,
trust, or dependency-generation change invalidates the relevant cache.

### C014: MoonRuntime owns evolution (Confirmed direction)

MoonRuntime introduces implementations and code from Moon Containers and trusted Luna Native
and manages evolution. Safe updates require at least module/content/generation identities,
staging, validation, resolution, atomic activation, old-generation lifetime, and failure
fallback.

`EV001` (Confirmed): the minimum 0.3.0 evolution loop is host-only, stateless, uses explicit
safe points, and activates atomically. A staging generation completes verification, resolution,
binding, and initialization first; failure leaves the old generation unchanged. Existing
references pin old generations, and 0.3 does not reclaim code.

`EV002` (Confirmed): runtime distinguishes pinned-generation references from Runtime-managed
switchable bindings. Ordinary references never silently retarget after activation; only
explicitly declared bindings participate in an atomic switch.

`EV003` (Confirmed): 0.3 has no state migration. A module initializer runs during staging and
may fail before activation. Initial rollback means continuing with or restoring the old
generation, not reversing external state already changed by code.

`TBD-EV004`: freeze exact source/API spelling for pinned references, switchable bindings,
initializers, and activation.

## 7. Symbol Query and Slot/Fragment

The following directions are confirmed:

- `Q001` (Confirmed): one Symbol Catalog; compile-time queries produce typed sets that resolve
  and erase before MoonIR;
- `Q002` (Confirmed): runtime queries return only explicitly runtime-exported,
  descriptor-backed typed references;
- `Q003` (Confirmed): every query uses an explicit terminal such as `.one()`, `.optional()`, or
  `.all()` to decide cardinality; no-match and ambiguity never depend on implicit link order;
- `SF001` (Confirmed): a slot is a module-level second-class symbol with stable
  SlotId/ContractId; invocation is a local control operation and creates no transferable slot
  value;
- `SF002` (Confirmed): a fragment nominally targets a SlotId and remains restricted control-flow
  composition rather than a general service/provider;
- `SF003` (Confirmed): static apply composition moves into MoonIR and retains zero Runtime cost;
- `SF004` (Confirmed): ordinary functions/function references and RuntimeFragmentRef are separate
  models and do not implicitly interchange even when shapes match.

`SF005` (Confirmed): 0.3.0 slot/fragment results remain `unit`. Static paths support
single-shot interceptors and single-shot contexts; the first runtime path supports only a
single-shot interceptor. Non-unit results, `many`, and runtime context/continuation ABI are
deferred.

`TBD-Q004`: freeze canonical ordering for `.all()` and the explicit ordering API; results must
not depend on link or registration order.

`TBD-SF006`: freeze exact syntax for module-level declarations, nominal fragment targets, and
lexical invocation, plus the precise interaction of return, abort, `?`, and cleanup in a
single-shot context.

Luna does not gain an effect mechanism merely because Slot/Fragment borrows control ideas from
algebraic effects.

## 8. Migration summary

Migration documentation records breaking changes, but the 0.3 compiler does not implement old
semantic compatibility.

| Area | Before: 0.2.1 | After: 0.3 | Reason | Implementation |
|---|---|---|---|---|
| Compatibility | one 0.2 semantics | one 0.3 semantics | keep the compiler focused | delete old logic; use the old compiler for old source |
| Phases | CompileTime/Runtime/Dynamic | compile-time/runtime | Dynamic conflates concepts | remove Dynamic retention and branches |
| Effects | no formal effects; scattered facts | still no effects; unified read-only sysmeta | avoid a new language mechanism | compiler-derived schema plus verifier |
| Type default | named types structural by default | named types nominal by default | stable ecosystem/runtime identity | one new TypeId rule |
| Usage | per-binding modifiers | add affine/linear block sugar | reduce resource-code noise | erase block policy after Sema fixes bindings |
| Rc/Arc | compiler-special TypeKinds | ordinary nominal containers | remove special cases | Resource/Drop protocol plus library implementation |
| MoonIR | pointer-heavy AST-like in-memory IR | one canonical serializable IR | support containers without two IRs | refactor type/symbol references in place |
| Native | generic descriptor/plugin ABI | trusted Luna Native | fast loading with proven provenance | code digest and build proof |
| Moon | no container | locally verified safe container | safe extension and evolution | serializer/parser/verifier/loader |
| C FFI | explicit extern C | extern C remains the foreign boundary; no general unsafe block | foreign code is not provable by Luna | `-t cffi` artifact plus extern C declaration |
| Old syntax | accepted by compiler | rejected by 0.3 compiler | no compatibility branches | migration table or separate migration tool |

## 9. Implementation priority

### C015: Splitting Sema is the first code implementation task (Confirmed)

The first 0.3 code implementation task is to split the current `SemanticAnalyzer` without
changing language semantics. The split must not opportunistically implement another `TBD-*`,
change AST/MoonIR/ABI, or change diagnostics and codegen results. Tooling keeps one
`SemanticAnalyzer` facade so internal architecture does not become public API.

The recommended shape is one shared `SemanticContext` and five coarse components, rather than
many micro-passes:

1. `DeclarationCollector`: declaration identity, namespaces, metadata schemas, and FFI
   declarations;
2. `TypeResolver`: TypeAST, constraints, trait/impl coherence, and generic instantiation;
3. `BodyAnalyzer`: statement, expression, call, iterator, and device semantics;
4. `CompileTimeEvaluator`: constants, constraint evaluation, selectors, and later Symbol Query;
5. `ControlAnalyzer`: Slot/Fragment/apply control semantics.

The existing `OwnershipChecker` stays separate; no EffectAnalyzer is created. `SemanticContext`
is the single owner of symbols, type/trait catalogs, inference state, diagnostics, and declaration
references. Components must not copy authoritative tables.

`SEMA001` (Confirmed): dependency direction is facade -> components -> context/core; components
cooperate through narrow interfaces and do not own one another. Before the first refactoring
commit this boundary only needs concrete C++ APIs; responsibility boundaries and the rule
against a second authoritative catalog are no longer open decisions.

Implementation completion (2026-08-09): the facade and the single state-owning context are in
place. `BodyAnalyzer`, `DeclarationCollector`, `TypeResolver`, `ControlAnalyzer`, and
`CompileTimeEvaluator` have been extracted with semantic parity. Each component now receives a
distinct context capability containing only its audited references and services; the analyzers
are no longer friends of `SemanticContext`. No authoritative catalog is copied and no component
owns another component. C015 and `SEMA001` have therefore passed their implementation gate.

| Order | Priority | Work | Completion gate |
|---:|---|---|---|
| 1 | P0 | split Sema with semantic parity, freezing the current regression baseline first | semantic/MoonIR/diagnostic/codegen evidence and tooling facade remain unchanged |
| 2 | P0 | freeze Confirmed/TBD boundaries and terminology | no pending item is replaced by an implementation assumption |
| 3 | P0 | establish final 0.2 baseline, migration corpus, and breaking-change list | old compiler reproduces all 0.2 evidence independently |
| 4 | P0 | freeze `-t` suffixes, package-kind spelling, and foreign API details | CLI/artifact RFC complete |
| 5 | P1 | add shadow SymbolId/ContractId/AbiLayoutId and sysmeta schema | identities compare without changing current codegen |
| 6 | P1 | switch once to the 0.3 nominal default and delete the old structural-default path | no language/edition branch |
| 7 | P1 | implement usage-block sugar and final binding contracts | no usage-scope node reaches MoonIR |
| 8 | P1 | complete generic Resource/Drop contracts | recursive cleanup positive/negative paths close |
| 9 | P2 | implement Rc/Arc library containers and remove compiler-special kinds | JIT/AOT/cleanup parity passes |
| 10 | P2 | refactor current MoonIR in place into table-referenced canonical IR | no frontend pointer identity reaches sealed IR |
| 11 | P2 | complete the single verifier, serializer, parser, and fuzz corpus | deterministic round trip; invalid containers rejected |
| 12 | P2 | implement Moon/Native/CFFI `-t` artifact paths | trust boundaries never silently downgrade |
| 13 | P3 | complete compile-time Symbol Catalog/query and static composition | static results erase with zero Runtime cost |
| 14 | P3 | complete Runtime descriptors, typed references, and loader | load-once Moon/Native loop works |
| 15 | P4 | complete minimum generation staging/activation/rollback | confirmed minimum evolution loop passes |
| 16 | P4 | audit the repository for Dynamic, old Rc/Arc, old slot/plugin, and other legacy surfaces | each replacement atomically removed its old path; production has no 0.2 compatibility branch |
| 17 | P5 | update formatter, LSP, Lunax, docs, benchmarks, and release gates | ecosystem targets only the new 0.3 semantics |

Every phase adds positive, negative, MoonIR/ABI, and pay-for-use evidence. Rollback uses version
control; it does not permanently retain legacy paths in the production compiler.

## 10. Non-priority placeholders

These confirmed deferrals are not 0.3.0 implementation priorities:

- `NP001` (Confirmed deferral): parallel computation and general concurrency;
- `NP002` (Confirmed deferral): expanded GPU types, grids, and cross-generation device updates;
- `NP003` (Confirmed deferral): stateful hot migration;
- `NP004` (Confirmed deferral): runtime context/multi-shot continuation ABI;
- `NP005` (Confirmed deferral): hotspot JIT, PGO, deoptimization, and code reclamation;
- `NP006` (Confirmed deferral): open runtime reflection and general runtime trait objects.
