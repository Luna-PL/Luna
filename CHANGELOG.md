# Changelog

## 0.3.0 — Development

- Enforced the confirmed T003 formal-build boundary. Artifact-producing
  `luna build` now requires a directory with `luna.package` and an explicit
  application/library kind; standalone files remain available to `check`,
  `run`, and `analyze`. Native applications emit under `build/native`, Moon
  Containers under `build/moon`, and CFFI libraries under `build/cffi`, with
  `-o` retaining full-path override semantics. Legacy single-file AOT tests
  now stage temporary application packages instead of weakening the driver.

- Implemented the first fail-closed CFFI artifact slice. `luna build <package>
  -t cffi` now accepts only manifest-declared library packages with a non-empty
  surface made exclusively of `export "C" fn`, emits the platform shared
  library plus a sealed-MoonIR-derived `<name>.h`, and uses the real link
  symbols in that header. A strict C11 consumer compiles, links, and executes
  against the generated artifact. Applications, package `main`, standalone
  files, ordinary Luna exports, and empty C export sets are rejected. Native
  library builds now fail explicitly while proof-section emission is absent,
  preventing an accidental downgrade to an unproved executable or library.

- Froze the M005 Moon Container wire boundary and implemented its hardened
  binary framing layer. The reader/writer now enforce the eight required
  sections, fixed little-endian header/directory fields, ascending IDs,
  8-byte zero padding, no compression, SHA-256 integrity, optional-section
  skipping, and parser byte/section limits. Deterministic and malformed-input
  coverage is included. The manifest and complete TypeRecord/sysmeta payload
  codecs now enforce fixed-width fields, UTF-8 and resource bounds, canonical
  TypeId order, and strict enum/boolean values; an independent decoded Module
  is re-materialized and verified. Symbol, contract, and sysmeta sections are
  normalized by SymbolId and atomically reconstruct declaration records,
  schemas, typed metadata constants, and canonical contracts. Import/export
  sections now retain package edges, manifest-authorized typed host
  capabilities, and root-package public declarations without consulting the
  AST. The explicit 5-operation/28-expression code codec now round-trips
  sealed CFG tables with a 256-level nesting bound. Whole-container loading is
  atomic and runs the MoonIR verifier before publication; a production
  frontend -> container -> loader -> LLVM JIT test preserves behavior.
  `luna build <package> -t moon [-o path]` now emits a deterministic,
  host-specific, self-verified `.moon` artifact. Its verified concrete
  projection removes generic recipes and unresolved/transitively generic types
  while retaining monomorphized instances; an entry/export/host/runtime-rooted
  closure now removes unreachable concrete code and model rows while preserving
  direct and dynamic callees, Drop glue, fragments, metadata, and type edges.
  The loader reconstructs that closure before publication, closing an
  authenticated missing-TypeRef path found by mutation testing. Canonical
  verification now also requires every literal to carry an existing,
  category-correct frozen type; dynamic dispatch synthesis labels its runtime
  name literals as `cstr`. Exported generic APIs remain an explicit error.
  Standalone input, invalid package entrypoints, and native
  linker options are rejected. Package manifests provide the application/library
  kind and version. A standalone Python conformance oracle now parses every
  field of two real CLI products without linking the Luna reader, and the model
  gate exercises raw-bit, truncation, and re-authenticated payload mutations
  with failure-atomic and byte-canonical postconditions. An opt-in Clang
  libFuzzer target now combines raw mutation with a SHA-repairing custom
  mutator, a protocol dictionary, and a reproducibly generated corpus that
  includes real CLI products. Its harness checks framing/model failure
  atomicity and canonical re-encoding; the framing reader now publishes its
  version and sections only after every check succeeds.

- Deleted the unreachable structured executable-body backend. Function,
  lambda, and closure codegen now require an exclusive canonical CFG body;
  the old statement/continuation/slot/fragment consumers and their private
  state are gone. A direct backend regression proves that unsealed structured
  input is rejected before LLVM lowering while sealed input still JITs.

