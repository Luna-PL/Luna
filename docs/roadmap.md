# Luna Roadmap

> Document category: project roadmap
> Applies to: 0.2.0-alpha long-term maintenance line
> Status: Planned
> Normative status: non-normative
> Updated: 2026-07-31

This is the single active roadmap. Planned syntax is not a statement of current compiler
support.

## Project status

- The 0.2.0-alpha line is maintained long term; upgrades do not happen automatically with time.
- The active development line is tooling, not expansion of the language surface.
- Same-version builds are identified by source commit, release manifest, and checksums.
- Beta or a new language version requires a separate decision and a fresh semantic-baseline and
  migration review.

## Principles

- Current capability follows the 0.2 Alpha semantic reference.
- Syntax, types, and APIs listed on this roadmap are not implemented by default.
- Work is limited to correctness, safety, contract gaps, and internal interfaces required by tooling.
- Runtime, Dynamic, and hardware capabilities must be explicitly enabled with explainable cost.
- Code separation may move implementation responsibility but must not change frozen semantics.

## Near term: toolchain

1. Improve stable diagnostic codes, source snippets, fix suggestions, and machine-readable output.
2. Build minimal formatter, language-server, and editor integration.
3. Complete build/test commands, test selection, package/workspace workflows, and local caching.
4. Stabilize installation trees, prebuilt packages, checksums, reproducible builds, and cross-platform release.
5. Provide benchmark, profiling, MoonIR/LLVM inspection, and developer-audit tools.
6. Continue splitting compiler modules only when required at a tooling boundary.

## Maintenance work

1. Keep the type system, builtin types, and error model in single authoritative sources.
2. Maintain implementation boundaries according to the file and responsibility guide.
3. Fix mismatches with frozen contracts and add positive, negative, and migration evidence.
4. Stabilize the Parser, Sema, MoonIR, Codegen, and Runtime interfaces needed by tooling consumers.

## Non-near-term language work

- Define clearer rules for From, Drop, recursive types, and generic boundaries.
- Add more negative coverage for Places, partial moves, borrowing, and control-flow merging.
- Keep Result and ? as explicit recoverable failure and panic as an abort boundary unless a complete RFC changes it.
- Continue separating standard-library types from compiler builtins.
- Extend stable diagnostic numbering to new core errors.

## Standard library and external boundaries

- Establish clear dependency direction among core, alloc/host, and sys.
- Wrap Runtime status, GPU errors, errno, and foreign resources in safe adapters.
- Wait for complete Drop, allocator-domain, and exceptional-path cleanup rules before adding general
  heap-owning containers.
- Define layout, lifetime, and threading boundaries before expanding C struct/union and callback FFI.

## Heterogeneous compute

1. Keep the compiler GPU target separate from the runtime backend setting.
2. Extend device scalar, buffer, and grid surfaces while preserving CPU-simulator parity.
3. Add more CUDA/ROCm hardware matrices, long-running tests, and reproducible performance baselines.
4. Use explicit Runtime and benchmark tools until a language-level profiling API is stable.

## Fragments, Runtime, and Dynamic

- Keep external plugin v1 host-only and single-shot.
- Require plugin v2 to receive an explicitly authorized module context.
- Solve persistent continuation frames and occupancy proofs before external context/resume,
  multi-emission, or capture.
- Give Runtime Descriptor, registry, unload, and reselect explicit lifetimes.
- Keep dynamic reflection, inspect, replace, and runtime weaving outside the stable core until
  capability, safety, and rollback models are complete.

## Packages and distribution

- Add content digests, caching, signatures, and reproducible-build verification.
- Delay remote registries and network dependency resolution until local workspace/lock integrity
  is complete.
- Improve formatter, language server, diagnostic snippets, and test selection.
- Keep release flow gated by strict warnings, ASan/UBSan, installation-tree JIT/AOT, and platform CI.

## Later capabilities

Native concurrency, task-local failure, general async, complete runtime reflection, hot replacement,
and distributed package resolution have no current schedule. Each requires an independent design
and must not enter Alpha through tooling patches.

## Delivery rule

Every roadmap item must state:

- owning layer and static/runtime cost;
- type, ownership, error, and ABI impact;
- positive, negative, and JIT/AOT or hardware evidence;
- authoritative documentation source and migration strategy.
