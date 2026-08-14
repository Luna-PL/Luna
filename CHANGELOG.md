# Changelog

## 0.3.0 — Development

- Implemented the Copy-only closure-environment slice (C016 CL001-CL009):
  capture-free `Function` values keep the 8-byte bare code pointer ABI, while
  a capturing lambda becomes a layout-bearing `Closure` type with an inline
  `{ code, env_fields... }` environment. Sema derives the free-variable set in
  first-reference order, rejects Affine/Linear and borrowed captures with an
  explicit diagnostic, and records the capture list in canonical name order.
  MoonIR gains `MakeClosure` and `EnvLoad` nodes; the verifier checks capture
  list/environment agreement, environment parameter identity, EnvLoad field
  bounds, and non-Copy capture fields; canonical CFG construction declares the
  environment parameter local and rewrites capture reads into EnvLoad; both
  JIT and AOT lower the closure call through a hidden environment parameter.
  Nested lambdas propagate their transitive free variables to enclosing
  lambdas, Sealer cloning preserves `MakeClosure`/`EnvLoad` nodes, and closure
  calls carry their resolved result type through lowering so the canonical
  verifier can check them. Copy-capture, multi-capture, nested/partial/
  parameter capture, capture shadowing (block and parameter), closure return
  values, and affine/borrowed-capture rejection all pass as semantic
  regressions and JIT/AOT parity fixtures under the full 51-test suite, the
  strict-warning build, and ASan/UBSan.
- Hardened recursive analysis against pathological nesting: structured-body
  cloning and capture rewriting in canonical CFG construction enforce a
  maximum nesting depth instead of overflowing the stack, and the parser
  rejects grouped expressions deeper than its nesting limit with a clean
  diagnostic. Iterator adapters now accept capturing closures in addition to
  plain functions (map/filter/fold/for_each lower the closure call through its
  environment), and the adapter diagnostic distinguishes a non-callable
  argument from an arity mismatch. A transient moon-verify failure observed
  during QA did not reproduce across 50+ reruns; type identity uses
  deterministic FNV-1a hashing and ordered traversal, so no non-determinism
  source was found and the observation is recorded without a code change.
- Extended the LLVM CFG consumer with projected/guarded cleanup, canonical
  allocation/free handling, pointer-backed allocation locals, LocalId-based
  array access, guarded non-Copy cursor advancement, and projection-aware
  return-transfer verification; strict audit coverage remains green at 51/51
  tests. Production CFG sealing stays disconnected, and
  fragment/runtime composition remains deferred until upstream non-Copy
  match/loop sealing gaps are resolved.
- Continued the strict CFG audit through non-Copy match/loop sealing: fixed
  Copy projections being mistaken for whole-value moves, recovered delayed
  iterator source types, and lowered direct affine-array recipes through
  synthetic ownership transfer into guarded projected cleanup.
- Extended the same guarded ownership path to move-only materialized iterator
  recipes, including affine source state, synthetic transfer into the loop, and
  projected tail cleanup; audit-only AOT execution now covers both direct and
  materialized affine array loops.
- Kept the external fragment-plugin ABI interceptor-only at the dynamic dispatch
  boundary: unknown selections for dynamic context or multi-shot slots now
  terminate through the explicit unknown-fragment runtime error instead of
  falling through to the plugin ABI.

- Began the clean-break 0.3 language line with no `language=`, edition, or
  structural-default compatibility mode; the frozen 0.2.1 compiler remains
  the migration tool for old source.
- Made every named struct and enum nominal by default, removed the redundant
  `nominal` keyword/modifier, and kept explicit `type_same_shape` queries for
  structural relations.
- Added keyword-free anonymous record types and values, explicit named
  `Target { field: value }` construction, canonical field identity/layout,
  source-ordered evaluation, recursive owned-field cleanup, and grammar-context
  separation from statement blocks.
- Added C++-style constrained parameters and inline `where` predicates as
  frontend sugar over the existing compile-time constraint evaluator, without
  an effect mechanism or a MoonIR node.
- Added zero-cost `affine {}`/`linear {}` binding-default blocks and explicit
  `copy let` overrides. Sema fixes local, pattern, and loop usage contracts,
  rejects weakening, and erases the lexical policy before MoonIR.
- Split Sema behind component-scoped capabilities and introduced distinct
  shadow TypeId, ShapeId, SymbolId, ContractId, and AbiLayoutId identities plus
  verified sysmeta projections.
- Added compiler-derived generic Resource/Drop contracts, recursive exact-once
  cleanup for named/anonymous/array/active-ADT payloads, and Runtime-callable
  type-erased Drop glue.
- Replaced compiler-special `rc new`/`arc new`, Rc/Arc TypeKinds, cleanup nodes,
  and legacy Runtime entries with ordinary nominal Core `Rc<T>`/`Arc<T>`,
  explicit `Clone`, and Runtime ABI v1 shared cells; 0.3 now rejects the old
  syntax instead of desugaring it.