- Completed canonical finite-linked context and multi-shot composition.
  Runtime-selected contexts now lower each `resume()` to an independent
  Continuation region; replay-safe `many` contexts, statement-form apply,
  runtime selection inside loops, and the full showcase work in canonical
  JIT/AOT. Unknown context/many candidates still abort instead of crossing the
  interceptor-only external plugin ABI v1. Added positive dynamic multi-shot
  selection and negative replay-unsafe ownership coverage. The newly reachable
  showcase also fixed nested short-circuit normalization and Copy-field moves
  from affine aggregates.

- Made canonical sealing unconditional in CompilerPipeline and removed the
  `LUNA_SEAL_CANONICAL` production gate. Registered tests now assert canonical
  IR directly; the legacy structured codegen deletion is recorded above. The
  conservative O3 four-way unroll
  hint moved to eligible canonical backedges so the switchover does not regress
  the CPU reduction optimization contract.

- Closed the canonical move-only materialized-iterator lifetime gap. Owning
  recipes now install outer projected guarded cleanups at materialization, so
  abandonment and conditional early return release every source element.
  Starting a terminal or `for` transfers the source into loop-local guarded
  state, preventing duplicate obligations. Projected array elements clean in
  ascending source order while ordinary locals retain reverse declaration
  cleanup. The complete 51-test suite now passes with
  `LUNA_SEAL_CANONICAL=1`.

- Removed temporary `into_iter`/verifier stderr tracing and added a regression
  proving that an ordinary user function named `into_iter` leaves successful
  diagnostics clean. The package-export ABI test now stages its package under
  the build tree, and the repository inventory test runs serially, eliminating
  the observed parallel CTest race over source-tree AOT artifacts.

- Added AwaitStmt handling to canonical CFG codegen. The canonical
  generateControlFlowBody handled LetStmt, FreeStmt, AllocateStmt, and
  ExprStmt operations but not AwaitStmt, so heterogeneous programs with
  `await done;` failed with "canonical CFG operation is outside the
  initial LLVM slice". generateControlFlowBody now emits the
  rt_gpu_await_event call and GPU failure check for AwaitStmt, matching
  the structured path. heterogeneous, heterogeneous_move_event, and
  heterogeneous_versioned now run correctly under
  LUNA_SEAL_CANONICAL=1. Canonical scan improves from 38 to 41 fully
  successful runs; codegen errors drop from 6 to 3.

- Fixed canonical closure environment parameter loading in
  generateControlFlowBody. A capturing closure's env parameter arrives as
  a pointer to the env struct ({ptr, i32}), but the canonical local is
  typed as the Closure struct value. The parameter store previously wrote
  the pointer directly into the struct alloca, leaving the captured-field
  half uninitialized and producing garbage values. generateControlFlowBody
  now loads the struct from the env pointer before storing it. Capturing
  closures in iterator adapters (map/filter with captures, for_each) now
  produce correct results under LUNA_SEAL_CANONICAL=1. All remaining
  canonical-path crashes are now by-design aborts (panic,
  result_unwrap_panic, reflection_index_out_of_range) or the known
  heterogeneous bulk-transfer GPU gap.

- Extended generateBorrow to handle slice-typed sources. A BorrowExpr wrapping
  an IndexExpr into a slice source previously only handled Array types,
  falling through to generateIndex (which loads the element value) for Slice
  sources. This caused a type mismatch (storing i32 into a ptr alloca) and
  SIGSEGV for slice-iteration patterns like `for value in slice { print(*value); }`.
  generateBorrow now loads the slice value, extracts its data pointer and
  length, bounds-checks the index, and returns the GEP element pointer.
  iterator_slice now produces the correct result (3) matching the structured
  path. Canonical scan crashes drop from 5 to 4.

- Resolved canonical-CFG borrowed locals in generateBorrow. The BorrowExpr
  codegen resolved borrowed locals only through the structured-path name map
  (mLocals/mLocalTypes); under LUNA_SEAL_CANONICAL=1 the canonical CFG uses
  LocalId-indexed tables (mCanonicalLocals/mCanonicalLocalTypes). A borrowed
  IdentifierExpr carrying a LocalId but no mLocals entry fell through to
  generateExpr, loading the value instead of returning the address, causing a
  type mismatch and SIGSEGV for iterator map/filter adapters that take &i32.
  generateBorrow now falls back to canonical local tables and handles
  BorrowExpr wrapping IndexExpr by emitting the GEP element pointer.
  iterator_materialized now runs correctly under the canonical gate; remaining
  iterator-pipeline cases no longer crash (adapter logical correctness is a
  separate issue). Canonical scan improves from 37 to 38 fully successful runs;
  crashes drop from 7 to 5.

