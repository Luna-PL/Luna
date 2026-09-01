# Luna 0.2 Alpha Architecture Decisions

> Document category: adopted design decisions
> Applies to: Luna 0.2.1
> Status: Frozen 0.2 rationale baseline; superseded where the 0.3 overall design differs
> Normative status: non-normative rationale record; current behavior follows the [Alpha semantic reference](reference/README.md)
> Implementation audit: 2026-07-31

This document condenses the “why” from adopted RFCs. It does not repeat complete semantics,
ABI numbers, or the future roadmap. If a decision summary conflicts with a reference
document, follow the reference and its regression evidence.

## D001: MoonIR is the sole backend input

**Decision**

- Type-correct programs lower to MoonIR before entering JIT or AOT.
- MoonIR retains package/module identity, type tables, ownership-cleanup obligations, metadata,
  and reachability.
- It is verified before LLVM lowering and verified again after optimization.

**Rationale**

Generating LLVM IR directly from multiple frontend stages would let semantic checks, ownership
cleanup, and JIT/AOT behavior drift. A verifiable intermediate layer separates language
correctness from target-specific code generation.

**Deferred**

Moon containers, signature verification, standalone MoonRuntime loading, and hotspot JIT are
not yet Alpha contracts.

## D002: Metadata, Selector, and declaration identity are separate

**Decision**

- Metadata is structured declaration data and does not change a function type by default.
- Candidate discovery and candidate selection are separate steps; a selector works only within
  a type-correct candidate set.
- Ordinary metadata does not participate in declaration identity; distinguishing declaration
  families requires an explicit discriminator/selector rule.
- Version, channel, and `latest` are selection strategies expressible by Core/standard
  library, not hard-coded into the type system.

**Rationale**

Putting a version or label into a function name, function type, or symbol identity confuses
discovery, compatibility, and linking. A separate protocol permits static selection with no
runtime cost while retaining one candidate model for explicit dynamic selection.

**Deferred**

Stable Runtime Descriptors, registry lifetime, dynamic metadata schemas, and plugin unloading
remain planned capabilities.

## D003: Type domains, structural shape, and nominal identity

> 0.3 update: the relation split remains valid, but named structs/enums are now nominal by
> default. See C008/TY002 in the 0.3 overall design.

**Decision**

- Distinguish Value, Meta, Compiler/Inference/Error, and related type domains.
- TypeId is language identity, ShapeId is structural shape, and ABI compatibility is a third
  relation.
- Named structs/enums have nominal declaration identity by default. Structural comparison is an
  explicit ShapeId/constraint relation; `nominal` is not a declaration modifier in 0.3.
- Type, layout, and ownership are separate dimensions.

**Rationale**

One notion of “type equality” cannot support structural generics, nominal safety, reflection,
and ABI checks at once. Separating the relations lets each boundary state which proof it needs.

See the [type-system reference](reference/type_system.md) and
[builtin type inventory](reference/builtin_types.md) for exact rules and implementation matrices.

## D004: Ownership relation and usage cardinality are orthogonal

**Decision**

- Owned, SharedBorrow, and MutableBorrow describe the relation between a value and storage.
- Copy, Affine, and Linear describe how many times a value may be consumed.
- Borrowing applies to Places; a Place consists of a root binding plus field, index, and
  dereference projections.
- Cleanup obligations are explicit in MoonIR and verified on every reachable exit path.

**Rationale**

“Owned” does not mean “must be used exactly once.” The orthogonal model expresses owned
values that may be discarded but require destruction, events that must be consumed, and
borrows that own no resource.

**Boundary**

Multi-execution contexts, loops, and GPU in-flight state must prove consistent usage and loan
state on every path. Generic recursive Drop is complete; general heap-owning containers still
await stable element move-out/initialization tracking and mutable-view invalidation rules.

## D005: `Result`, `?`, and abort-style `panic`

**Decision**

- `Result<T, E>` is an inline ADT with an active tag and owns only the active variant payload.
- `?` unwraps on success and returns early on failure; the error path performs the same
  cleanup as explicit `return`.
- Alpha `panic` is an unrecoverable abort boundary, not an exception or algebraic effect.
- `never` is a control-flow bottom type, not an instantiable ordinary value.

**Rationale**

Recoverable failure must be visible in function types and control flow. Unrecoverable failure
needs a simple, predictable boundary that does not bypass ownership-cleanup proofs.

See the [error-model contract](reference/error_model.md) for current conversion limits and
the status matrix.

## D006: Standard errors and external boundaries use explicit conversion

**Decision**

- Core provides the smallest error abstractions; the standard library defines platform and
  domain errors.
- `From<Source>` conversion is a static, explicitly provable one-hop conversion; there is no
  implicit chain search currently.
- C FFI, Runtime, and GPU status/errno/snapshots must be copied immediately by an adapter into
  an owned error.
- `Result` and standard error ADTs do not cross the C ABI directly.

**Rationale**

Foreign borrowed error strings, errno, and backend state have different lifetimes. Implicit
promotion hides ownership and failure origin; an explicit adapter provides a stable ABI.

## D007: Fragment control contracts are explicit

**Decision**

- An interceptor continues after normal completion; a context controls its continuation through
  `resume()`; `abort()` discards the continuation.
- Luna 0.3 exposes only single-shot unit-result contracts; `many` is deferred rather than
  inferred from how many `resume()` calls appear.
- Static contexts use canonical stack-scoped continuation regions. The former
  external plugin ABI was deleted rather than extended to contexts.

**Rationale**

Explicit control contracts let ownership checking verify every resume/abort path and avoid
exposing stack continuations as unconstrained shared-library callbacks.

## D008: Runtime ABI is versioned and pay-for-what-you-use

**Decision**

- Allocator, console, error snapshots, GPU, and plugins use versioned C ABIs.
- JIT explicitly registers Luna Runtime symbols with ORC; it does not rely on host-executable
  export policy.
- Installed AOT must explicitly select, or select through the environment, a matching Runtime
  archive and native compiler.
- Programs that do not use Runtime/kernel capabilities should not emit corresponding symbols.

**Rationale**

Source semantics, compiler-internal layout, and host ABI have different stability cycles.
Versioned C boundaries can be tested independently while keeping ordinary-program runtime cost
explainable.

## Change rules

A new decision may enter this document only after implementation, tests, and reference
documents are synchronized. A reversed decision directly updates the current summary and
records migration in `CHANGELOG.md`; detailed discussion remains in Git history rather than
creating a maintained historical document tree.
