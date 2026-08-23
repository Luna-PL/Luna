# Luna 0.3 Overall Design Draft

English | [简体中文](luna_0.3_design.zh-CN.md)

> Document category: RFC / overall design
> Applies to: candidate Luna 0.3.0
> Status: Draft
> Normative status: non-normative RFC; implementation-completion records identify the portions already active in the 0.3 development compiler
> Final 0.2 implementation checkpoint: `a188d87a6f10d7fa67582389a0a0b915f3741401` (2026-08-09)

This document is the umbrella design for Luna 0.3. The existing
[Slot/Fragment refactoring audit](luna_0.3_evolution_audit.md) is a topic audit governed by
this document, not the complete 0.3 specification.

The document uses these states:

- **Confirmed**: a direction explicitly confirmed by the project owner;
- **Proposed**: a concrete recommendation that is not frozen;
- **TBD-xxx**: a stable placeholder that must be resolved before implementing its capability.

All example syntax is Draft. This document does not claim compiler support until the
implementation, tests, references, and changelog have been updated together.

### Frozen decision boundary (2026-08-09)

Confirmed IDs in this document are implementation authority. The following three IDs are the
complete unresolved set; an implementation must not choose their answers implicitly. A newly
discovered ambiguity must receive a stable `TBD-*` ID here before dependent code is written.

| ID | Decision still required | Blocks | Does not block |
|---|---|---|---|
| `TBD-EV004` | pinned/switchable/initializer/activation source and API spelling | the public evolution API and its final source/runtime bindings | generation identity, module leases, staging invariants, or internal state-machine tests |
| `TBD-Q004` | `.all()` order and explicit ordering API | public `.all()` semantics and its ABI | Symbol Catalog, typed query sets, `.one()`, or `.optional()` |
| `TBD-SF006` | module Slot/Fragment syntax and precise single-shot control interactions | new Slot/Fragment parsing, control semantics, and public runtime-apply surface | shadow SlotId/ContractId, descriptor schema, or legacy-corpus capture |

No Proposed decisions are currently registered. A new Proposed item must be labeled and added
to this boundary before it can influence an implementation.

The normative vocabulary for this draft is:

| Term | Meaning and excluded interpretation |
|---|---|
| compile-time / runtime | the only language phases; “dynamic” is explanatory prose only |
| sysmeta | compiler-derived, read-only typed facts; never an effect mechanism or user metadata |
| user `meta` | policy/application metadata; never evidence for safety |
| artifact target | the output level selected by `-t`; never a language mode or semantic version selector |
| Moon Container / Luna Native / Foreign C FFI | safe-after-local-verification / trusted-by-proof / unsafe foreign implementation boundaries |
| relation / usage | ownership relation and Copy/Affine/Linear consumption discipline; orthogonal axes |
| slot / RuntimeFragmentRef | a second-class control symbol / a typed runtime value; they are not ordinary functions |
| MoonIR | one canonical backend IR with in-memory and serialized forms, not two semantic IRs |
| unchecked operation | a possible narrow primitive-specific check omission; never a general `unsafe {}` scope |

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

`T003` (Confirmed): an artifact-producing `luna build` requires a `luna.package`; standalone
files remain valid inputs to `check`, `run`, and `analyze`. The manifest requires exactly one
of `kind = "application"` or `kind = "library"`. It has no language/edition field and 0.3.0
adds no linkage setting. `-t` is accepted only by `build`, while `-o` may replace the complete
default output path. Diagnostic `--emit-moonir` remains separate from a sealed `-t moon`
artifact.

The 0.3.0 target/package matrix is fixed:

| Package kind | `-t native` | `-t moon` | `-t cffi` |
|---|---|---|---|
| `application` | platform executable; exactly one package `main` | `.moon` with exactly one `main` entry | invalid |
| `library` | trusted loadable shared library; no `main` | `.moon` library; no `main` | C ABI shared library plus generated C header; no `main` |

Native and CFFI library outputs use the platform's ordinary shared-library convention:
`lib<name>.so` on ELF platforms, `lib<name>.dylib` on macOS, and `<name>.dll` plus the required
import `.lib` on Windows. Native applications use the platform executable convention. Both
Moon package kinds use `<name>.moon`; the manifest entry distinguishes their entrypoint rule.
`<name>` defaults to the final component of the Package ID. Without `-o`, outputs go under
`<package-root>/build/<target>/`.

0.3.0 deliberately has no distributable machine-code static-library artifact. Static-first
applications compose dependencies from source or canonical MoonIR before producing the final
application; this avoids adding archive-proof and duplicate-linkage policy to the MVP. A Luna
Native shared library carries its proof in a platform binary section. The proof section is
excluded canonically from its own digest calculation, while the proof binds all other loadable
code/data and typed descriptors. Removing or corrupting the section makes the Native loader
reject the artifact; no proof sidecar is searched.

`-t cffi` exports only functions explicitly declared `export "C" fn`; ordinary `export fn`
retains the Luna typed ABI and is therefore invalid in a CFFI library's public surface. Every
C export must use the closed C-ABI-safe type subset and the generated `<name>.h` declares the
compiler-selected link symbol, so metadata/module name isolation cannot disagree with the
binary. At least one C export is required. This explicit ABI spelling is retained because it
keeps source ABI independent of `-t`, not as a compatibility mode.

A Moon package declares authorized host imports in one flat manifest section:

```toml
[host-imports]
"io::write" = "org.luna.host.console.write"
```

The key is a package-local, module-qualified `extern "C"` declaration name and the value is a
stable host capability ID. The compiler derives the typed ContractId from that declaration;
users do not write or override it. `-t moon` rejects an undeclared foreign dependency, a listed
declaration whose derived contract is not representable as a typed host import, duplicate link
symbols with different contracts, and every path/library name in this section. MoonRuntime host
policy maps the import identity, ContractId, and capability to an implementation.

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
- 0.3 has no compiler mode for the 0.2 structural default;
- `nominal` is not a keyword or declaration modifier in 0.3: `struct` and
  `enum` already state all required identity semantics.

`TY001` (Confirmed): anonymous records, tuples, and function shapes remain structural. Named
types do not convert structurally merely because layouts match. Shape constraints may test
structural relations but never erase TypeId; conversion between named types requires explicit
construction or projection.

`TY002` (Confirmed): anonymous records do not use a `record` keyword. Their type and value
spellings are `{ x: i32, y: i32 }` and `{ x: 1, y: 2 }`. `Point { x: value.x,
y: value.y }` explicitly constructs a named value, while `{ x: point.x, y: point.y }`
explicitly projects a value to an anonymous record. A named value and an anonymous record, or
two differently named values, never convert implicitly merely because their fields match.

Record field names must be unique. Initializers execute in source order, but field identity,
ShapeId, TypeId, and physical layout use one canonical name order so reordering source fields
cannot change the structural type or ABI. Grammar context distinguishes records from blocks:
`{ ... }` is a block where a block is required and a record where an expression or type is
required. A statement-leading brace remains a block; a standalone record expression can be
parenthesized. 0.3 does not add unrestricted bare block expressions, which would make these
forms ambiguous.