- Resolved canonical-CFG local callables in codegen generateCall. The
  canonical CFG seals function bodies into ControlFlowGraphs whose local
  bindings use LocalId references, not the structured-path name map
  (mLocals). generateCall previously checked only calleeRef and mLocals,
  rejecting a sealed CFG CallExpr whose callee carried a LocalId but no
  calleeRef and no mLocals entry. It now falls back to mCanonicalLocals /
  mCanonicalLocalTypes and the rejection guard accepts a structured local,
  canonical local, or verified declaration. Closures, lambda captures,
  usage blocks, dynamic select, versioning, and trait versioning programs
  now run under LUNA_SEAL_CANONICAL=1. The canonical scan improves from 33
  to 36 fully successful runs; codegen errors drop from 28 to 6.

- Connected the canonical CFG to dynamic single-shot interceptor apply
  (SF005). A dynamic slot interceptor declaration with a dynamic apply
  carrying a finite candidate set now seals into a verified canonical CFG
  instead of being fail-closed. Each candidate fragment body is cloned
  into its own Fragment region; the runtime selects among statically
  linked candidates via rt_dynamic_fragment_select /
  rt_dynamic_fragment_matches, a matched interceptor forwards to a shared
  Continuation region, and an unknown runtime name calls
  rt_dynamic_fragment_report_unknown_and_abort. lowerApply records
  candidate sets in a new mDynamicApplyScopes stack;
  lowerDynamicSlotInvoke builds the dispatch graph with Branch
  terminators on per-candidate match results. At this intermediate slice,
  runtime context and multi-shot composition remained fail-closed; the later
  finite-linked continuation work above closes that gap. The
  moonir_canonical_test runtime-boundary test was then
  updated with a positive test that
  verifies the interceptor path seals with 2 Fragment + 1 Continuation
  regions. Sealer coverage on valid programs rose from 90/93 (97%) to
  91/93 (98%). The 51-test CTest suite, strict-warning build, and
  ASan/UBSan build all remain green.

- Folded compile-time-valued reflection calls into MoonIR literals at lowering time. A DeclarationRef let binding is already skipped by lowering, but a reflection call referencing it reached sealed IR as a CallExpr with no canonical local. Lowering now folds any call whose compileTimeValue is set into the corresponding literal. static_declaration_reflection now seals; coverage 89/93 to 90/93.

- Completed the materialized recipe full state slice for the canonical CFG.
  Guarded array cleanup state (design doc S784-806) now extends from the
  direct-for path to materialized consuming for-loops, all terminal kinds
  (count/for_each/fold), and inline consuming terminals over move-only
  arrays. The materialized for-loop reuses a fresh Copy cursor in the loop
  scope (initialized to zero) while the source local stays in the recipe's
  outer scope; two verifier scope checks are relaxed, guarded-tail-only, to
  accept a child-scope cleanup/cursor targeting a parent-scope owning source.
  A count terminal now emits an implicit per-iteration drop for move-only
  items it does not consume. An unconsumed materialized iterator binding
  drops its owning source array whole at scope exit. Terminal for_each/fold
  contracts accept affine item parameters, passing items as MoveExpr. The
  allocation-initializer type resolver now recurses through BinaryExpr,
  FieldAccessExpr, and EnvLoadExpr. Canonical Sealer coverage on valid
  programs rose from 87/93 (94%) to 89/93 (96%). The 51-test CTest suite,
  moonir-canonical-test, and every iterator_*_invalid negative fixture
  remain green under the gate.

