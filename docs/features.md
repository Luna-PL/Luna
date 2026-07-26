# Luna feature overview

[English](features.md) | [简体中文](features.zh-CN.md)

This page is a map of the implemented 0.2.0-alpha language surface. It explains
where each feature belongs in the architecture and points to the detailed
reference or design document. Experimental status and current limitations are
listed in the [Alpha release notes](alpha_release.md).

## Verified compilation pipeline

Luna source is parsed and checked for types, traits and ownership before it is
lowered to MoonIR. MoonIR is verified both before and after MoonIR optimization;
LLVM JIT and AOT consume that same representation rather than reading the Luna
AST independently.

MoonIR is intended to preserve language-level safety and dynamic contracts for
future portable Moon containers and MoonRuntime loading. The container loader
and hotspot runtime are reserved interfaces, not completed release features.

Detailed design: [MoonIR metadata refactor RFC](Arch/MoonIR_Metadata_Refactor_RFC.md).

## Types, generics and traits

Structs and enums are structural by default. `nominal` creates an explicit
identity boundary without conflating semantic identity with physical layout.
Generics are instantiated for concrete uses, and ordinary static traits are
resolved at compile time without a vtable or runtime trait lookup.
Named `constraint` predicates provide a separate C++-concept-style boundary:
they compose compile-time type propositions and are discharged at generic
instantiation sites without entering MoonIR.

Detailed design: [Types and identity](types.md) and
[Type-system identity RFC](Arch/Type_System_Identity_RFC.md).

## Ownership and explicit cost

Luna separates ownership relation from usage cardinality. Values may be owned,
shared-borrowed or mutably borrowed, while their use may be copy, affine or
linear. `move`, `borrow`, path-sensitive cleanup and linear kernel events make
resource transitions visible to both the checker and MoonIR.

Detailed design: [Ownership and affine model](Arch/Ownership_Affine_Model_RFC.md).

## Errors and panic

Recoverable failure uses the ordinary `Result<T, E>` value type. `Ok`, `Err`,
`is_ok`, `is_err`, `unwrap` and `unwrap_err` form the initial intrinsic
surface; postfix `?` performs a checked early return and carries path-sensitive
cleanup obligations through MoonIR. Resource-bearing Results clean only their
active payload.

`panic(string)` is an abort boundary, not stack unwinding: it reports through
the Runtime ABI console and terminates. This error model does not introduce a
general algebraic-effect system or an effect-summary layer.

Detailed design: [Result and panic RFC](Arch/Error_Result_Panic_RFC.md).

## Iteration pipelines

Arrays, slices and integer `range` values can feed lazy `map`, `filter` and
`take` adapters, then terminate in `for`, `fold`, `for_each` or `count`.
The initial compiler-known recipe is fused into one LLVM loop with no
intermediate collection or iterator runtime allocation. Shared and mutable
iteration participate in the ownership checker.

General containers remain library types rather than builtins. User-defined
Iterator implementations and escaping adapter values are reserved until the
Core `Option`/Iterator layouts and move-only per-item drop state are complete.

Detailed design and current limits: [Iteration, pipelines and container
boundaries](iterators.md).

## Metadata and dynamic capability

Metadata schemas are first-class declarations. Static `select` executes an
ordinary user function over real iterable declaration/metadata views and is
resolved at compile time. Known declarations can instead use direct static
reflection without metadata or selection. Runtime visibility must be requested with `runtime`, while
`dynamic select` and `dynamic apply` explicitly opt into runtime binding costs.
Compile-time-only metadata is not silently retained.

Detailed guide: [Metadata and selectors](versioning.md).

## Packages and modules

Package IDs use reverse-DNS notation and form the versioning/dependency unit.
Modules use `::` and form source namespaces within a package. Manifests,
workspaces, lockfiles, `using ... as` aliases and explicit `export` declarations
define the current local package boundary.

Detailed guide: [Packages and modules](packages.md).

## Structured fragments

`interceptor`, `context`, `slot`, `resume`, `abort` and `apply` model structured
control effects. Static paths use direct structured lowering; dynamic paths are
explicit and retain only the descriptors needed for runtime dispatch.

Detailed guide: [Fragments and slots](fragments.md) and
[external fragment plugins](dynamic_plugins.md).

## Runtime and C FFI

Compiler-generated allocation, cleanup and output use the versioned Luna
Runtime ABI. User C interoperability remains an explicit `extern "C"` boundary
with constrained types and ownership contracts.

Detailed guides: [Runtime ABI](runtime_abi.md) and [C FFI](ffi.md).

## Heterogeneous compute

Reachable `kernel fn` declarations can run through the always-available CPU
simulator or be compiled for explicitly requested CUDA/ROCm targets. `launch`
returns a linear event, `await` closes the operation, and device buffers remain
protected while work is in flight.

Detailed guide: [Heterogeneous compute](heterogeneous_compute.md).

## Compile-time facilities and collections

The Alpha includes `const`, `constexpr`, compile-time type reflection, safe
fixed arrays, indexing and borrowed slices. Heap-owned general collections and
the formatted standard I/O layer remain roadmap work.

Detailed guides: [Compile-time facilities](compile_time.md),
[arrays and slices](arrays.md), [iteration pipelines](iterators.md), and
[standard-library skeleton](standard_library.md).
