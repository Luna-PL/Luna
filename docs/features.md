# Luna feature overview

[English](features.md) | [简体中文](features.zh-CN.md)

This page is a map of the implemented 0.3 development language surface. It explains
where each feature belongs in the architecture and points to the detailed
reference or design document. Experimental status and current limitations are
tracked in the [0.3 overall design](luna_0.3_design.md). The
[0.2.1 release notes](alpha_release.md) are a historical baseline.

## Verified compilation pipeline

Luna source is parsed and checked for types, traits and ownership before it is
lowered to MoonIR. MoonIR is verified both before and after MoonIR optimization;
LLVM JIT and AOT consume that same representation rather than reading the Luna
AST independently.

MoonIR preserves language-level safety and runtime contracts for the 0.3
host-specific Moon Container. Its serializer and atomic verified loader are
implemented, and `luna build <package> -t moon` emits a self-verified `.moon`.
The encoder projects generic compiler input to concrete types and monomorphized
functions; unresolved recipes never cross the container trust boundary. An
entry/export/host-interface/runtime-retention reachability closure then removes
unused concrete code and model rows while retaining direct/dynamic callees,
Drop glue, metadata, fragments, and their type dependencies.
The evolution runtime is not complete; portable cross-target containers are
explicitly deferred beyond 0.3.

Detailed design: [Architecture decisions D001/D002](decisions.md).

## Types, generics and traits

Named structs and enums are nominal by default. Anonymous `{ field: Type }`
records remain structural, and `type_same_shape` tests shape without erasing
declaration identity. `Target { field: value }` is explicit named construction.
Generics are instantiated for concrete uses, and ordinary static traits are
resolved at compile time without a vtable or runtime trait lookup.
Named `constraint` predicates provide a C++-concept-style boundary. Constrained
parameters, named `where Constraint<T>`, and inline `where` predicates compose
compile-time type propositions and are discharged at generic instantiation
sites without entering MoonIR.

