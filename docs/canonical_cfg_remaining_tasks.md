# Canonical CFG Remaining Tasks & Known Issues

> Snapshot: 2026-08-26, after the public compile-time query `.optional()`
> slice and overload-identity hardening landed in the working tree.
> This file began as the item-10 canonical-CFG switchover ledger and now keeps
> the canonical-path evidence needed by the remaining 0.3 completion items.
> CompilerPipeline seals canonical CFG unconditionally and the structured
> executable-body backend has been deleted.

## Sealer coverage: complete registered 60-test gate passes

## Remaining tasks (by priority)

### Design decisions (confirmed by project owner)

- **`linear {}` / `affine {}` usage blocks are syntactic sugar**: they only
  constrain the usage of *newly declared* bindings within the block. Borrowed
  bindings (references) are unaffected — a `&T` inside a `linear {}` block
  remains Copy cardinality. The canonical verifier's check
  `relation != Owned → usage == Copy` is correct and should not be relaxed.
  The `usage_blocks` fixture tests this sugar on borrowed bindings; if it
  fails, the fixture or the OwnershipChecker needs adjustment, not the
  canonical verifier.

- **`DeclarationRef` type should be downgraded from compiler-intrinsic to a
  stdlib/core type**: `declaration_of`, `declaration_id`, `declaration_name`,
  etc. should not be compiler builtins. Instead, a `Declaration` type should
  exist as a core or stdlib type that only exists at compile time (erased
  before runtime). This is a larger design change and does not need to be
  fully completed in this session. For now, `static_declaration_reflection`
  remains a known gap.

- **Unreachable CFG blocks from context/fragment should be avoided at
  construction time**, not tolerated by the verifier. The CFG builder should
  not create blocks that become unreachable after a `return` from a context
  continuation. This is part of the slot/fragment canonicalization slice.

### P0 — Blockers for production switchover

1. **"let initializer type disagrees with its canonical local"**
   - Root cause: MoonIR `IdentifierExpr` type is not always set during lowering.
   - **Fixed**: slice, gpu_*, type_* reflection intrinsics, and string/cstr
     coercion now work in the canonical verifier.
   - **Remaining**: none (FFI let-initializer was a string/cstr coercion).

2. **"allocation initializer element disagrees with its frozen layout type"**
   - **Fixed**: `lowerAllocationElements` now resolves element types inline
     from expression structure (MoveExpr → operand, IdentifierExpr → local
     table) before the frozen-layout check. `bindExpr` for `HeapAllocExpr`
     now binds only constructor call arguments, not the struct-type callee.

3. **"call argument type disagrees with its signature"**
   - **Fixed**: integer-to-integer coercion, string/cstr coercion, and
     nominal TypeId propagation through composite types (Function, Closure,
     Array, Reference, RawPointer, Record, Result, Enum, DeviceBuffer) now
     work in the canonical verifier. The root cause was
     `canonicalIdentityImpl` falling back to `canonicalShape` for structural
     composite types, which erased nominal child distinctions (fn(First) vs
     fn(Second) collided on TypeId). Now `canonicalIdentityImpl` recursively
     uses identity for composite children, and Closure identity includes
     capturedFields.

### P1 — Feature gaps (designated later slices)

4. **slot/fragment canonicalization** — RESOLVED for the SF006 0.3 surface.
   Module-level nominal slots and targeted fragments compose lexical
   single-shot interceptor/context regions. Interceptors forward to one
   Continuation region; each supported context `resume()` owns an independent
   region. The earlier dynamic/multi-shot construction coverage is retained as
   migration/rejection corpus, not as a public source path. Positive coverage
   includes `fragments`, qualified package composition, cleanup/control exits,
   nominal shadow identity, and the full showcase under canonical JIT/AOT.
   **Remaining beyond plugin ABI v1**: a persistent continuation callback ABI
   for external plugin contexts/many remains NP004 and requires a separate
   lifetime/authorization design.

5. **materialized recipe full state** — RESOLVED. An owning materialized
   move-only array now installs projected guarded cleanup rows when the recipe
   is created. Unconsumed and conditionally abandoned recipes therefore clean
   every element on early return. Consumption transfers the outer source into
   loop-local guarded state, so the old rows are deactivated before the new
   rows become active. Guarded elements execute in source-index order while
   ordinary locals retain reverse declaration cleanup order.

6. **move-only iterator terminals** — RESOLVED. `for_each`, affine `fold`,
   `collect`, direct terminals and materialized terminals transfer each
   move-only item explicitly and preserve source-tail cleanup on exhaustion,
   rejection and early return.

7. **heterogeneous compute** — RESOLVED for the current simulator/CUDA/ROCm
   surface. Canonical codegen handles kernel locals, launch, moved events and
   `AwaitStmt`; hardware execution remains an opt-in platform gate.

