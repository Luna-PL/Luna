# Migrating Luna 0.2 Source to 0.3

English | [简体中文](migration_0.2_to_0.3.zh-CN.md)

> Document category: migration record
> From: Luna 0.2.1
> To: candidate Luna 0.3.0
> Status: Draft; target spellings identified by `TBD-*` are intentionally not invented here
> Frozen 0.2 source/compiler checkpoint: `a188d87a6f10d7fa67582389a0a0b915f3741401`

Luna 0.3 is a clean break. The 0.3 compiler has no package language selector, edition,
compatibility flag, or legacy lowering path. Keep a 0.2 compiler for unchanged 0.2 source and
migrate a package as one explicit operation.

## Breaking-change register

| Area | 0.2.1 source/model | 0.3 source/model | Why it changes | Migration action / status |
|---|---|---|---|---|
| Compiler selection | the 0.2 compiler accepts the 0.2 language | the 0.3 compiler accepts only 0.3 | two semantic tracks would permanently enlarge the compiler | pin the old compiler until the package is migrated; do not add `language=` |
| Phases/retention | CompileTime, Runtime, and Dynamic retention; `dynamic select`, `dynamic slot`, and `dynamic apply` | compile-time/runtime only; runtime behavior uses values, descriptors, queries, and builtins | Dynamic mixes retention, discovery, and dispatch | remove Dynamic forms when their replacements are frozen; query ordering is `TBD-Q004` and Slot/Fragment source spelling is `TBD-SF006` |
| Named type default | named struct/enum are structural unless declared `nominal` | named declarations have nominal TypeId by default; `nominal` is no longer a keyword; anonymous records use `{ field: Type }` / `{ field: value }` with no `record` keyword | stable package/runtime identity cannot depend on accidental layout equality, and a redundant modifier would preserve needless grammar | rewrite `nominal struct/enum` as plain `struct/enum`; replace implicit cross-named conversion with `Target { field: value }`, project with `{ field: source.field }`, or prove an explicit constraint/shape relation; `TY002` is confirmed |
| Ownership usage | binding-level `affine`/`linear` declarations | prefix `copy let`/`affine let`/`linear let` contracts plus implemented `affine {}`/`linear {}` default blocks | block sugar reduces resource-code noise without adding runtime state | move any post-`let` qualifier to the prefix; optionally group bindings in a usage block; Sema erases the block policy after fixing and validating bindings |
| Rc/Arc | compiler-special `rc new T(...)`, `arc new T(...)`, and special TypeKinds | ordinary nominal `Rc::new(value)`/`Arc::new(value)` containers implementing Resource/Drop | removes cross-compiler special cases and lets libraries own container policy | construct library containers explicitly and clone handles explicitly; no compatibility desugaring |
| Slot/Fragment | function-local slot statements, structural contracts, `apply name(fragment)` and dynamic finite candidates | module-level second-class slot identity, nominal Fragment target, static MoonIR composition, typed RuntimeFragmentRef | local strings and structural shapes cannot support safe open-world extension | old forms are rejected; migrate after `TBD-SF006` freezes exact declarations and control behavior |
| MoonIR | pointer-heavy in-memory IR; `--emit-moonir` is a diagnostic output | one canonical table-referenced MoonIR with in-memory and serialized forms | Moon Containers require stable identity and verification without a second IR | tooling moves to the canonical model; wire details remain `TBD-M005` |
| Artifact output | build/check flags expose current executable and diagnostic MoonIR paths | manifest `kind` plus confirmed `-t native` (default), `-t moon`, or `-t cffi`; formal build requires a package | one output dimension keeps source semantics independent from packaging | add package `kind`; migrate output paths to confirmed `T003`; use `export "C" fn` for CFFI and `[host-imports]` for Moon foreign dependencies |
| Native/foreign trust | current Runtime/plugin descriptors do not prove an entire native artifact | Moon is locally verified, Luna Native is trusted by a proof bound to code/data, C FFI remains foreign/unsafe | typed headers alone cannot authenticate implementation bytes | rebuild native Luna libraries with proofs; import unproven libraries explicitly through `extern "C"`; there is no general `unsafe {}` |
| Evolution | no Moon Container generation loop | host-specific Moon Container staging, verification, safe-point activation, rollback | evolution must be explicit, verified, and off ordinary call hot paths | public binding spelling waits for `TBD-EV004`; 0.3 does not migrate persistent state |

No row authorizes compatibility code in the 0.3 compiler. Where a destination spelling is TBD,
the correct interim action is to keep using the frozen 0.2 compiler, not to guess the syntax.

## Frozen 0.2 migration corpus

[`tests/migration_0_2_baseline.cmake`](../tests/migration_0_2_baseline.cmake) records ten
representative cases for structural named types, generic structural reuse, dynamic symbol and
fragment selection, local Slot/Fragment/apply, and compiler-special Rc/Arc behavior. It reuses
the existing fixtures so the migration evidence and the full 0.2 regression suite cannot drift
into competing copies.

The normal 0.3 test configuration validates only that the manifest and its source fixtures
still exist. To reproduce 0.2 semantics, build the pinned commit with its own build directory
and run:

```sh
cmake \
  -DLUNA_SOURCE_DIR=/path/to/luna-0.2-source \
  -DLUNA_0_2_EXECUTABLE=/path/to/luna-0.2-build/luna \
  -P /path/to/luna-0.2-source/tests/migration_0_2_baseline.cmake
```

The runner rejects a compiler whose analysis identity is not the pinned commit. This makes the
old evidence independently reproducible without carrying any 0.2 branch inside the 0.3 binary.
