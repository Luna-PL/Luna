# Standard-Library Skeleton

The standard library is currently logically independent inside the main repository's
`stdlib/` workspace. It will move to a separate repository after package, Moon-container,
and Runtime ABI contracts are frozen.

```text
stdlib/
├── luna.workspace
├── luna.lock
├── core/   # org.luna.core
└── std/    # org.luna.std -> org.luna.core
```

`org.luna.core` and `org.luna.std` are independent packages, not subpackages. `core`
contains types and operations that do not require host/OS services; `std` may use I/O,
allocation, and other host capabilities through the Runtime ABI.

`org.luna.std` reserves a real `io` module, but does not yet wrap the compiler builtin
`print` as a library function. Before doing so, format-string checking, the
`Format`/`Scan` traits, static/dynamic formatting cost boundaries, and the error-return
type must be frozen.

The error layer uses “concrete error plus static `From` aggregation.” Core has materialized
host-independent `ErrorCode`, `InvalidArgumentError`, `BoundsError`, `UtfError`,
`LayoutError`, and `AllocError`. They use the 0.2 compiler/MoonIR inline ADT v1
internal layout, which is not a C FFI commitment. Language-level boundary errors that depend
on volatile error text are not public yet. Runtime provides a caller-owned
`domain/code/message` snapshot and requires machine fields to be retained while owned
diagnostic text may be omitted if allocation fails. The next step is to materialize safe
adapters and concrete error types in Std rather than exposing compatibility-only
`last_error` pointers directly. See the [error-model reference](reference/error_model.md)
and [architecture decision D006](decisions.md).

The first iteration stage is already fused directly by the compiler for arrays, slices, and
`range`; `Vec` need not become a builtin. Core has materialized `Option`, `Iterator`,
`IntoIterator`, `FromIterator`, and `Map`/`Filter`/`Take` adapters. Static member
calls for user impls, the Core Iterator `for` protocol, and normal/early-return cleanup
for protocol move-only items are complete. `for` also materializes the unique Core
`IntoIterator` implicitly and cleans hidden state. Consuming array recipes use per-element
initialization bits; move-only `filter`/`take` and move-only `map` inputs/outputs have
rejection, truncation, and early-return cleanup. Lambda bodies use path-sensitive ownership
checks, and `fold`/`for_each`/`count` materialize move-only terminal-recipe state;
affine move-only fold accumulators use independent replacement initialization bits.
Capture-free adapters support affine local stack materialization and remain statically fused.
Move-only owning recipes use hidden source snapshots and per-element initialization bits for
consumed, unconsumed, and early-return cleanup. Terminal `collect::<Target>()` uses the
Core `FromIterator` static `begin/push/finish` builder protocol, without a cross-ABI
iterator or intermediate container; JIT/AOT covers fused Copy and move-only items,
truncation, and tail cleanup. Remaining work is to freeze internal Drop state as a
cross-function Core adapter layout, complete closure environments, and support generic impl
specialization. See the current [iteration, pipeline, and container boundary](iterators.md).

Stackless coroutines and concurrency keywords are not current standard-library prerequisites;
design them only after the Core types, Drop, and error adapters above are stable.
design them only after the Core types, Drop, and error adapters above are stable.