- Made `type_size::<T>()` report the actual value-slot ABI and added
  `type_alignment::<T>()`, so generic library storage uses the same layout as
  MoonIR/codegen.
- Reclassified the published toolchain/Lunax ecosystem lock as a frozen 0.2.1
  migration baseline; it is no longer a release-compatibility claim for the
  in-progress 0.3 compiler.

## 0.2.1 — Prerelease

- Made the embedded compiler commit follow Git HEAD/ref changes during incremental
  builds and added a release-gated identity regression that rejects stale binaries.
- Added an immutable cross-repository ecosystem candidate lock and verifier for exact
  LunaToolchain/Lunax commits, protocol compatibility, and compiler source identity.
- Defined the 0.3-oriented standard-library package graph and minimum IO, file,
  Vec, String, concrete-error, and supporting Core surfaces, with explicit
  resource prerequisites and a staged implementation boundary.
- Completed the standard-library Stage A foundation: centralized legacy 0.2
  and canonical 0.3 Core protocol identities, added Sys/Alloc package
  skeletons, specified borrowed byte/text views, and appended compatible
  console-input and filesystem capabilities to Runtime ABI v1.
- Added the explicit native application-host profile for generated JIT/AOT
  entry points, with stdin, UTF-8 paths, opaque filesystem handles, partial
  I/O, metadata, seek/sync, structured errors, and embedding-host precedence.
- Added fixed Runtime I/O forwarding entries and real `org.luna.sys::console`
  / `fs` raw modules, keeping service-table layouts and native descriptors out
  of future safe standard-library code.
- Added checked array layouts and caller-owned fallible allocator ABI entries,
  with allocation-free errors, zero-size allocation semantics, transactional
  realloc failure, JIT bindings, a raw `org.luna.sys::alloc` bridge, and strict
  plus sanitizer regressions.
- Added a deliberately temporary 0.2.1 `std::io` surface for cstr/i32
  stdout/stderr output, flush, raw byte I/O, bounded lossy line input, and
  fallback-based i32 parsing; unsupported builtin `print` payloads are now
  rejected instead of being lowered as accidental pointers.
- Established the A0 documentation contract, 0.2 Alpha semantic baseline,
  exhaustive built-in/internal type inventory, and a frozen error-model
  reference that separates language guarantees from implementation ABI and
  planned adapters.
- Established A1 release hygiene: removed tracked build outputs, ignored
  repository-local `build-*` trees, added an isolated installed-tree JIT/AOT
  smoke test, and enabled strict-warning plus ASan/UBSan Linux CI gates.
- Consolidated the active documentation from 57 Markdown files and 9,297 lines
  to 33 files and about 4,100 lines: current semantics remain in
  `docs/reference`, architecture rationale is summarized in two documents, and
  superseded drafts, duplicate roadmaps and temporary status pages now rely on
  Git history instead of a maintained archive tree.
- Reorganized the project entry documentation into an English default README and a linked Simplified Chinese README, with feature, Hello World, build, CLI, platform, roadmap, and topic-oriented documentation entry points.
- Added the versioned, C-compatible Runtime ABI v1 with replaceable host allocator and console services, exact `size/alignment` allocation lowering, foreign-resource carriers, and an optional W^X executable-memory capability reserved for MoonRuntime/JIT hosts.
- Added caller-owned Runtime error snapshots with stable GPU/plugin domain and code fields, allocation-free diagnostic copying, and compatibility coverage for the legacy borrowed `last_error` views.
- Moved compiler-generated `new`, automatic cleanup, explicit `free`, and language `print` behind Luna runtime symbols; kept layout-less `rt_malloc/rt_free` only as an Alpha compatibility bridge for previously emitted IR.
- Verified the stable JIT/AOT/runtime boundaries on Linux, macOS, and Windows UCRT64, including explicit ORC symbol registration and parameterized AOT process launching.
- Separated reverse-DNS Package IDs from `::` module/submodule identities, added `using <Package ID> as <alias>` dependency edges, and preserved the package/module graph in verified MoonIR.
- Added strict Alpha TOML schemas for `luna.package`, `luna.workspace`, and `luna.lock`, local workspace Package ID resolution, manifest source roots, a no-codegen `luna check` command, and logically independent `org.luna.core`/`org.luna.std` packages.
- Added recursive locked-workspace dependency loading, `alias::module::symbol` and `module::symbol` resolution, package-private export enforcement, and deterministic linkage isolation for same-named declarations in different modules.
- Added an executable two-package, multi-module language showcase and a portable MoonIR/JIT/AOT integration test covering the complete positive Alpha feature surface.
- Materialized solved omitted/`auto` signature types before MoonIR lowering, restoring verified JIT/AOT code generation for inferred function and closure signatures.
- Replaced placeholder-based generic-body cloning with exhaustive AST cloning,
  recursive type substitution and source-location preservation; nested generic
  calls no longer rewrite source identifiers to internal instance symbols.