Named `constraint` declarations remain the only user-visible compile-time proposition
mechanism. A C++-concept-style constrained parameter, a named `where Constraint<T>` clause,
and an inline `where` predicate are normalized to the same constraint predicate during
frontend analysis. The inline form is an anonymous, lambda-like spelling: it has no public
SymbolId, does not enter the Symbol Catalog, and is erased before MoonIR. A structural condition
uses the existing type-relation predicates, for example
`type_same_shape::<T, { x: f64, y: f64 }>()`; `where` does not introduce a separate
ShapeConstraint, effect, runtime contract, or TypeKind. Trait behavior bounds retain their
distinct trait semantics even when `where` carries their surface spelling.

### C009: Relation and usage remain orthogonal (Confirmed)

Ownership relation continues to describe owned/shared borrow/mutable borrow, while usage
continues to describe Copy/Affine/Linear. `affine` and `linear` modify a binding contract and
do not change TypeId.

### C010: `linear {}` and `affine {}` (Confirmed)

Block syntax is pure sugar: variables declared in the block default to the corresponding
usage. Sema fixes the final usage of every binding, and MoonIR carries no usage-block node, so
the construct has no runtime cost.

Example:

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
The weaker explicit spelling is rejected rather than silently promoted. `copy {}` is not a
usage-block form, and post-`let` qualifier spellings are not retained.

`US002` (Confirmed): ordinary nested blocks inherit the current usage default and nested
`linear {}`/`affine {}` blocks override it. The default applies to new local, pattern, and loop
bindings. Borrow relation remains orthogonal to usage, so a borrowed binding receives the
default while still obeying the borrow checker. Lambda and local-function parameters restart
from the Copy default; captures retain the captured binding's existing contract.

Implementation rule: the parser propagates only the lexical default to each affected binder.
Sema then computes the final contract as the stronger of that default and the type/initializer's
inherent requirement. MoonIR retains those final per-binding contracts for verification, but
never contains a usage-scope node or runtime operation.

### C011: Rc/Arc migrate to containers (Confirmed, implemented)

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

Implementation boundary (2026-08-09): reference-counting policy is trusted Core/Runtime library
implementation, not a compiler Resource kind. The compiler sees only ordinary nominal structs,
`Clone`, `Drop`, Affine handles, and the Global Luna release domain. The language currently has
no cross-thread transfer or sharing entry point, so Arc payload thread-safety admission is not
yet observable. Any future concurrency API must add a compiler-derived sysmeta gate before it
creates such a path and must not trust user metadata. This is a prerequisite of `NP001`; it does
not restore Rc/Arc TypeKinds.

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

`M001-A` (Confirmed): the first 0.3 canonical CFG uses typed-local form rather than requiring
MoonIR itself to be SSA. Parameters, source bindings, and lowering-generated temporaries are all
referenced by stable `LocalId`s; names survive only for diagnostics. The LLVM backend may still
obtain SSA performance through mem2reg and later optimization, while the Moon format avoids phi
construction and a second value discipline.

`M001-B` (Confirmed): an executable body contains one CFG block table plus region, scope, local,
and cleanup tables. `BlockId`, `RegionId`, `ScopeId`, `LocalId`, and `CleanupId` are zero-based
indices into their canonical tables and may neither be reordered nor dangle after sealing. A
block contains only operations that cannot transfer control and exactly one terminator. Jump,
conditional branch, switch, return, resume, abort, and unreachable cover the 0.3 exits. `if`,
`match`, loops, `?`, block expressions, and slot/fragment continuations must be normalized to
these blocks and edges before sealing; they cannot enter a container as nested executable nodes.
A lambda owns an independent CFG body rather than embedding a second body semantics in an outer
expression.

`M001-C` (Confirmed): regions record only the structural ownership and entry of function,
lambda, fragment, continuation, lexical, loop, match-arm, and apply regions; successor edges
remain the sole control semantics. A scope records its lexical parent, owning region, locals, and
cleanup set. A cleanup row uses a stable
`PlaceRef { root: LocalId, projections } + TypeRef + CleanupAction` to describe a possible
obligation; field, constant/dynamic index, and dereference projections may never fall back to
source strings. Every scope-exiting edge explicitly lists still-active cleanup references in
execution order. The verifier combines the scope-parent chain with initialization, move,
explicit-free, and transfer state derived from CFG operations, then independently recomputes the
active reverse-declaration cleanup order. It rejects omissions, duplicates, out-of-scope or
already-moved places, and wrong actions.
Ordinary fallthrough, return, `?` failure, and fragment abort/resume use the same edge rule.

`M001-D` (Confirmed): after migration, `FunctionDecl`, `FragmentDecl`, and lambda bodies can own
only canonical CFG bodies; there is no structured-body fallback or format compatibility flag.
A construction builder may temporarily retain source structure, but module sealing atomically
removes it, and both verifier and codegen accept only the CFG.

`M002` (Confirmed): a 0.3 Moon Container accepts only fully instantiated MoonIR. Before encoding,
the compiler derives a concrete projection: `TypeParam`, inference/unknown types, every type that
transitively depends on them, and generic declaration/function recipes are omitted, while concrete
instances and their runtime interfaces remain. An exported generic recipe is rejected because
silently removing public API would be unsound; open-world generic libraries require a later recipe
format. The concrete projection is also reachability-closed: application entrypoints, library
exports, typed host imports, and explicitly runtime-retained declarations are roots. Direct calls,
dynamic candidates, Drop glue, fragment regions, metadata schemas, and frozen type edges form the
transitive closure. Unreachable concrete functions/declarations/types are not serialized; package
dependency imports remain manifest-level interface facts.

`M003` (Confirmed): the 0.3 MVP Moon Container is host-specific and its manifest declares the
target triple and data layout. Cross-target portable containers and target-specific device code
are deferred to later format versions.

`M004` (Confirmed): the container is a deterministic sectioned binary with explicit format
version, section lengths, and parser resource limits. Manifest, type, symbol, contract, code,
import/export, and required sysmeta sections are mandatory; debug/source/device data sections
are optional. Content digests cover canonical non-signature sections, and parser/verifier work
requires a fuzz corpus.

`M005` (Confirmed, 2026-08-20): the 0.3 Moon Container uses the 8-byte magic
`89 4D 4F 4F 4E 0D 0A 1A`. Every multibyte integer is fixed-width little-endian: table indices,
enums, counts, and UTF-8 string byte lengths use `u32`; file offsets and section lengths use
`u64`; signed integer literals use two's-complement `i64`; floating literals use the raw `u64`
bits of IEEE-754 binary64. Section IDs are fixed and appear in ascending order. Required sections
are manifest, type, symbol, contract, code, imports, exports, and sysmeta; the high ID bit marks an
optional section. Unknown required sections, duplicate IDs, out-of-order entries, and overlapping
sections are rejected. Section starts are 8-byte aligned and every padding byte must be zero.