8. **reflection/compile-time** — RESOLVED for the current surface.
   Compile-time-valued reflection calls fold before sealed runtime IR, so
   `DeclarationRef` values do not leak into canonical locals.

### P2 — Cleanup

9. **Delete structured-body codegen implementation** — RESOLVED. Function,
   lambda, and closure codegen require exclusive canonical CFG bodies. The
   statement/continuation/slot/fragment consumers, their build entries, and
   their private fragment/continuation state have been removed. Direct backend
   coverage rejects unsealed structured input.

10. **Remove env-var gate** — RESOLVED. CompilerPipeline always seals and
    verifies canonical CFG before optimization.

   **Switchover evidence (2026-08-22):** The complete 55-test suite passes on
   the unconditional canonical pipeline, including parallel execution. The move-only
   materialized iterator test now proves both JIT/AOT output parity and
   ascending projected-element cleanup. `core-surface` also seals cleanly.

   Static single-shot contexts run under both paths. Dynamic/multi-shot source
   forms are rejected by SF006; plugin ABI v1 remains deliberately
   interceptor-only and NP004 covers any future persistent callback ABI.

### Canonical-path and container status (2026-08-24)

- Full registered non-hardware CTest: 60/60 on the unconditional canonical pipeline.
- Strict-warning build: green.
- Production canonical ASan/UBSan suite: 60/60 green after the public query
  optional slice and overload-identity hardening.
- Slot/Fragment: executable canonical successes for static single-shot
  composition; dynamic/multi-shot forms and external context callbacks remain
  outside the 0.3 surface.
- Structured executable-body consumer: deleted; item 10's one-way backend
  switchover is complete. Item 11's eight-section serializer, bounded parser,
  concrete reachability projection, atomic verified loader, deterministic
  mutation gate, independent Python oracle, and persistent coverage-guided
  fuzz corpus/harness are implemented. Item 11 is closed.
- Item 12 target-boundary slice: `-t cffi` now accepts only manifest-declared
  library packages with a non-empty, exclusively explicit `export "C" fn`
  surface. It emits a platform shared library and a sealed-MoonIR-derived C
  header; a strict C11 consumer compiles, links, and executes against the real
  symbols. Application, standalone, ordinary Luna export, empty export, and
  Native-library downgrade cases fail closed. T003 formal-package-only build
  enforcement and the `build/native`, `build/moon`, and `build/cffi` default
  output split are complete. Native library proof emission is recorded below;
  the verified loader is now also complete, so item 12 is closed.

  **Native proof and loader update (2026-08-24):** a
  Native library carries one pointer-free v1 record in a platform binary
  section. Canonical SHA-256 excludes that record and binds every other file
  byte; separate digests bind the sorted typed-export and declared foreign
  dependency sets, alongside package/version, target ABI, and compiler
  identity. The generated `.trust` file is only an installation candidate and
  is never searched implicitly. Offline explicit-trust verification rejects
  missing proof, byte tampering, target mismatch, and absent trust records.
  The sealer and verifier now independently parse the linker's final ELF,
  Mach-O, or PE dependency table and bind its canonical digest. Native
  libraries also expose a v1 typed descriptor registry bound by the
  export digest; independent code resolves and invokes an entry only after
  checking its SymbolId, ContractId, kind, flags, and linkage. The loader
  captures a private immutable image, verifies and loads that same image, and
  publishes lookups only by SymbolId plus ContractId. Deterministic tests
  replace the original path with a different implementation after verification
  and forge a trust-matching export digest; the staged implementation remains
  selected and the forged registry is rejected. Generation activation belongs
  to the separate evolution-runtime work.
- Item 15 state-machine slice (completed by EV004 on 2026-08-30): EV001–EV004 have executable
  generation identities, retained module leases, verify/resolve/initialize
  staging, one-use safe points, atomic activation, pinned references,
  compatible switchable bindings, and rollback. Initializer and binding
  failures preserve the active generation; concurrent readers see only whole
  generations across repeated transitions. Real verified Native libraries now
  enter this state as executable lease-backed bindings, and verified Moon
  Containers enter with retained ORC JIT function bindings plus descriptor-backed
  non-function exports. A real 60 -> 13 -> 60 Moon switch/rollback preserves both
  generation leases. The installed C++17 host API now fixes the public control
  plane without adding Luna evolution syntax or an activation CLI.