- Fixed nominal TypeId propagation through composite types in
  `canonicalIdentityImpl`: structural composite types (Function, Closure,
  Array, Reference, RawPointer, Record, Result, Enum, DeviceBuffer)
  previously fell back to `canonicalShape` for their identity, which erased
  nominal child distinctions. Two functions `fn(First)` and `fn(Second)`
  collided on the same TypeId even though `First` and `Second` are distinct
  nominal types, causing "conflicting frozen payloads" or "call argument
  type disagrees" in the canonical verifier. `canonicalIdentityImpl` now
  recursively uses identity for composite children, and Closure identity
  includes capturedFields. This matches the language design: named types are
  nominal by default, and a composite type's identity must distinguish
  nominal children even when their structural shapes coincide. Sealer
  coverage on valid programs rose from 76/93 (82%) to 77/93 (83%).
- Fixed intrinsic call type resolution for Sealer canonicalization: the
  `slice`, `gpu_*`, and `type_*` reflection intrinsics previously returned
  their result type from Sema without storing it in `call->resultType`, so
  MoonIR lowering produced CallExpr nodes with empty types. The canonical
  verifier then rejected `let` bindings initialized by these calls with "let
  initializer type disagrees with its canonical local". Sema now sets
  `call->resultType` for `slice`, `gpu_alloc_i32`, `gpu_free`,
  `gpu_copy_from_host_i32`, `gpu_copy_to_host_i32`, `gpu_load_i32`,
  `gpu_store_i32`, and all `type_*` reflection intrinsics (in
  `CompileTimeEvaluator::analyzeReflectionCall`), matching the pattern
  already used by `pointer_cast`, `drop_callback`, and `range`. Additionally,
  `MoveExpr` now carries its operand's type in MoonIR lowering, fixing
  "call argument type disagrees" for move-qualified call arguments. The
  canonical verifier now tolerates integer-to-integer coercion (i32 literal
  to usize/i64 parameter) and string/cstr coercion, matching the structured
  backend's implicit conversions. Sealer coverage on valid programs rose
  from 63/93 (68%) to 76/93 (82%).
- Connected the Sealer to the production compiler pipeline behind an
  environment-variable gate: when `LUNA_SEAL_CANONICAL=1` is set,
  `CompilerPipeline` calls `Sealer::sealFunctionBodies` after MoonIR
  verification and before optimization, converting structured function bodies
  into canonical CFGs. The gate defaults to off, preserving the structured-body
  backend for all existing tests. Sealer failures produce clear "moon-seal"
  stage diagnostics. This is the first step toward the item-10 production
  switchover: it enables incremental testing of the canonical CFG path on real
  programs without committing to the one-way module switch. A new canonical
  test exercises the pipeline gate end-to-end and verifies that a sealed
  module contains canonical CFGs.
- Enabled Sealer canonicalization of iterator terminals with closure
  callbacks: `for_each` and `fold` terminals previously required their
  callable to be `TypeKind::Function`, rejecting `TypeKind::Closure` (lambda
  callbacks) with "for_each action disagrees with its Copy terminal contract"
  / "fold reducer disagrees with its accumulator ownership contract". Both
  terminal checks now accept Closure alongside Function, matching the
  map/filter adapter relaxation. New Sealer tests cover for_each and fold
  with lambda callbacks end-to-end.
- Enabled Sealer canonicalization of programs using compiler intrinsics: calls
  to `print`, `panic`, `slice`, `new`, `free`, `clone`, `type_*` reflection
  builtins, `Ok`/`Err`/`is_ok`/`is_err`/`unwrap`/`unwrap_err` result builtins,
  `pointer_cast`/`drop_callback` FFI builtins, and `gpu_*` heterogeneous
  compute builtins previously failed canonical CFG construction with
  "identifier has no canonical local or declaration reference" because these
  builtins have no MoonIR declaration table row. `ControlFlowBuilder::bindExpr`
  now skips the local/declaration lookup for CallExpr callees whose names match
  the known compiler-intrinsic set, and the canonical verifier's
  `scanGraphIdentifier` likewise skips the DeclarationRef requirement for
  intrinsic identifiers. The intrinsic sets are kept synchronized between
  ControlFlowBuilder and Verifier. This unblocks Sealer canonicalization for
  the majority of production programs that use `print` for output.