0.3 does not support payload compression; a non-zero compression flag is rejected, avoiding both
multiple canonical encodings of one model and decompression bombs. The container digest is
SHA-256 over the canonical header, directory, and all section payloads except digest/signature
material. 0.3 defines no Moon signature section: safety comes from the local verifier, while
authenticity signatures remain a later-format feature and must not be confused with Luna Native
proofs. Unknown optional sections may be skipped, but the emitter does not produce unknown
sections. Reader defaults are at most 64 sections, a 1 GiB container, a 16 MiB individual string,
`2^24` rows per table, and 256 nesting levels; host policy may only tighten these limits. These
choices do not change the M001-M004 semantic boundary.

The canonical 0.3 header is exactly 80 bytes: magic `[0,8)`, format major/minor `[8,16)`,
header size/flags/section count/reserved `[16,32)`, directory offset/file size `[32,48)`, and
SHA-256 `[48,80)`. The digest field is treated as all zeroes while computing the digest. The
directory immediately follows the header; each entry is exactly 32 bytes in the order
`id:u32`, `flags:u32`, `offset:u64`, `storedLength:u64`, `decodedLength:u64`. In 0.3 flags must
be zero and both lengths must match. File size is the end of the final section payload; trailing
data is not permitted.

`M005-A` (Confirmed, 2026-08-20): payloads use the recursive primitives
`str = u32 byte length + valid UTF-8 bytes`, `vec<T> = u32 row count + T...`, and
`bool = u32(0|1)`. Enums also use `u32`, and readers reject out-of-range values. Stable
TypeId/ShapeId/SymbolId/ContractId/AbiLayoutId values and `DeclarationRef` components are encoded
directly as `str`; decoding never depends on emitter pointers or an out-of-container symbol table.

The manifest payload order is `packageId:str`, `packageVersion:str`, `packageKind:u32`
(application=1, library=2), `targetTriple:str`, `dataLayout:str`, `entrySymbol:str`,
`entryContract:str`, and `featureBits:u32`. Feature bits 0..5 respectively denote runtime,
dynamic reflection, dynamic apply, dynamic select, kernel, and reserved kernel runtime; every
other bit is zero. Applications have a complete entry reference and libraries have none.

The type payload starts with `vec<TypeRecord>` in strictly increasing TypeId UTF-8 byte order.
The frozen `TypeRecord` field order is: its three identities; domain/identity-mode/kind; sysmeta;
the display/source/linkage/nominal names; type-parameter names; type arguments; inner type; array
length; mutability; parameter types; return type; parameter/return ownership contracts;
multi-shot; continuation kind; iterator mode; fields; captured fields; variants;
`inferenceId:i64`; the three canonical payload strings; layout ABI version/size/alignment/
signature; the drop-glue `DeclarationRef`; and the immediate referenced TypeIds. A field is
`name:str + type:str`, a variant is `name:str + vec<type:str>`, and an ownership contract is
`relation:u32 + usage:u32`.

Sysmeta within a TypeRecord is ordered as schema major/minor (each `u32`), five identity strings,
the four control enums and two booleans, resource parameter contracts/result contract/
management/release-domain/lifetime/relation/usage/cleanup and four booleans, six capability
booleans, then the two ABI booleans and drop-glue symbol string. Decoding completes bounds,
UTF-8, resource-limit, enum/boolean, and canonical-order validation before constructing sealed
Module indexes and invoking the MoonIR verifier.

`M005-B` (Confirmed, 2026-08-20): the declaration model is normalized into three sections with
strictly increasing SymbolId keys; no section duplicates a complete DeclarationRecord. A symbol
row contains `symbol/id/family/source/linkage:str`, `kind/retention:u32`, `type:str`, and source
location (`path:str + line:i64 + column:i64`). A contract row contains `symbol:str`,
`ContractId:str`, the complete typed sysmeta facts, the drop-glue `DeclarationRef`, and
`canonicalContract:str`.

The sysmeta section first carries metadata-schema rows in increasing schema-ID order (id/name,
typed fields, and location), followed by declaration-metadata rows whose SymbolId key set exactly
matches the symbol and contract sections. A metadata instance retains its schema ID, constants
in declaration order, retention, and location. Constants use `tag:u32 + payload`; tags 0..3 are
respectively `i64`, IEEE-754 raw `u64`, `bool`, and `str`. Readers require identical declaration
key sets, recompute SymbolId/ContractId/canonical contract, and publish the declaration table and
metadata schema index atomically only after every check succeeds.

`M005-C` (Confirmed, 2026-08-20): the imports section is a canonical sequence of ImportRecords
ordered by kind/owner/local-name/package/alias. A package row contains only the owner Package ID,
dependency Package ID, alias, and location. A host row contains only the owner, module-qualified
local declaration name, capability ID, link symbol, `C` ABI, typed `DeclarationRef`, TypeId, and
location. Package-only and host-only fields cannot be mixed.

The exports section is ordered by public name/Declaration SymbolId. Each row carries the public
name, typed `DeclarationRef`, TypeId, declaration kind, optional `C` ABI, and location. Only
explicit exports owned by the root package enter this table; dependency exports exist only for
compile-time resolution. The verifier resolves every typed reference, compares TypeId/kind,
rejects host imports that reuse a link symbol with different ContractIds, and never accepts a
path or library name as a substitute for a capability ID.

`M005-D` (Confirmed, 2026-08-20): the code section never serializes C++ RTTI or class names; it
uses explicit non-zero `u32` opcodes. Canonical block operations are only Let=1, Allocate=2,
Expression=3, Free=4, and Await=5. Structured Return/If/Match/While/For/Slot/Apply/Resume/Abort
nodes must already have become CFG terminators, edges, and regions.

Expression opcodes are Integer=1, Floating=2, String=3, Boolean=4, Unit=5, Identifier=6,
Binary=7, Unary=8, Call=9, DynamicSelect=10, Launch=11, VariantConstruct=12,
ResultConstruct=13, FieldAccess=14, Index=15, SliceLength=16, ArrayLiteral=17,
RecordLiteral=18, HeapAllocate=19, InitializeAllocation=20, Move=21, Borrow=22,
Dereference=23, AddressOf=24, Lambda=25, MakeClosure=26, EnvironmentLoad=27, and Assign=28.
Unknown, zero, or structured-only tags are rejected. Entering each nested expression or lambda
CFG consumes one reader depth level, with the default maximum fixed at 256.

`M005-E` (Confirmed, 2026-08-20): the code payload starts with function rows in increasing
SymbolId order. Each row carries executable facts that cannot be reconstructed from declaration
tables alone: package/module identity, source/generated name, kernel/reachability/extern/
constexpr/selector flags, ABI/link name, type parameters, typed parameter contracts, return
contract, template concrete arguments, location, and an optional sealed CFG. An extern function
has no CFG; every other concrete function has one. Generic recipes do not enter a 0.3 container.

A CFG encodes entry/root-region/root-scope followed, in order, by block, region, scope, local,
and cleanup tables. Every `TableRef:u32` in a table's identity column equals its row ordinal;
`0xffffffff` is the only empty reference. A block carries its region/scope, tagged operations,
and one terminator. A terminator uses the existing TerminatorKind as `u32` and explicitly carries
its operand, switch type, primary/secondary edges, switch cases, and exit cleanups. The reader
checks table shape, resources, and depth before the CFG verifier checks reachability,
scope/region relationships, and cleanup invariants.

