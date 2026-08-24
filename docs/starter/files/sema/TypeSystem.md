# src/sema/TypeSystem.cpp — Type AST Translation and Constraint Solving Implementation

> In one sentence: this file implements two core capabilities — translating a type AST directly into a `Type` (`resolveType`) and the complete inference algorithm of `ConstraintSolver` (resolve/unify/defaulting).

## What This File Does

The first half is the global function `resolveType`: it constructs a `Type` purely from the syntax of the type AST, recognizing only built-in type names (`i32`, `Result`, `raw`, `array`, `slice`, `device_buffer`, etc.) and the supplied `typeBindings` — it does **no symbol table lookup** (that is `TypeResolver`'s job). This is essentially the "deserialization" of the type tree.

The second half is the complete implementation of `ConstraintSolver`: the constraint-solving engine for type inference, responsible for binding inference variables, recursive expansion (resolve), unification (unify), and defaulting of unresolved numeric variables.

C++ analogy: `resolveType` ≈ converting a `std::variant`/syntax tree into a runtime type object; `ConstraintSolver::unify` ≈ performing pattern-matching-style structural comparison of two type trees and establishing substitutions when placeholders are encountered.

## Key Structs, Classes, and Enums

No new types are added; the structures live in `Inference.h`/`core/TypeSystem.h`. There are only two implementation bodies here: `resolveType` (a free function) and the member functions of `ConstraintSolver`.

## Key Functions and Methods

`resolveType(ast, bindings)`:

- `RecordTypeAST` → `Type::makeRecord` (fields translated recursively).
- `NamedTypeAST`: first looks up `typeBindings` (type parameter substitution); then built-in names (integer/float/bool/string/cstr/unit/never/Self); then special syntax types: `raw<T>` (`makeRawPointer`), `Result<T,E>`, `device_buffer<T>`, `array<T,N>`, `slice<T>`, `event`, `metadata_view<M>`, `declaration_view`, `declaration_ref`; finally falls back to `Type::makeStruct(name)` + recursive typeArgs (the definition in the symbol table is not known at this point and is filled in by a later stage).
- `RefTypeAST` → `makeReference(inner, isMutable)`; `LinearTypeAST`/`AffineTypeAST` merely strip the shell and return the inner type; `FunctionTypeAST` → `makeFunction`.
- Unrecognized types return `TyUnknown`.

`ConstraintSolver`:

- `fresh()`: `Type::makeInferenceVar(mNextId++)`.
- `resolve(type)`: unwinds the binding chain and recursively expands `inner`/`typeArgs`/`paramTypes`/`fields`/`variants`/`capturedFields`, etc.; includes path compression (writing the end of the chain directly back into intermediate nodes).
- `contains(type, id)`: the occurs-check, which detects whether an inference variable appears inside a tree (to avoid recursive types).
- `unifyInternal(lhs, rhs, reason*)`: the core unification:
  - if either side is an inference variable: occurs-check, numeric/bool constraint validation and merging, and binding establishment;
  - if both sides are concrete types: compare domain and kind (record/struct can be treated as interchangeable product types); for nominal types (`IdentityMode::Nominal/MetaSchema`) compare `nominalId` and typeArgs; for references compare mutability; for wrappers such as raw/device_buffer/metadata_view/iterator compare `inner` (iterator also compares `iteratorMode`); for `Result` compare both typeArgs; for record/struct compare fields one by one by name and type; for enum compare variants; for function/slot/fragment compare continuationKind/multiShot/parameters/return.
- `unify(lhs, rhs, reason*)`: the external shell, delegating to `unifyInternal`.
- `requireNumeric`/`requireBool`: flag inference variables (validation of concrete types is done in TypeResolver).
- `collectUnresolvedNumeric`: recursively finds unresolved inference variables carrying the numeric flag and binds them to `TyI32`.
- `defaultUnconstrainedNumeric()`: runs the above collection for every allocated id — numeric literals default to i32.
- `hasUnresolved(type)`: recursively checks whether unresolved inference variables still remain.

## Relationship to Surrounding Files and Pipeline Stages

- `TypeSystem.h` provides declarations, and `Inference.h` declares `ConstraintSolver`; this file implements them.
- At the end of `SemanticContext::analyze`, `mConstraints.defaultUnconstrainedNumeric()` is called, followed by `checkUnresolved`.
- `TypeResolver::resolveTypeAST` is the version with symbol table resolution; its built-in name part largely reuses the logic here, but it first consults `mSymTable`/`mDeclaredTypes`.
- `TraitChecker` and others use `resolveType` for purely syntactic translation.

## Further Reading

- `Inference.h` (the constraint solver interface), `src/core/TypeSystem.h` (the `Type` type itself).
- `TypeResolver.cpp`: the resolution path that actually connects the symbol table and the type system.
- Textbooks: Hindley–Milner type inference, unification algorithms, occurs-check.


---

---
kind: source-file-guide
module: sema
source: src/sema/TypeSystem.h
lang: en
audience: Readers who know C/C++ and want to understand Luna's type system
---

# src/sema/TypeSystem.h — Semantic Analysis Type Entry Point and Inference Bindings

> In one sentence: the header file that glues together the "canonical type graph (Core)" and the "inference solver (Sema)" — the type graph itself lives in Core, while inference belongs to Sema.

## What This File Does

This is Sema's main type entry point — only a few lines, but dense with information: it draws a clear architectural boundary — the canonical type graph (`Type` / `TypeKind` / `Type::makeXxx` / `TyI32`, etc.) is **not owned by Sema** but is defined in Core (`src/core/TypeSystem.h`), so that MoonIR never depends on Sema headers. What Sema adds on its side is "inference and constraint solving" capability, namely `ConstraintSolver` in `Inference.h`.

## Key Structs, Classes, and Enums

- This file has no types of its own; it pulls them in via `#include "../core/TypeSystem.h"`:
  - Factories such as `Type` / `TypePtr` / `TypeKind`, `Type::makeStruct`/`makeEnum`/`makeFunction`/`makeInferenceVar`.
  - Global singleton types `TyI32`/`TyF64`/`TyBool`/`TyUnit`/`TyNever`/`TyUnknown`, etc.
  - Helper structures such as `TypeVec` (`vector<TypePtr>`), `TypeField`, `TypeVariant`.
- Sema's exclusive `ConstraintSolver` (inference variables, unify, numeric/boolean constraints) is brought in via `#include "Inference.h"`.

## Key Functions and Methods

This file has no function implementations; `TypeSystem.cpp` provides two global entry points:

- `TypePtr resolveType(const TypeAST*, const unordered_map<string, TypePtr>& bindings)`: translates a type AST directly into a `Type` (no symbol table / name resolution — only built-in names and structure).
- All of `ConstraintSolver`'s methods (`fresh`/`resolve`/`unify`/`requireNumeric`/`requireBool`/`defaultUnconstrainedNumeric`/`hasUnresolved`).

## Relationship to Surrounding Files and Pipeline Stages

- `SemanticContext.h` holds a `ConstraintSolver mConstraints` and performs defaulting and unresolved checks at the end of `analyze()`.
- `TypeResolver` (`TypeResolver.h/.cpp`) is the main consumer of `ConstraintSolver` and the real "type resolver": it calls `resolveTypeAST`/`resolve`/`constrain`, etc., to complete name resolution + inference.
- The pure translation (no symbol table) from top-level type AST → `Type` is handled by `TypeSystem.cpp::resolveType`; the version with name resolution lives in `TypeResolver.cpp::resolveTypeAST`.
- Independent checkers such as `TraitChecker` also call `resolveType` directly.

## Further Reading

- `src/core/TypeSystem.h`: the canonical type graph itself.
- `Inference.h` / `TypeSystem.cpp`: the inference and `resolveType` implementations.
- `TypeResolver.h/.cpp`: the complete type resolution flow.


---
