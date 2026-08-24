# src/sema/TraitChecker.cpp — Implementation of trait consistency checking

> One-line summary: the implementation of `TraitChecker`: registers trait/impl tables, checks `where` clauses, and provides the `satisfies` query.

## What This File Does

Implements the interface from `TraitChecker.h` (about 130 lines). The flow is:

1. `registerTraits`: registers the built-in `Drop` contract and walks `TraitDecl` declarations to build `mTraitSigs` (trait id → method signatures).
2. `registerImpls`: walks `ImplDecl` declarations to build `mImplMap` (traitId → typeId → methodName → implementation pointer).
3. `checkConcreteFunction`: verifies each function's `where T: Trait` clause.
4. `satisfies`: performs lookups against the tables that were built.

Two file-local static helpers: `traitIdOf` (prefers `resolvedTraitId`, falling back to `generatedSymbolName`/`name`) and `typeIdOf` (`luna::types::typeId`).

## Key Structs, Classes, and Enums

Defines no new types; uses `MethodSig`, `mTraitSigs`, and `mImplMap` from the header.

## Key Functions and Methods

- `check(Program*)`: runs `registerTraits` → `registerImpls`; then, for each `FunctionDecl`, sets the diagnostic location and runs `checkConcreteFunction`; returns `mErrors.empty()`.
- `registerTraits`: first writes the signature of `luna::sysmeta::DropTraitId` (`Drop::drop() -> unit`) into `mTraitSigs`; then, for each `TraitDecl`, resolves its methods' parameter/return types.
- `registerImpls`: walks `ImplDecl` declarations; skips those with an empty `traitId` (already diagnosed by semantic analysis); writes `method->name → method` into `mImplMap` keyed by `resolvedTargetTypeId` (using `?` when empty).
- `checkConcreteFunction`: only handles `WhereClause::Kind::TraitBound`; checks in turn that the trait id is resolved, that the type parameter is actually in `decl->typeParams`, and that the trait exists in `mTraitSigs`; each of the three failure modes has its own dedicated error message.
- `satisfies(type, traitName) const`: finds (traitName → typeId) in `mImplMap`; then checks whether the trait's set of method signatures is covered by the implementation; returns true directly when there are no signature requirements.
- `error(msg)`: `diagnostic::format("trait", msg, file, line, col, hint)`.

## Relationship to Surrounding Files and Pipeline Stages

- Runs independently, outside `SemanticContext`; depends on `resolvedTraitId`/`resolvedTargetTypeId` already filled in by semantic analysis (written by `DeclarationCollector`).
- Type utilities come from `src/core/TypeRelations.h` (`typeId`) and `resolveType` (`TypeSystem.cpp`).
- Belongs to the "declaration shape review" category of checks and, like OwnershipChecker, is a second line of defense outside the main Sema pipeline.

## Further Reading

- `TraitChecker.h` (interface), `DeclarationCollector.cpp` (trait/impl registration).
- `satisfiesTrait` in `SemanticContext.cpp` (the primary runtime path for trait queries).
- Language feature background: the trait/constraint design in `docs/reference/`.


---

---
kind: source-file-guide
module: sema
source: src/sema/TraitChecker.h
lang: en
audience: Readers familiar with C/C++ who want to understand Luna's trait checking
---

# src/sema/TraitChecker.h — A trait consistency checker independent of the main pipeline

> One-line summary: `TraitChecker` is a second, independent checker outside Sema: it verifies that `where T: Trait` constraints are legal and provides the `satisfies` query (whether a given type implements a given trait).

## What This File Does

Main semantic analysis (`SemanticContext`) handles type inference and symbol binding; the trait-related "declaration shape" checks live independently in a small utility class, `TraitChecker`. It scans the whole `Program`:

- Registers the method signatures of all traits (`registerTraits`).
- Registers the method mappings of all impls (`registerImpls`).
- Checks the `where` clause of each concrete function (`checkConcreteFunction`): whether the type parameter exists and the trait is known.
- Provides `satisfies(type, traitName)` for other stages to query.

C++ analogy: a static check at the "concepts" level: trait ≈ concept, impl ≈ specialization, `where T: Trait` ≈ a constraint expression.

Note that this is a separate path from `SemanticContext::satisfiesTrait`: this class independently builds its own tables (`mTraitSigs`/`mImplMap`) for independent re-checking.

## Key Structs, Classes, and Enums

- `struct MethodSig`: a summary of a trait method signature: `name`, `paramTypes` (`TypeVec`), and `returnType`.
- `mTraitSigs: unordered_map<string, vector<MethodSig>>`: trait (resolved id) → list of method signatures.
- `mImplMap: unordered_map<string, unordered_map<string, unordered_map<string, FunctionDecl*>>>`: traitId → (typeName → (methodName → `FunctionDecl*`)), isomorphic to `SemanticContext::mImpls`.
- Diagnostic fields: `mErrors`, `mDiagnosticFile/Line/Col`.

## Key Functions and Methods

- `check(Program*)`: the entry point: `registerTraits` → `registerImpls` → walks the function declarations and runs `checkConcreteFunction`; returns `mErrors.empty()`.
- `registerTraits`: first registers the built-in `Drop` (`DropTraitId`, `DropMethodName`, returning `TyUnit`); then walks `TraitDecl` declarations and uses `resolveType` to resolve method signatures into `mTraitSigs`.
- `registerImpls`: walks `ImplDecl` declarations and writes the method pointers into `mImplMap` keyed by `resolvedTraitId`/`resolvedTargetTypeId`.
- `checkConcreteFunction(decl)`: for each `where T: Trait` clause, checks that the trait id is resolved, that `T` is a declared type parameter, and that the trait exists (otherwise reports an error).
- `satisfies(type, traitName) const`: looks up (type id, trait) in `mImplMap`, then verifies that every method name required by the trait is implemented; returns `true` directly when the trait requires no methods.
- `error(msg)`: finishes through `diagnostic::format("trait", ...)`.

## Relationship to Surrounding Files and Pipeline Stages

- Exists in parallel with `SemanticContext`/`SemanticAnalyzer`; `TraitChecker::check` is invoked by the driver outside the full semantic analysis (usually afterward, for independent re-checking) and keeps its own diagnostic list.
- Uses `luna::types::typeId` from `src/core/TypeRelations.h` and `resolveType` (`TypeSystem.cpp`).
- Depends on `resolvedTraitId`/`resolvedTargetTypeId` on `ImplDecl`/`TraitDecl`, already filled in by semantic analysis (written by `DeclarationCollector`).

## Further Reading

- `TraitChecker.cpp` (implementation).
- `satisfiesTrait`/`resolveTraitRef` in `SemanticContext.cpp` (trait logic within the main pipeline).
- `declareTrait`/`declareImpl` in `DeclarationCollector.cpp` (the source of the resolved ids).


---

---
kind: source-file-guide
module: sema
source: src/sema/TypeResolver.cpp
lang: en
audience: Readers familiar with C/C++ who want to read about Luna's type resolution and specialization implementation
---