Implementation completion record (2026-08-22): all eight required sections now
have fixed-width canonical codecs. After SHA/directory checks, the whole-container
reader atomically decodes types, declarations, interfaces, and code and publishes
the Module only after MoonIR verification. An independent Python oracle parses
actual container/code bytes, and LLVM JIT replay preserves the behavior of a real
frontend product after encode/decode. The encoder derives and verifies a concrete
projection, so a package may contain generic recipes while only instantiated code
and reachable concrete type/declaration rows enter the file. The loader independently
reconstructs the same closure before publication, preventing authenticated code from
smuggling a missing runtime TypeRef past the verifier. The driver now exposes
`luna build <package> -t moon [-o path]` and rejects standalone input, exported or
entrypoint generic recipes, kind/main mismatches, and native-only options.
The parser/verifier fuzz requirement is implemented as an opt-in Clang
libFuzzer target. Its reproducible corpus combines an actual CLI container,
independently constructed framing seeds, truncations, integrity failures, and
re-authenticated section mutations. A custom mutator preserves framing and
recomputes SHA-256 for part of the schedule so coverage reaches model decoders
instead of stopping at integrity rejection; the harness enforces failure
atomicity and canonical re-encoding.

### C016: Closure environment ABI (Confirmed)

Capturing closures extend the capture-free lambda subphase without altering its shipped ABI.
The capture-free `Function` value remains an 8-byte code pointer; only a lambda that actually
captures becomes a layout-bearing `Closure` value. This keeps the "pay for use" boundary exact:
a lambda that captures nothing does not pay for an environment pointer or a hidden argument.

`CL001` (Confirmed): the capture-free `Function` type, its 8-byte value size, its opaque code
pointer ABI, and its indirect-call convention are unchanged. A capturing closure does not add a
hidden environment argument to capture-free calls and does not promote a capture-free lambda to
a fat pointer.

`CL002` (Confirmed): a capturing closure has a distinct `TypeKind::Closure` type whose
environment layout is part of canonical type identity. The environment is a canonical,
independently recomputable product of captured-field records (field name, `TypeRef`, relation,
usage), so the verifier and a future Moon Container reader can reconstruct it from the frozen
type table without frontend pointers. `typeSize`, alignment, copy/move, and drop behavior are
derived from that product layout.

`CL003` (Confirmed): captures are by-value and never implicit references. A captured binding
retains its existing contract (`US002`): a Copy binding contributes a copy, an Affine or Linear
binding contributes a move. 0.3 has no capture-list syntax; the free-variable set is derived
from the lambda body by the frontend and independently cross-checked by the verifier.

`CL004` (Confirmed): the closure value uses an inline environment `{ code_ptr, env_fields... }`,
not a `{ code_ptr, env_ptr }` fat pointer. Inline environments make a Copy-only closure
naturally Copy (copying the value copies the environment fields) and avoid the ownership,
aliasing, and clone semantics a heap environment pointer would force onto the first slice.

`CL005` (Confirmed): the first slice supports Copy-only captures. Capturing an Affine or Linear
binding is rejected by both Sema and the MoonIR verifier, with an explicit diagnostic, until
move construction, partial-initialization cleanup, closure movement, and exactly-once
destruction are implemented and tested. Capturing a borrowed binding (a `Reference`-typed local)
is rejected by Sema with an explicit diagnostic for the same reason: an implicit environment
reference would silently outlive its loan. A closure whose environment owns non-Copy state is a
later slice, not an implicit behavior.

`CL006` (Confirmed): closure cleanup reuses the named-product recursive drop path. The
environment is lowered as a canonical product with recursive field cleanup, and the closure
drop delegates to that product's `dropGlue`. There is no separate closure-allocation or
closure-deallocation protocol in the inline slice.

`CL007` (Confirmed): MoonIR gains two explicit nodes. `MakeClosure` constructs a canonical
closure value from the lambda's code identity plus the materialized captured values. `EnvLoad`
reads a typed environment field from the closure's implicit environment parameter. `LambdaExpr`
retains the lambda executable identity and references its canonical closure/environment type
but does not hide environment construction inside the node.

`CL008` (Confirmed): the closure model is identical for structured and canonical-CFG emission.
Until the production sealer atomically replaces structured bodies (the Item 10 one-way switch),
capture support is implemented and tested on the representation the production backend actually
emits; the disconnected CFG path must not diverge from it. Capture-set ordering is derived from
one canonical rule shared by the builder and the verifier, not two independent derivations.

`CL009` (Confirmed): the builder resolves captures through an explicit capture phase that
identifies free references, verifies they are legal initialized captures, assigns deterministic
field indices, emits `MakeClosure`, and rewrites captured reads inside the lambda to `EnvLoad`.
A lambda CFG may not read an enclosing local directly.

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

Planning-gate completion (2026-08-09): the then-five-item unresolved register, confirmed `T003`, and
normative terminology above are exhaustive and guarded by `luna.0.3-design-contract`. The final
0.2 migration corpus contains ten representative cases, is pinned to the checkpoint above, and
has been independently replayed by that compiler. Priority items 2, 3, and 4 have therefore
passed their completion gates. The next implementation item is 5: introduce shadow identities
and sysmeta without changing current code generation.

Shadow-identity implementation completion (2026-08-09): TypeId, ShapeId, SymbolId, ContractId,
and AbiLayoutId are distinct C++ types. Canonical MoonIR type records now carry recomputable
Type/Shape/ABI-layout payloads, declaration records carry recomputable Symbol/Contract payloads,
and sysmeta schema 1.2 introduced a closed identity namespace projecting those IDs. The verifier rejects
payload mismatches and cross-payload hash collisions; deterministic tests prove that equal
contracts share ContractId while distinct declarations retain distinct SymbolId. LLVM codegen
continues to consume the old fields and the full JIT/AOT suite is unchanged. Priority item 5 has
therefore passed its completion gate.

Nominal-default implementation completion (2026-08-09): every named struct/enum now receives a
declaration TypeId, and the old source-default structural branch and redundant `nominal`
modifier/keyword have been deleted without adding a language/edition switch. Anonymous
`{ field: Type }` and `{ field: value }` records are inline
structural aggregates; `Target { field: value }` is checked as explicit named construction, and
field evaluation order is independent from canonical identity/layout order. C++-style
`<Constraint T>`, named `where Constraint<T>`, and inline `where predicate` all use the existing
compile-time constraint evaluator; the inline spelling has no symbol or MoonIR node. Positive,
negative, package-qualified, reflection, identity, layout, MoonIR-verifier, JIT, and AOT evidence
passes all 48 registered tests. Priority item 6 has therefore passed its completion gate. Named
products still use the current pointer representation as an implementation detail; this phase
does not claim an inline named-product layout redesign. The next implementation item is 7:
usage-block sugar and final binding contracts.