- Item 13 compile-time foundation: each `SemanticContext` analysis now owns one
  immutable, validate-once catalog snapshot keyed by canonical
  SymbolId/ContractId/TypeId and family SymbolId. It projects every stable
  source declaration kind represented in canonical MoonIR: functions with
  resolved signatures, impl methods, fragments, structs, enums, traits,
  implementations, and metadata schemas. Drop-bearing rows close the drop-glue
  dependency to strong IDs, and an integration oracle compares every projected
  row with sealed MoonIR. Named constraints, compiler intrinsics, and
  body-generated generic instances remain outside the source snapshot.
  Typed finite sets implement phase/kind/family/type/exact-metadata filtering
  and order-independent `.one()` and `.optional()`. The first public function
  surface is `symbols::<Signature>(family).matching(metadata).one()` with a
  locally bindable, non-iterable compiler-domain `symbol_set<T>` and a bindable,
  callable `declaration_ref<T>` result. Function declaration discrimination now
  combines metadata with a normalized source signature, so heterogeneous
  overloads sharing identical metadata retain distinct deterministic linkages
  and stable source-derived SymbolIds while exact duplicates still fail closed;
  extending a family does not rewrite an existing SymbolId. Public `.optional()` now yields a
  compiler-domain Core `Option<declaration_ref<T>>`: exhaustive `None`/`Some`
  matching is checked in Sema and lowering retains only the statically selected
  arm. Local rebindings preserve the static choice; payloads cannot escape
  through ordinary call or return boundaries. Query state erases before MoonIR,
  whose verifier independently rejects a forged leaked Option specialization,
  and lowering rechecks the selected strong SymbolId/ContractId. Cross-program
  snapshots also prove that adding a sibling overload preserves the existing
  DeclarationId, SymbolId, and ContractId. Q004 is now frozen: `.all()` uses canonical
  SymbolId byte order, while `.all::<M>()` uses a validated unique lexicographic metadata key;
  both produce a statically iterable compiler-domain view that erases before MoonIR. Static `select_unique`
  composes candidate restriction, metadata filtering, and `.one()`, preserving
  distinct no-match/ambiguity failures; static selectors use this catalog, and lowering checks the selected identity
  against the sealed declaration table before publishing a reference. The set
  and evaluator erase before MoonIR. `symbols(Name)` now covers the
  source-name-bearing Fragment, Struct, Enum, Trait, and MetadataSchema kinds;
  unspecialized generic nominals fail closed and internal Implementation rows
  remain catalog-only. Item 13 is closed. Item 16 subsequently removed the
  former dynamic exact-match path and reserved its old artifact tags as invalid.

The canonical codegen now resolves all local-reference paths through the
LocalId-indexed tables (mCanonicalLocals / mCanonicalLocalTypes): generateCall
(closure/bindable callables), generateBorrow (IdentifierExpr, IndexExpr with
Array and Slice sources, Reference/RawPointer/DeviceBuffer locals),
generateAddrOf, generateAssign (FieldAccessExpr record field assignment),
and generateControlFlowBody (closure env parameter loading, AwaitStmt). The
backend entry accepts only canonical CFG bodies. Some shared expression and
iterator helpers still carry name-keyed fallback storage internally; it is not
a second executable-body path. Removing that compatibility state requires a
separate proof that every post-CFG expression form has a LocalId-native lowering.

## Known issues (non-security, design-boundary)

- **String/CStr cleanup no-op**: `typeRequiresCleanup(String)=true` but string literals are global constants. Value cleanup is a no-op in `emitOwnedPayloadCleanup`, `emitCleanup`, and the canonical `emitCanonicalCleanup`; allocation cleanup remains active. JIT/AOT and ASan/UBSan regressions cover a local string literal. When the stdlib introduces heap-allocated text, this must be revisited.
- **Dead loads at -O0**: String cleanup early-return generates unused `CreateLoad` instructions. Harmless; eliminated at `-O2+`.
- **Canonical path duplicate intrinsic sets**: **Fixed**. The intrinsic name
  sets in `ControlFlowBuilder::bindExpr`, `Verifier::scanGraphIdentifier`,
  and `hoistOrderedOperand` are now unified into a single shared function
  `moon::isCompilerIntrinsicName()` declared in `MoonIR.h` and defined in
  `MoonIR.cpp`.
## Completed slices (this session)

1. Repository hygiene (.gitignore for DSH tool dirs)
2. Non-Copy closure capture — structured path (C016 CL010)
3. Non-Copy closure capture — canonical CFG path (env param usage, FreeStmt redirect, return cleanup, MakeClosureExpr transfer)
4. Copy array for-each — canonical CFG (identifier type resolution in parseIteratorRecipe)
5. Capturing-closure iterator adapters — canonical CFG (Closure type accepted in map/filter)
6. Compiler intrinsics — canonical CFG (bindExpr + verifier intrinsic sets)
7. Iterator terminal closures — canonical CFG (for_each/fold accept Closure)
8. String/CStr cleanup crash fix (pre-existing defect)
9. File-guide cleanup (remove stale REFACTOR_SPLIT_PLAN.md)
10. Sealer pipeline gate, followed by unconditional canonical switchover
11. Moon Container serialization, independent oracle, mutation gate, and fuzzing
12. CFFI shared-library/header slice with a real C consumer and fail-closed target matrix
