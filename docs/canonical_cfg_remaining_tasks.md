# Canonical CFG Remaining Tasks & Known Issues

> Snapshot: 2026-08-15, after the Sealer pipeline gate landed.
> All items below are 0.3 item-10 canonical-CFG switchover work.
> Default path (structured body) remains production; `LUNA_SEAL_CANONICAL=1` gates the canonical path.

## Sealer coverage: 85/93 valid programs pass (91%)

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

4. **slot/fragment canonicalization** — context_*, fragments, dynamic_fragments, external_fragment_dispatch all fail. Design doc lists this as a separate slice.

5. **materialized recipe full state** — iterator_materialized, iterator_materialized_move_only, iterator_move_only_array fail on move-only consuming source cleanup state.

6. **move-only iterator terminals** — for_each_move_only, fold_move_only require projected source cleanup state.

7. **heterogeneous compute** — gpu intrinsics now resolve, but heterogeneous example programs fail on kernel/launch CFG construction.

8. **reflection/compile-time** — compile_time, type_relations, type_domains_reflection now pass. static_declaration_reflection still fails (declaration_of intrinsic doesn't set call->resultType for DeclarationRef return type).

### P2 — Cleanup

9. **Delete structured-body path** — only after all P0/P1 items close and the full 51-test suite passes under `LUNA_SEAL_CANONICAL=1`.

10. **Remove env-var gate** — flip default to sealed, remove the `LUNA_SEAL_CANONICAL` check.

## Known issues (non-security, design-boundary)

- **String/CStr cleanup no-op**: `typeRequiresCleanup(String)=true` but string literals are global constants. Cleanup is a no-op in `emitOwnedPayloadCleanup` and `emitCleanup`. When the stdlib introduces heap-allocated text, this must be revisited.
- **Dead loads at -O0**: String cleanup early-return generates unused `CreateLoad` instructions. Harmless; eliminated at `-O2+`.
- **Canonical path duplicate intrinsic sets**: `ControlFlowBuilder::bindExpr` and `Verifier::scanGraphIdentifier` maintain separate `unordered_set<string>` of intrinsic names. These must be kept in sync manually. A future refactor should extract a single shared constant.
- **env-var gate TOCTOU**: `std::getenv` is called once and the result is checked immediately. No security impact (compiler flag, not auth), but a race could theoretically flip the value between read and use. Acceptable for a development gate.

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
10. Sealer pipeline gate (LUNA_SEAL_CANONICAL env var in CompilerPipeline)