Usage-block implementation completion (2026-08-09): `affine {}` and `linear {}` now propagate
lexical defaults to local, enum-pattern, and `for` bindings; ordinary blocks inherit, nested
usage blocks override, and lambda bodies restart from Copy. Prefix `copy let`, `affine let`, and
`linear let` contracts replace the block default, while Sema rejects an explicit contract that
weakens a type/Resource, moved-source, or function-result requirement. A bare allocation of a
Copy type does not acquire an implicit affine contract: Luna retains its explicit-safety,
C-like default. The frontend
records only final per-binding usage in verified MoonIR; no usage-scope construct or runtime
operation survives lowering. Positive nesting/override/binder/lambda evidence, negative
linearity and weakening evidence, JIT/AOT behavior, and the full 48-test suite pass.
Priority item 7 has therefore passed its completion gate. The next implementation item is 8: complete
the generic Resource/Drop contracts and recursive cleanup paths.

Resource/Drop implementation completion (2026-08-09): `Drop` now resolves directly to the
canonical `org.luna.core::resource::Drop` identity with no legacy language branch. The compiler
derives one typed ResourceContract per frozen type containing relation, usage, cleanup strategy,
lifetime, management/release domain, and recursive-cleanup facts; sysmeta schema 1.3 and the MoonIR
verifier carry and validate that contract. A source `Drop::drop(&mut T) -> unit` is an
infallible in-place finalizer: compiler-owned field recursion runs afterwards, then the outer
storage is released by its allocation strategy. Named products, anonymous records, arrays,
active Result/enum payloads, shared payloads, and generic nominal instances therefore use one
recursive cleanup implementation. Generic Drop impl type parameters are resolved as impl
parameters; until Drop bodies are monomorphized, such impls require a representation-stable
target layout and reject inline type-parameter-dependent storage. Drop contracts are registered
before ordinary bodies so declaration order is irrelevant. Named-product storage now computes field offsets from actual value size/alignment,
including pointer-represented nominal fields. Positive recursive/generic/declaration-order
evidence and negative signature/Copy-weakening evidence pass with JIT/AOT and the full suite.
Priority item 8 has passed its completion gate. The next implementation item is 9: ordinary
Core `Rc`/`Arc` containers and removal of their compiler-special kinds.

Rc/Arc-container implementation completion (2026-08-09): `Rc<T>` and `Arc<T>` are now ordinary
generic nominal structs declared by `org.luna.core`. `Rc::new`/`Arc::new`, the prelude `rc`/`arc`
helpers, and `resource::Clone` are ordinary library functions/traits. Shared cells use Runtime
ABI v1 allocation, non-atomic/atomic retain-release, and compiler-produced type-erased Drop
callbacks so the last handle cleans the payload exactly once. Parser tokens, TypeKinds,
Sema/codegen cleanup paths, the `clone` intrinsic, and old Runtime entry points have been deleted.
`rc new`/`arc new` remains only in the frozen 0.2 corpus for replay by the old compiler and is
explicitly rejected by 0.3. JIT/AOT, nested Drop, explicit clone, implicit-copy rejection,
ordinary-nominal MoonIR, and LLVM Runtime-ABI evidence pass, together with the full 50-test
CTest suite, the strict-warning build, and resource-path ASan/UBSan checks. Named products remain
pointer-represented, so transparent/inline handle-wrapper ABI optimization is not claimed by
this semantic migration. Priority item 9 has therefore passed its completion gate. The next
item is 10: refactor the existing MoonIR in place into table-referenced canonical IR.

The first two subphases of item 10 are complete. The type subphase makes metadata, declaration,
signature, and operation nodes in sealed MoonIR carry only `TypeId`; the complete type structure is frozen in the single type
table. The LLVM backend uses an external, disposable materializer to reconstruct and cache backend
types by `TypeId`, without retaining or reading frontend `TypePtr`. The MoonIR verifier also
reconstructs only from frozen records and recomputes TypeId, ShapeId, layout, and every structural
reference. This adds no program runtime cost: it is compilation/load-time work. The migration also
fixed two identity bugs previously hidden by process-local pointers: Iterator recipe source types
now participate in stable shape/type identity, while sealing merges nominal forward placeholders
and then normalizes ShapeId and layout from the closed frozen type graph instead of accepting an
arbitrary frontend placeholder graph. Independent materialization, registration-order determinism,
tamper rejection, and full JIT/AOT regression evidence cover this boundary.

The symbol/contract subphase makes direct calls, function values, trait implementations,
Iterator/FromIterator protocol witnesses, kernels, `From` conversions, dynamic selection, and
fragment binding uniformly carry `DeclarationRef { SymbolId, ContractId }`. Linkage names are now
declaration-table payload only: the verifier proves that each use names an existing declaration of
the right kind with the expected ContractId, and the backend resolves linkage from that verified
row. Drop glue uses the same stable reference instead of entering sealed IR as a raw symbol string.
Compiler-owned `Drop`/`From` traits without source declarations receive ordinary synthetic table
rows when referenced, avoiding verifier exceptions. These checks remain compilation/load/binding
work and do not enter an ordinary call hot path.

The M001 subphase now has its table and construction foundation. Stable block, region, scope,
local, cleanup, and projected-place references define one typed-local CFG model; the structural
verifier checks table ownership, terminator shape, lexical visibility, local definitions, switch
bindings, and path-sensitive cleanup state across edges. A construction-only builder consumes the
transient structured body and produces this CFG for ordinary statements, lexical blocks,
`if`/`else`, `while`, `match`, and protocol-backed `for`. Direct `Iterator` and implicit
`IntoIterator` both become an ordinary call plus `Switch`/backedge graph; hidden converted state is
initialized once and cleaned on the `None` exit edge. Compiler-fused range and Copy-array recipes,
including shared/mutable/consuming source modes and `take`, expand into ordinary source/index/limit/
counter locals, indexed item construction, comparisons, assignments, branches, and backedges. No
iterator terminator or opaque recipe operation is added, and the verifier rejects an unexpanded
recipe in a sealed CFG. The builder is deliberately not attached beside the old body: a sealed
executable must never acquire two execution meanings.

The first declaration-level sealing slice is now in place for concrete function bodies. The
sealer builds every candidate graph from a non-consuming copy, independently verifies it, and
commits the whole candidate set only after all graphs succeed. A failed function therefore leaves
every structured body untouched; the runtime-apply boundary is an explicit rollback fixture.
Successful sealing removes the function's structured body and installs one `Function`-rooted CFG,
and module verification rejects missing bodies, simultaneous body/CFG ownership, root-kind drift,
or parameter-table disagreement. Frozen operand types omitted as redundant construction payload
are reconstructed from LocalId operands and the sealed type table without overwriting a conflicting
non-empty type. This sealer is not yet invoked by the production pipeline: fragment/runtime
composition and LLVM CFG consumption must close before the one-way module switch.

