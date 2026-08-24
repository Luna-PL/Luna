# src/sema/Inference.h — Constraint Solver (Type Inference)

> In one sentence: `ConstraintSolver` is Sema's dedicated type-inference state machine: it allocates inference variables, performs unification (unify), tracks numeric/boolean constraints, and finally defaults unconstrained numeric variables to i32.

## What This File Does

Luna's type inference follows the "constraint solving" style: when it encounters `let x = 1 + y` without knowing the types of `x`/`y`, it first creates an inference variable (similar to Hindley–Milner's `?T`), then unifies the two sides, and finally concretizes any variables that are still left dangling. This file declares `ConstraintSolver`, which is the core of this entire mechanism.

Note: inference variables (`TypeKind::InferenceVar`) are not legal MoonIR types. The Moon verifier rejects any front-end specialization residue that slips through, so `SemanticContext::analyze` must run the `defaultUnconstrainedNumeric()` and `checkUnresolved` cleanups before it finishes.

C++ analogy: this is like the solver for "placeholder types to be deduced" (`auto` / `decltype(auto)`) in C++ template deduction: variables start out unresolved, get progressively narrowed by constraints, and finally land on a concrete type.

## Key Structs, Classes, and Enums

- `class ConstraintSolver`: the core class; its private members are the solver state:
  - `int mNextId`: the id of the next inference variable (monotonically increasing).
  - `unordered_map<int, TypePtr> mBindings`: inference variable id → bound type (which may itself point to another inference variable, forming a chain).
  - `unordered_map<int, bool> mNumericConstraints`: records that an inference variable has been required to be numeric (flagged by `requireNumeric`).
  - `unordered_map<int, bool> mBoolConstraints`: records that an inference variable has been required to be bool (flagged by `requireBool`).

## Key Functions and Methods

Public interface (called by `TypeResolver`):

- `TypePtr fresh()`: creates a new inference variable (`Type::makeInferenceVar(mNextId++)`).
- `TypePtr resolve(type)`: recursively expands the binding chain, replacing inference variables with their final types; it also resolves the member types of intermediate nodes (`inner`, `typeArgs`, `fields`, etc.), ensuring the result is a fully "expanded" tree.
- `bool unify(lhs, rhs, reason*)`: unifies by recursively comparing the two trees; it handles inference-variable binding, occurs-check (`contains` detects recursive constraints), numeric/bool constraint propagation, nominal type id comparison, and structural comparison of record/enum/function/slot/fragment, etc. On failure it writes the reason into `reason`.
- `void requireNumeric(type)` / `requireBool(type)`: if the target is an inference variable, flags it as numeric/bool (validation of concrete types is left to `TypeResolver` so it can produce diagnostics with source locations).
- `void defaultUnconstrainedNumeric()`: iterates over all allocated ids and binds inference variables that still carry `mNumericConstraints` and remain unresolved to `TyI32` (the deterministic default for numeric literals).
- `bool hasUnresolved(type)`: recursively checks whether the type tree still contains unresolved inference variables (used by `checkUnresolved` to report "cannot infer" errors).

Private implementation: `unifyInternal` (the actual unification algorithm), `contains` (occurs-check), and `collectUnresolvedNumeric` (collects unresolved numeric variables).

## Relationship to Surrounding Files and Pipeline Stages

- `TypeSystem.h`'s `#include "Inference.h"` shows that inference lives together with the type graph; `SemanticContext` holds a `ConstraintSolver mConstraints`.
- `TypeResolver` (`TypeResolver.cpp`) is the main caller: `fresh()` for `declaredType`/`auto`; `unify` for `constrain`; `requireNumeric`/`requireBool`/`requireInteger` for operator checks; `resolve` for reading values.
- `SemanticContext::analyze` calls `defaultUnconstrainedNumeric()` during its finalization phase, runs `checkUnresolved` on each `mInferenceRoots` entry, and then `materializeInferredTypes` writes the concrete types back into the AST.
- Inference variables must never leak into MoonIR: the Moon verifier is the last line of defense.

## Further Reading

- `TypeSystem.cpp`: the complete implementation of `ConstraintSolver` (`resolve`/`unifyInternal`/`contains`/`collectUnresolvedNumeric`, etc.).
- `TypeResolver.cpp`: `constrain`/`requireNumeric`/`requireBool`/`requireInteger`/`checkUnresolved`/`materializeInferredTypes`.
- `SemanticContext.h`: the `mConstraints`, `mInferenceRoots` fields.
- Textbooks: type inference (HM style), constraint solving, and occurs-check.


---

---
kind: source-file-guide
module: sema
source: src/sema/OwnershipChecker.cpp
lang: en
audience: Readers who know C/C++ (familiar with move semantics/smart pointers) and want to read Luna's ownership checker implementation.
---
