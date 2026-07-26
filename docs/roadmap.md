# Luna roadmap

[English summary](roadmap.md) | [中文详细路线](evolution_roadmap.md)

Luna's next phase prioritizes a coherent, testable language foundation over
rapid expansion of syntax. This page is the short English roadmap; the
[detailed evolution roadmap](evolution_roadmap.md) and
[post-Alpha plan](future_roadmap.md) retain the full design notes.

## Immediate priorities

### 1. Dynamic & runtime descriptor re-design

We will clarify and partition the capability sets provided by the runtime and dynamic modifiers, and refactor the current compiler design.

### 2. Design of a more comprehensive reflection capability

We will comprehensively refine the design of the reflection mechanism to better support our type system architecture. Furthermore, we will enable the processing of language objects with attached metadata by reflection, thereby further eliminating dependencies on selectors. Selectors will explicitly serve as user-defined or open-ended operational tools, whereas reflection will function as a lower-level primitive. For language objects with closed boundaries or fully determined states, this structural shift will yield significantly better performance.

### 3. Error handling before a broad standard library

The first executable slice is now present: `Result<T, E>`, intrinsic
construction/inspection, a general tagged-union payload layout, formal `never`,
ownership-aware `?`, recursive active-payload cleanup, exhaustive enum/Result
matching, static `From<Source>` conversion and abort-style `panic` agree in
Sema, MoonIR, LLVM JIT and AOT. Inline ADT ABI, cross-package trait
coherence/orphan rules, Core value errors, `Option`, and Iterator adapters are
now materialized. User trait member calls use static impl symbols, Core
`Iterator` drives protocol `for` loops, and move-only protocol items are
cleaned exactly once on normal and returning paths. `for` now owns implicit
Core `IntoIterator` state, while consuming arrays and fused `filter`/`take`
carry per-element drop state. Lambda bodies and move-only terminal recipes now
participate in that model, including affine fold accumulator replacement state.
No-capture recipes, including recipes that own move-only array sources, now
materialize as single-consumption stack values without losing fusion. Owning
recipes use per-element initialization bits on consumption, abandonment and
early-return paths. `collect::<Target>()`
now lowers through one coherent Core `FromIterator` begin/push/finish builder
implementation without materializing an iterator ABI or intermediate
collection. Next, map this compiler-internal Drop state to stable cross-function
Core adapter layouts, then define closure environments and the diagnostic/status
ABI before host I/O and boundary error types.

### 4. Package and standard-library foundation

Finish local package workflows and keep `org.luna.core` separate from
host-dependent `org.luna.std`. The first public library surface should include
errors, strings/slices and a high-performance formatted `std::io` design with
clear static/dynamic cost boundaries.

### 5. Ownership and type-system closure

Continue tightening structural/nominal identity, trait coherence, partial
moves, borrows and affine/linear control-flow merging. Static traits and static
selection should retain zero runtime dispatch overhead.

### 6. Moon containers and MoonRuntime boundaries

Freeze the verified Moon container contract, validation rules, capability
descriptors and compatibility fields. Reserve interfaces for safe loading,
runtime JIT adaptation and hotspot evolution without prematurely fixing a full
runtime implementation.

### 7. Heterogeneous generalization

Generalize `device_buffer<T>`, address spaces, launch configuration, queues and
events. Maintain one source-level safety model across the simulator, ROCm and
CUDA while expanding real-device correctness and performance coverage.

### 8. Tooling and ecosystem

Improve diagnostics, formatter and language-server support; add reproducible
package caches and registries; expand fuzzing, sanitizer, cross-platform and
JIT/AOT parity tests.

## Later work

Native structured concurrency remains important, but follows the error,
ownership and standard-library foundations it depends on. External dynamic
contexts, richer C import/callback support and additional accelerator backends
also remain post-foundation work.

## Delivery rule

Every promoted feature must have explicit semantics, positive and negative
tests, JIT/AOT parity, cost documentation and a migration note. Unsupported
dynamic or hardware behavior must fail explicitly rather than silently degrade.