The first LLVM-consumption slice is now executable but remains outside the production pipeline.
For an exclusively CFG-backed function, the backend allocates typed-local storage by `LocalId`,
maps parameters by the verified parameter table, emits non-control operations, and translates
`Jump`, `Branch`, `Return`, `Switch`, `Resume`, `Abort`, and `Unreachable` directly from the block
table. `Switch` reads the frozen enum/Result tag once, extracts payload fields with the frozen ABI
layout, and installs pattern bindings by `LocalId` only after the case-edge cleanups. Source-level
`Ok`/`Err` construction is normalized to the data-only `ResultConstructExpr`, rather than retained
as an unresolved intrinsic call. Source-to-JIT fixtures with lexical shadowing and enum/Result
matches both return 42: they prove that duplicate diagnostic names retain distinct storage and
that three pattern locals receive the active payloads. This is the same function codegen entry
selecting one exclusive body representation during migration, not a second compiler backend.
Root, unguarded value cleanups now lower on jump, branch, switch, return, resume, and abort edges in the
exact verifier-approved order. Branches receive edge-specific cleanup blocks, while a return value
is evaluated before its exit cleanups, preserving Luna evaluation order. The fixture covers both a
root parameter return cleanup and a lexical fallthrough cleanup. Projected/guarded and
raw-allocation cleanups, allocation operations, and the wider expression surface currently fail
closed; the production sealer therefore remains disconnected until those paths and runtime
composition are complete.

The capture-free lambda subphase is also complete. A lambda expression remains a closure-value
node, while construction consumes its structured body into an independent canonical CFG rooted at
a `Lambda` region. This is not a second IR layer: the parent function and lambda each have exactly
one CFG, and the closure value merely refers to the lambda executable. The verifier recursively
checks that child graph, matches the closure type, parameter contracts, and parameter locals, and
rejects simultaneous body/CFG ownership. Sema already rejects local capture until closure-
environment layout exists; canonical construction likewise rejects `captures`, so this step does
not introduce an implicit or partial closure ABI. Signature consistency checking also exposed and
fixed a lowering defect where explicit `linear T`/`affine T` lambda returns bypassed the resolved
nominal identity. A usage wrapper remains only a binding contract, while the inner `T` now retains
its actual TypeId instead of degrading to a same-named placeholder.

Using those lambda CFGs, capture-free `map`/`filter` over Copy items and Copy callable contracts now
enter the same canonical expansion. The source and each callable/`take` argument evaluate once in
source order in loop init; `map` becomes an ordinary typed-local call plus result local, while
`filter` becomes a bool call and
branch whose rejection edge enters the shared index-increment backedge. Adapter order therefore
controls which items consume a `take` counter, with no intermediate container and no iterator IR
operation. The verifier additionally matches local-call arguments/results and let initializer/local
types, so the expanded graph no longer relies on the recipe's frontend trust.

The first non-Copy per-item slice is complete for a direct `for` whose `map` turns a Copy input into
an owned Affine value. That result initializes a synthetic Affine local. Later `filter` predicates
read it through an explicit `SharedBorrow`, while `take` preserves it unchanged. A rejection or
exhaustion edge releases the temporary before leaving the per-iteration scope; an accepted item is
either explicitly moved into a later owning `map` or, at the end of the adapter chain, into the
source-visible iteration binding. An owning map consumes the old synthetic local and atomically
activates its Copy or Affine replacement in the ordinary result `LetStmt`. Normal body fallthrough
releases the final binding on the existing cleanup edge, and an early return uses the existing
return cleanup set. The verifier independently rejects a copied map input/result or any missing
rejection/body-exit cleanup, so this adds no runtime initialized bit. This also fixes the underlying
relation rule: a borrowed local has Copy cardinality and owns no cleanup even when its underlying
type is Affine. Move-only consuming source arrays, Linear per-item state, a non-Copy callable or
closure environment, materialized recipe state, and iterator terminals remain outside this slice.

The guarded-array cleanup foundation is now frozen for the next consuming-source slice. Instead of
the structured backend's `[N x i1]` initialization bitmap, canonical CFG records one synthetic Copy
integer `nextUnread` cursor and a constant-index cleanup row for every array element. Element `i`
is cleaned only when `nextUnread <= i`; the cursor therefore represents the unread tail with one
runtime word, while guard checks occur only on cleanup exits. The verifier requires an owned frozen
array, a same-scope synthetic integer cursor, one guard per element, one shared cursor, and no mixed
whole-array or unguarded cleanup. Cleanup-table canonicalization now orders projected places
deterministically. This commits the verifiable state representation but does not yet relax the
builder's move-only consuming-array rejection; cursor updates and every early-exit edge must be
generated together in the following slice.

That first generation slice is now complete for a direct, non-materialized `for` over a
cleanup-bearing Affine array. The guarded `nextUnread` state reuses the loop index itself, so the
successful path adds no state word beyond the cursor already required by Copy-array iteration. A
dynamically indexed `MoveExpr` atomically transfers `source[index]` and advances that same cursor to
`index + 1`; the bottom backedge therefore does not increment it again. Exhaustion cleans the
guarded unread tail, filter/take plumbing can retain the current item's ordinary cleanup, and a
function/fragment return or abort receives both the current frontend obligations and the guarded
tail. The verifier requires a zero initializer, the same local as dynamic index and cleanup cursor,
complete guards, immutable cursor access outside the atomic transfer, and exact cleanup sets on
normal and early exits. The direct fixture independently rejects a missing transfer witness or one
omitted tail cleanup. Linear elements, materialized move-only sources, and terminal consumption
remain later slices.

The slice-bound subphase is now complete as well. A slice recipe evaluates its source exactly once,
materializes its runtime bound once in loop init, and uses the same ordinary indexed-borrow CFG as a
shared array recipe. The bound is represented by `SliceLengthExpr`, a fundamental slice projection,
not an iterator operation or terminator. It preserves the slice ABI's `usize` length instead of
canonizing the old backend's narrowing to `i32`; the slice loop index therefore also uses `usize`,
while the existing source-level `take` count remains `i32`. The verifier independently requires a
frozen slice operand and a `usize` result. Because the stable language surface has only read-only
slices, Sema and canonical construction accept direct/shared slice iteration and reject mutable or
consuming slice recipes.

The first materialized-recipe subphase is complete for `for` consumption. Binding a range, borrowed
array/slice, or consuming Copy array now immediately produces ordinary source/index/limit/adapter
locals; the compiler-domain Iterator binding itself is erased. A consuming Copy array is snapshotted
at the binding point, and source plus adapter arguments retain their original left-to-right
evaluation order. The existing index local is strengthened to an affine synthetic local and moved
into the consuming loop, so it doubles as the zero-extra-state single-consumption witness. A second
consumption, copying that cursor, or path-inconsistent consumption is rejected by canonical CFG
ownership dataflow. No Iterator-typed local, recipe metadata, runtime token, allocation, or new ABI remains in
the sealed graph.

The first materialized terminal slice is also complete. `count` and a Copy-accumulator `fold`
continue from erased recipe state as ordinary loops with outer result locals, while an
expression-statement `for_each` emits an ordinary body call. Adapters appended at the terminal are
evaluated once before terminal arguments. This slice permits a value-producing terminal directly as
an initializer or return value, and as the sole argument of a direct ordinary call; `for_each` is
accepted as an expression statement. Wider expression sibling hoisting was deliberately deferred in
that slice so evaluation order was never guessed; its first eager-Copy coverage is recorded below.

