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

Define first-class, ownership-aware error propagation before stabilizing file,
I/O, allocation and package APIs. `Result`-style values, cleanup on error paths
and FFI/runtime error conversion must agree in Sema, MoonIR, JIT and AOT.

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