- Split the REPL from the driver, routed in-memory submissions through the
  production compiler pipeline, and defined a tested Alpha contract for i32
  expressions, persistent one-line declarations and isolated statements.
- Added a tag-gated prerelease workflow that tests and publishes checksummed
  Linux x86_64, macOS runner-architecture and Windows UCRT64 x86_64 prebuilt
  archives with bundled LLVM compiler runtime dependencies.
- Added one authoritative repository file guide with directory dependency
  rules, per-production-file responsibilities, a complete path inventory and a
  CTest gate that rejects unregistered files.
- Removed the final legacy build-tree compatibility executable/symlink; build,
  install, documentation and release surfaces now use only `luna`.
- Declared `0.2.1` a long-lived maintenance line with no scheduled Beta
  or language-version bump, and shifted near-term development toward
  diagnostics, editor/build/package integration, distribution and reliability.
- Added a bounded O3 four-way unroll hint for medium-sized straight-line,
  call-free while loops, with positive reduction coverage and a nested-loop
  exclusion regression.
- Decoupled the installed-tree AOT smoke linker from the compiler used to
  build Luna, allowing GCC-hosted builds to validate textual LLVM IR with a
  separately discovered or configured Clang driver.
- Added a retained frontend analysis snapshot for tooling consumers and routed
  the production compiler pipeline through the same package, semantic, trait,
  and ownership result without changing MoonIR or code-generation behavior.
- Added a deterministic read-only declaration index with versioned Symbol IDs,
  typed signatures, package/module identity, visibility, and source-name
  locations for language-server consumers.
- Added the `luna.analysis` v1 JSONL declaration producer and `luna analyze`
  command, including stable symbol records, byte-exact selections, partial
  snapshot summaries, and an end-to-end protocol regression.
- Added compiler-resolved direct-call references mapped to stable Symbol IDs;
  the analysis protocol now exposes the first definition-safe reference class
  without relying on source-name matching.
- Added compiler-resolved user trait-method references with exact member-name
  spans, mapped to the selected impl method's stable Symbol ID and advertised
  independently through the `method-references` capability.
- Added resolved user type and trait references for type syntax, impls, and
  bounds, advertised independently through `type-references` and
  `trait-references` and consumed by language-server definition requests.
- Advertised reverse package reference queries through the explicit
  `package-references` capability, using stable Symbol IDs across declaration
  selections and all emitted reference classes.
- Added stable child Symbol IDs for struct fields and enum variants, exact
  references for field access, variant construction, and qualified match
  patterns, and the `field-references`/`enum-variant-references` capabilities.
- Added a deliberately limited Luna 0.2.x package rename in `luna-lsp`, using
  complete analysis snapshots for functions, methods, named types, traits,
  fields, and enum variants without committing to the planned 0.3 syntax.
- Added package-aware single-document source overlays to `luna analyze` via
  stdin, preserving package/module identity and computing protocol byte spans
  against the in-memory UTF-8 snapshot rather than stale disk contents.
- Added versioned `luna.overlay` JSON stdin envelopes for atomic multi-document
  package analysis, while retaining the original single-document `--overlay`
  transport for compatible tooling clients.

## 0.1.0-alpha — Development baseline

- Inserted verified MoonIR between the typed Luna frontend and both LLVM AOT/JIT paths.
- Replaced compiler-defined version tags with first-class Metadata, static/dynamic Selector operations, and runtime descriptors.
- Split Selector, Instantiator, PackageManager, and MacroProcessor into independent compiler components.
- Added audited cost reporting, exact generic instance IDs, reachable-only kernel emission, and `--reserve-kernel-runtime`.
- Split Value/Meta/Compiler type domains, made structs/enums structural by default with explicit `nominal` declarations, and added stable TypeId/ShapeId relations plus a verified MoonIR type table.
- Stabilized explicit package exports and C FFI; split ownership relation from Copy/Affine/Linear usage, added Place-based partial moves/borrows, and emitted verified MoonIR cleanup obligations.
- Added JIT/AOT parity coverage at `-O0`, `-O2`, and `-O3`, plus reproducible AOT runtime-library selection.
- Made JIT runtime resolution platform-independent with explicit ORC symbols, split explicit `--gpu-target` code-object generation from runtime-only `LUNA_GPU_BACKEND`, and replaced shell-based AOT linking with parameterized process execution.
- Added CPU simulator regression coverage, CUDA PTX/ROCm HSACO paths, observable GPU launch/event failures, bulk i32 transfer ABI, and optional ROCm JIT/AOT smoke testing.
- Added Linux CI, installation guidance, package documentation, Alpha limitations, and benchmark methodology.
- Added the Alpha v1 external fragment-plugin ABI for host-only single-shot interceptors, including contract validation and dynamic dispatch tests.
- Added release metadata, `luna --version`, installation staging checks, a root README, and a release checklist.
- Added dual MIT / Apache-2.0 licensing and documented the post-Alpha development roadmap.
- Added the Luna-PL project branding, portable LLVM CMake target discovery, and a lightweight CPU baseline benchmark.