The materialized `collect` subphase is now complete too. Construction first validates the three
frozen `FromIterator` declaration signatures, then lowers `begin()` to one synthetic affine builder
local. Every loop-body `push` receives an explicit mutable borrow of that same local, and every
normal recipe exit converges before `finish(move builder)`. The affine finish result is held by a
second synthetic local and explicitly moved to the source-level initializer, return, or direct-call
consumer. Existing cleanup dataflow therefore activates the builder/result obligations exactly
once and deactivates them on each transfer, without a runtime iterator object, ownership flag, or
new ABI state. The independent verifier now also checks declaration-backed call signatures,
including an explicit borrow argument's relation, while synthetic ownership dataflow requires the
finish transfer; forged shared builder borrows or copied finish state are rejected.

The direct non-materialized Copy-terminal subphase is complete as well. In a direct initializer or
return, or as the sole argument of a direct ordinary call, receiver source/start and bound values
and adapter arguments are materialized in source order before terminal arguments. The resulting
ordinary state then enters exactly the same terminal expansion described above. This covers direct
`count`, Copy `fold`, expression-statement `for_each`, and affine-builder `collect` without adding a
second lowering. This direct slice initially rejected a terminal after any earlier sibling operand;
the Copy operand-hoisting subphase below now supersedes that temporary boundary.

The affine-accumulator `fold` subphase is now complete. One synthetic affine local owns the initial
value, is moved into the reducer on every iteration, and is reinitialized by that reducer's affine
return in the same transfer assignment. The independent ownership dataflow rejects a copied
accumulator, replacement without prior consumption, or a copied final result; the source-level
consumer receives an explicit move. Normal loop backedges and the zero-iteration exit therefore
agree on one active cleanup obligation without adding an initialized bit, runtime ownership flag,
or second accumulator. Linear accumulators remain outside this hidden terminal state.

The eager-expression ordered-operand slice is now complete. Ordinary-call callee and
arguments, non-short-circuit binary operands, index operands, array and variant elements,
dynamic-select filters, and launch operands are scanned in source order. When a later operand
expands into an iterator-terminal CFG, each earlier Copy value is first stored in an ordinary
synthetic local and the parent expression reads it by LocalId. A non-trivial earlier `unit`
expression is instead emitted exactly once as an ordinary `ExprStmt`, and the parent receives a
zero-sized `UnitExpr`; it therefore needs neither a synthetic local nor runtime state. A callee load or
side-effecting earlier call therefore cannot drift past the terminal loop, and no runtime tag or
ownership flag is introduced. The independent verifier checks the resulting let/local/type
references. The same ordered lowering now accepts a cleanup-free Affine value produced by an
Affine-returning call or explicit `move`: its synthetic local has Affine usage and the parent reads
it through exactly one generated `MoveExpr`. The verifier rejects a forged Copy read. A
cleanup-bearing Affine value with an explicit transfer is also accepted across later control flow.
The construction bridge records its synthetic cleanup row as active until the parent expression is
emitted. Any `TryExpr` failure or structured `return` built in that interval includes the row in its
ordinary `Return.exitCleanups`; the successful path reaches the generated parent `MoveExpr`, which
consumes it. The independent ownership dataflow rejects an omitted early-exit cleanup, so no runtime
initialized flag is required. An explicitly transferred Linear sibling is accepted only when a
recursive scan proves that no remaining operand contains a `TryExpr`, `BlockExpr`, or `IfExpr` that
may leave before the parent consumes it. Its synthetic Linear local is tracked by the verifier's
compile-time ownership marker and read through exactly one generated `MoveExpr`; unlike Affine, it
has no cleanup fallback on an early exit. Linear siblings across a potential early exit remain
explicitly rejected because they violate the exactly-once usage contract; they are never copied or
reordered implicitly.
Short-circuit operands follow the conditional CFG normalization below.

Anonymous structural records are now distinguished from allocating products. Their inline value
construction has no allocation boundary, so field expressions use the same source-ordered operand
normalization as arrays and variants; a terminal field becomes an ordinary result-local reference
inside the final `RecordLiteralExpr`.

The allocation-aware construction subphase is now complete for unique named-struct and explicit
heap allocation. `AllocateStmt` first defines an owned Affine allocation LocalId whose
`CleanupKind::Allocation` row releases backing storage even when the stored type itself is Copy.
Initializers then evaluate in source order through the ordinary operand normalization. Values that
must survive later control flow live in synthetic locals; an early `?` or structured return cleans
those evaluated values in reverse order and then releases the raw allocation. Only after every
initializer succeeds does `InitAllocationExpr` consume the raw identity and transfer the values into
their frozen field ordinals. A binding then owns exactly one final value or allocation cleanup.
Consequently partial initialization needs no runtime initialized bit and never invokes Drop on an
uninitialized field. The independent verifier rejects a missing/raw cleanup of the wrong kind, an
unknown allocation LocalId, a layout/type mismatch, or any sealed legacy `HeapAllocExpr` or
allocating `RecordLiteralExpr`. Discarding an owning allocation result is rejected rather than
leaked. Other storage kinds remain outside the current single `Unique` storage model.

The first conditional-expression slice is now complete for Luna's current block syntax. Because a
block has no tail value, both `BlockExpr` and block-style `IfExpr` are semantically `unit`; CFG
construction consumes their structured blocks, emits lexical regions plus ordinary branch/jump
edges, and replaces the completed expression with a zero-sized `UnitExpr`. Nested `else if` and an
`if` used in an ordered operand position follow the same path. `UnitExpr` owns no body or successor
and therefore does not add a second control IR. The verifier still rejects any sealed graph that
retains `BlockExpr` or `IfExpr`. A future tail-value design would require an explicit synthetic
result local, but that value semantics is not inferred by the 0.3 implementation.

`TryExpr` normalization is now complete as the next conditional slice. The operand is evaluated
once and becomes an ordinary `Switch` over the frozen Result ABI (`Err = 0`, `Ok = 1`). Its success
payload is a pattern local on the only continuing path. The failure path optionally invokes the
statically resolved owned `From` witness, builds an ordinary `ResultConstructExpr`, executes the
source-derived cleanup set on the return edge, and terminates with `Return`. Move-only Result/error
payloads use explicit transfers, so the independent ownership dataflow verifies operand
consumption, conversion, propagated return, and cleanup without an exception runtime or hidden
success flag. `ResultConstructExpr` is data only; sealed CFG still rejects every residual
`TryExpr`.

Short-circuit expression normalization is now complete. `&&` and `||` each use one ordinary
synthetic Copy `bool` local, initialized to `false` and `true` respectively, then branch around the
right operand on the short-circuit path. Only the required path evaluates and assigns the right
operand before both continuing paths converge. Nested control flow in that operand is normalized
on the same conditional path, so no operation can be hoisted across the language's evaluation
boundary. This adds no runtime ownership flag or second control representation; the state is an
ordinary boolean value and the independent verifier rejects every residual short-circuit
`BinaryExpr` in a sealed CFG.