Normative reference: [Type system](reference/type_system.md) and
[built-in type inventory](reference/builtin_types.md). Rationale:
[0.3 C008/TY002 design](luna_0.3_design.md#c008-named-types-are-nominal-by-default-confirmed)
and historical architecture decision D004.

## Ownership and explicit cost

Luna separates ownership relation from usage cardinality. Values may be owned,
shared-borrowed or mutably borrowed, while their use may be copy, affine or
linear. `move`, `borrow`, path-sensitive cleanup and linear kernel events make
resource transitions visible to both the checker and MoonIR.
`affine {}` and `linear {}` set zero-cost lexical defaults for new bindings;
explicit `copy let`/`affine let`/`linear let` contracts override the default without
weakening an inherent resource requirement. Only the resulting binding contracts reach
MoonIR.

Detailed design: [Architecture decision D004](decisions.md) and
[0.3 C009/C010](luna_0.3_design.md#c010-linear--and-affine-confirmed).

## Errors and panic

Recoverable failure uses the ordinary `Result<T, E>` value type. `Ok`, `Err`,
exhaustive general enum/Result `match`, inspection/extraction intrinsics and postfix `?`
form the initial surface. `?` either preserves the error type or invokes one
exact statically selected `From<Source> for Target` conversion, while carrying
path-sensitive cleanup obligations through MoonIR. Resource-bearing and nested
Results clean only their active payload.

`panic(string)` is an abort boundary, not stack unwinding: it reports through
the Runtime ABI console and terminates. This error model does not introduce a
general algebraic-effect system or an effect-summary layer.

Normative reference: [Error model](reference/error_model.md). Rationale:
[architecture decisions D005/D006](decisions.md).

## Iteration pipelines

Arrays, slices and integer `range` values can feed lazy `map`, `filter` and
`take` adapters, then terminate in `for`, `fold`, `for_each` or `count`.
The initial compiler-known recipe is fused into one LLVM loop with no
intermediate collection or iterator runtime allocation. Shared and mutable
iteration participate in the ownership checker.

General containers remain library types rather than builtins. Core now
materializes `Option<T>`, `Iterator`/`IntoIterator`/`FromIterator`, and inline
`Map`/`Filter`/`Take` adapters. Trait member calls select one static impl
symbol, and user Core `Iterator` implementations now drive `for` directly.
Move-only protocol items are dropped exactly once on normal and function-return
paths. `for` now inserts the unique static Core `IntoIterator` conversion and
owns its hidden state. Consuming move-only arrays use per-element initialization
bits; `filter`/`take` clean rejected items, and `map` may produce move-only
outputs or consume move-only inputs through an explicit owning parameter.
Lambda bodies now receive path-sensitive ownership checking, while
`fold`/`for_each`/`count` own hidden move-only terminal recipe state. The current
structured LLVM path uses an initialization bit for a move-only affine fold
accumulator. Its completed canonical-CFG replacement instead proves
move/reinitialization/final transfer statically with one synthetic local and no
runtime flag; that CFG is not yet the sole backend body. Linear accumulators remain
a staged boundary. `C016` closure environments support Copy capture and explicit
Affine/Linear move capture; borrowed capture remains rejected. No-capture recipes,
including recipes that own move-only array sources, can now materialize as affine,
single-consumption local stack values. Owning recipes carry per-element
initialization bits so consumption, abandonment and early returns close each
remaining item exactly once while retaining fused lowering and zero iterator
runtime allocation.

Detailed design and current limits: [Iteration, pipelines and container
boundaries](iterators.md).

## Metadata and runtime capability

Metadata schemas are first-class declarations. Static `select` executes an
ordinary user function over real iterable declaration/metadata views and is
resolved at compile time. Known declarations can instead use direct static
reflection without metadata or selection. Runtime visibility must be requested
with `runtime`; compile-time-only metadata is not silently retained. The 0.3
phase model has compile-time and runtime only. Legacy `dynamic select` and
`dynamic apply` forms still present in the development compiler are transitional
0.2 implementation surface pending the frozen runtime query/Slot replacements,
not a third phase or compatibility commitment.

Detailed guide: [Metadata and selectors](versioning.md).

## Packages and modules

Package IDs use reverse-DNS notation and form the versioning/dependency unit.
Modules use `::` and form source namespaces within a package. Manifests,
workspaces, lockfiles, `using ... as` aliases and explicit `export` declarations
define the current local package boundary.

Detailed guide: [Packages and modules](packages.md).

## Structured fragments

`interceptor`, `context`, `slot`, `resume`, `abort` and `apply` currently model
structured control flow and extensibility. Their current function-local source
forms are the implementation being migrated, not the final 0.3 surface. The
confirmed destination uses module-level second-class Slot identity, nominal
Fragment targets and verified MoonIR composition; exact declaration/control
spelling remains `TBD-SF006`.

Detailed guide: [Fragments, slots and plugins](fragments.md).

## Runtime and C FFI

Compiler-generated allocation, cleanup and output use the versioned Luna
Runtime ABI. User C interoperability remains an explicit `extern "C"` boundary
with constrained types and ownership contracts.

Detailed guide: [Runtime ABI and C FFI](runtime_abi.md).

## Heterogeneous compute

Reachable `kernel fn` declarations can run through the always-available CPU
simulator or be compiled for explicitly requested CUDA/ROCm targets. `launch`
returns a linear event, `await` closes the operation, and device buffers remain
protected while work is in flight.

Detailed guide: [Heterogeneous compute](heterogeneous_compute.md).

## Compile-time facilities and collections

The development compiler includes `const`, `constexpr`, compile-time type
reflection, safe fixed arrays, indexing and borrowed slices. Heap-owned general
collections and the final formatted standard I/O layer remain roadmap work; a
temporary typed `std::io` console surface is already available.

Detailed guides: [Compile-time facilities](compile_time.md),
[arrays, slices and iteration pipelines](iterators.md), and
[standard-library skeleton](standard_library.md).
