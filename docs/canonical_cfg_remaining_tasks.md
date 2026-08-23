# Canonical CFG Remaining Tasks & Known Issues

> Snapshot: 2026-08-22, after the canonical-only backend and Moon Container
> concrete-projection boundary landed.
> All items below are 0.3 item-10 canonical-CFG switchover work.
> CompilerPipeline seals canonical CFG unconditionally and the structured
> executable-body backend has been deleted.

## Sealer coverage: complete registered 55-test gate passes

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

4. **slot/fragment canonicalization** — RESOLVED for the linked 0.3 surface.
   Dynamic interceptor and context applies select from finite, type-checked,
   statically linked candidates through `rt_dynamic_fragment_select` /
   `rt_dynamic_fragment_matches`. Interceptors forward to one shared
   Continuation region; each context `resume()` clones an independent region,
   including replay-safe multi-shot contexts. Statement-form apply is scoped
   alongside lexical bindings. Unknown context/many candidates abort instead
   of entering the interceptor-only external plugin ABI v1. Positive coverage
   includes `fragments`, `dynamic_fragments`, `loop_plugins`, dynamic
   multi-shot selection, replay-unsafe ownership rejection, and the full
   showcase under canonical JIT/AOT.
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

   Finite linked runtime contexts and replay-safe multi-shot fragments now run
   under both paths. NP004 is narrowed to persistent continuation callbacks
   crossing the external plugin boundary; plugin ABI v1 remains deliberately
   interceptor-only.

### Canonical-path and container status (2026-08-22)

- Full registered CTest: 56/56 on the unconditional canonical pipeline.
- Strict-warning build: green.
- Production canonical ASan/UBSan suite: 56/56 green after the unconditional
  switchover, linked context/multi-shot slice, and CFFI artifact boundary.
- Runtime contexts/multi-shot: executable canonical successes for finite
  linked candidates; external plugin callbacks remain outside ABI v1.
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
  output split are complete. Native library proof emission and the
  corresponding loader remain open; item 12 is not closed.

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

- **String/CStr cleanup no-op**: `typeRequiresCleanup(String)=true` but string literals are global constants. Cleanup is a no-op in `emitOwnedPayloadCleanup` and `emitCleanup`. When the stdlib introduces heap-allocated text, this must be revisited.
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