One-shot statement discriminants now use the same expression normalization. An `if` condition is
fully normalized before its `Branch`, and a `match` scrutinee before its `Switch`; iterator
terminals and nested conditional expressions therefore cannot survive inside either terminator
operand or be evaluated after an arm begins.

Repeated `while` conditions are now normalized without reusing one-shot state. A loop-owned header
is the target of both the initial edge and the body backedge. It enters a child condition-evaluation
scope on every trip; terminal, short-circuit, and other synthetic locals live only in that scope,
then become invisible on both the body and exit edges. Canonical ownership dataflow consequently
deactivates compile-time affine markers before the next header entry and the declarations reactivate
them on the next execution. This models source-level repeated evaluation without a runtime
initialized flag, and an ordinary condition still keeps the previous compact single-block shape.

The first slot/fragment subphase is complete for static single-shot interceptors. A lexical
`apply { ... }` resolves its `DeclarationRef` during construction and becomes an `Apply` region;
each invoked interceptor is cloned from its construction body into a `Fragment` region, while the
slot body becomes a sibling `Continuation` region. Normal interceptor fallthrough uses a verified
ordinary `Jump` to the continuation entry; `Resume` remains exclusive to an explicit context
resume point. `abort()` uses an `Abort` edge to the fragment exit, and a
fragment-local `return` becomes an ordinary cleanup-bearing jump to that same exit, so neither can
enter the continuation. A return inside the continuation remains the enclosing function's
`Return`. Every Fragment region carries its stable declaration/contract reference. The verifier
independently requires automatic forwarding only under an interceptor contract, every resume only
under a context contract and into the continuation of the same Apply, and every abort to target its
enclosing fragment exit. Static composition
therefore retains no descriptor, selector, function pointer, heap continuation, or runtime dispatch
state. The declaration body is cloned only by the construction bridge and remains available for
other static call sites; it is not a second executable meaning in the sealed graph. Blockless apply
is rejected at this boundary because 0.3 requires an explicit lexical body.

Static single-shot contexts now use the same composition rather than a separate CPS operation.
Each source `resume()` has a `Resume` terminator entering a retained lexical `Continuation` region;
normal continuation completion jumps back to the statement following that resume. Fragment locals
remain live across the edge but are not lexically visible inside the continuation, whose names bind
to the invoking environment. A continuation `return` is the outer function `Return`, a context
fallthrough is an implicit `Abort`, and explicit fragment `return`/`abort()` both skip the
continuation through the fragment exit. The verifier rejects forged fragment identities, an
ordinary context jump into a continuation, a continuation escape through a non-exit edge, or a
continuation reference to fragment-local state. This graph requires no heap continuation, runtime
descriptor, dispatch, or ownership flag.

Fragment parameters now use ordinary canonical Let/local bindings with an explicit final relation.
The Fragment region records their ordered LocalIds, and the verifier matches each TypeRef,
SharedBorrow/MutableBorrow/Owned relation, and Copy/Affine/Linear usage against the frozen fragment
contract. Borrowed bindings therefore own no cleanup and do not consume their source, while an
owned move-only binding still requires the existing explicit transfer and activates exactly one
cleanup obligation. Forging both a binding row and its Let relation cannot bypass the region-level
contract check. This extends the typed-local model rather than adding a fragment-only parameter
operation.

The production frontend-to-construction bridge now exercises the same path as the hand-built
canonical fixtures. A direct invocation of a slot default synthesizes a construction-only `Apply`
region because no source `apply` block exists to own the Fragment/Continuation pair; this region
erases with static composition and is neither a source construct nor runtime state. During local
binding, an identifier whose construction form has no redundant expression TypeRef inherits the
frozen TypeRef from its resolved LocalId. A non-empty conflicting TypeRef is left intact for the
independent verifier to reject. The integration gate runs source parsing and Sema, Luna lowering,
structured verification, CFG construction, then CFG verification, and checks the default
fragment, lexical capture, resume edge, region topology, and preservation of the declaration's
construction body. The same source-level gate covers an explicit static `apply`; a separately
lowered dynamic composition is accepted by the frontend but must be rejected at this static CFG
boundary. Multi-shot and runtime apply remain later slices.

Item 10 was completed on 2026-08-20. Capturing closure environments (frozen as `C016`),
non-Copy item/callable per-element ownership transitions, unconditional sealing, and the
canonical-only backend boundary are implemented. The structured statement/continuation/slot/
fragment executable-body consumer has been removed from the source and build.
Linear hoisting across a potential early exit remains invalid by its exactly-once contract rather
than a deferred lowering feature. The next mainline is item 11 serialization/parsing.

Closure capture implementation completion (2026-08-14): the Copy-capture
`C016 CL001`-`CL009`
slice is active. Capture-free `Function` values keep the 8-byte bare code pointer ABI; a
capturing lambda becomes a layout-bearing `Closure` type whose environment fields participate
in canonical type identity, value size, and ABI layout. Sema derives the free-variable set in
first-reference order and rejects Affine/Linear and borrowed captures with an explicit
diagnostic. MoonIR carries `MakeClosure` and `EnvLoad` nodes; the verifier checks the capture
list against the environment layout, the environment parameter identity, and every EnvLoad
field bound. Canonical CFG construction declares the environment parameter local and rewrites
capture reads into EnvLoad, so structured and CFG paths share one closure model. JIT and AOT
execute captured closures through a hidden environment pointer; positive Copy-capture,
negative borrowed-capture, and canonical-CFG evidence pass with the full 51-test suite,
the strict-warning build, and ASan/UBSan. The later non-Copy slice explicitly moves
Affine/Linear captures into the environment, consumes the outer binding, and recursively cleans
the environment; borrowed captures remain rejected because their lifetime cannot safely escape.
Only after structured execution is deleted and the
full verifier/codegen regression gate passes does the item-level gate pass; serializer/parser work
remains item 11.

Canonical CFG switchover record (2026-08-20): all 51 registered CTests pass and the
CompilerPipeline now seals every executable function body unconditionally. A materialized move-only iterator installs outer projected guarded
cleanups when the recipe is created, cleans abandoned/early-returned elements in source order,
and atomically transfers its source into loop-local guarded state when consumption begins. Finite,
statically linked runtime contexts, replay-safe multi-shot continuations, and statement-form apply
are now canonical CFG features. Each `resume()` owns an independent Continuation region, while an
unknown context/many candidate cannot fall back to interceptor-only external plugin ABI v1. Item 10
remains open until the one-way production switchover is complete.

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
- `NP004` (Confirmed deferral): persistent runtime context/multi-shot continuation callback ABI
  across an external plugin boundary; finite statically linked candidates use scoped CFG regions;
- `NP005` (Confirmed deferral): hotspot JIT, PGO, deoptimization, and code reclamation;
- `NP006` (Confirmed deferral): open runtime reflection and general runtime trait objects.