- Enabled Sealer canonicalization of iterator adapters with capturing closures:
  map/filter adapters that capture Copy locals (e.g. `map(fn(x: &i32) -> i32
  { return *x + offset; })`) were previously rejected in the canonical CFG
  with "iterator map/filter requires one canonical capture-free callable".
  Two restrictions are relaxed: `parseIteratorRecipe` now accepts Closure
  types (not just Function) for the callable, and `validateIteratorRecipe`'s
  `commonCallable` check allows Copy-capture closures alongside capture-free
  functions. A new Sealer test covers end-to-end canonicalization of a
  capturing-closure map pipeline.
- Enabled Sealer canonicalization of Copy-array for-each loops: a simple
  `for v in array_binding` previously failed canonical CFG construction
  with "materialized iterator recipes require the later canonical subphase"
  because the MoonIR IdentifierExpr for the array source lacked an inline
  type (lowering does not always set it), so `parseIteratorRecipe` could
  not determine the source kind. The parser now resolves the source type
  from the canonical local table when the identifier's type is empty,
  letting direct Copy-array for-each enter the consuming-array recipe path
  and produce a verified canonical CFG. A new Sealer test covers
  end-to-end canonicalization of a Copy-array for-each with accumulation.
- Closed the canonical-CFG gap for Non-Copy closure capture (C016 CL010):
  the Sealer can now build a verified canonical CFG for a lambda that
  captures an Affine or Linear binding. Three interacting fixes were
  required. (1) The environment parameter's usage is now derived from the
  closure type's frozen resource usage instead of being hardcoded to Copy,
  so an Affine closure environment does not weaken its frozen usage
  requirement. (2) When the implicit FreeStmt for a captured binding is
  rewritten to an EnvLoad operand, canonical construction redirects the
  cleanup to the environment parameter's own cleanup obligation instead of
  reporting "implicit cleanup does not reference a canonical local"; the
  return-path cleanup obligations for captured bindings are similarly
  redirected, and the environment parameter's cleanup is explicitly added
  to every return edge. (3) The canonical verifier's path-sensitive
  transfer function now models MakeClosureExpr: each captured value is
  consumed (its cleanup state deactivated) when it moves into the closure
  environment, matching the OwnershipChecker's outer-binding consumption.
  A new Sealer test covers an Affine (string) capture end-to-end through
  canonical CFG construction and verification.
- Implemented Non-Copy closure capture (C016 CL010): a capturing lambda can
  now move an Affine or Linear outer binding into its closure environment.
  Sema records each capture's original usage, registers it as an owned local
  inside the lambda scope, and consumes the outer binding so it is
  unavailable after the closure is constructed. `defaultUsageForType` now
  treats a Closure with Non-Copy captured fields as Affine. The MoonIR
  verifier no longer rejects non-Copy or cleanup-bearing capture fields.
  Codegen recursively cleans closure environment fields through a new
  Closure case in `emitResourceContentsCleanup`. A pre-existing defect
  where string/cstr literals were passed to `rt_dealloc` during scope-exit
  cleanup is fixed: string and cstr cleanup is now a no-op, since string
  literals are immutable global constants. Positive Affine move capture,
  negative use-after-move, and the full 51-test suite pass under the
  strict-warning build and ASan/UBSan.
- Extended the benchmark surface to 20 CPU workloads and a heterogeneous
  scale sweep. New CPU dimensions (divmod, chase, stream read/write/copy,
  saxpy, sort, hash, find, recursion, rotate) separate scalar compute from
  memory behavior, latency chains and vectorization; input arrays are shared
  bit-for-bit between Luna and C++23 through `tools/gen_cpu_bench_sources.py`.
  The heterogeneous suite generates 8 MiB-1 GiB kernels with 1x/4x/16x
  compute-intensity variants, transfer roundtrips and launch-overhead
  microbenchmarks (`tools/gen_heterogeneous_scale.py` +
  `benchmarks/run_heterogeneous_scale.sh`), and `cpp23_hip_vector.cpp` now
  takes the same parameters from argv while keeping the legacy 64 MiB/29524
  contract. Gap attribution ships as `tools/benchmark_analyze.sh` (LLVM
  IR/asm static comparison, vectorization remarks, startup decomposition,
  optional llvm-mca) and `tools/benchmark_probe.py` (getrusage resource
  sampling, optional perf stat).
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
