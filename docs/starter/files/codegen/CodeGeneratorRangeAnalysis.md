# src/codegen/CodeGeneratorRangeAnalysis.cpp — Implementation of Array Index Upper-Bound Derivation and Safety

## What This File Does

Implements the two pure functions declared in `CodeGeneratorRangeAnalysis.h`: `knownArrayIndexUpperBound` (derives the exclusive upper bound of a Luna index expression) and `isProvablySafeArrayIndex` (decides whether an index is provably no greater than a given length). Purpose: let the code generator omit redundant safe array bounds checks whenever the index is statically provable.

For C++ readers: this is a simplified "range prover" internal to the code generator. It deliberately recognizes only three easily provable shapes — non-negative integer literals, local names with recorded upper bounds, and non-negative masks `x & mask` — and returns "unprovable" for everything else, which is what guarantees safety.

## Key Structs, Classes, and Enums

This file defines no types of its own. It uses `using moon::BinaryExpr; using moon::IdentifierExpr; using moon::IntLiteralExpr; using moon::Operator;` to shorten the AST type names, and implements the two free functions in the `luna::codegen` namespace.

## Key Functions and Methods

**`std::optional<uint64_t> knownArrayIndexUpperBound(const moon::Expr* expression, const std::unordered_map<std::string,uint64_t>& knownUpperBounds)`**
- `IntLiteralExpr` (integer literal): returns nullopt for negative values; for a non-negative `v`, returns `v+1` (exclusive upper bound).
- `IdentifierExpr` (local name): looks up the name in the `knownUpperBounds` map; if found, returns its upper bound.
- `BinaryExpr`: returns `rhs+1` only when `op == Operator::BitAnd` and the RHS is a non-negative `IntLiteralExpr` (because `x & mask` always lies in `[0, mask]`, even when x is signed). All other cases return nullopt.
- Callers: `isProvablySafeArrayIndex`; callees: it only reads the moon::Expr AST and never touches IR.

**`bool isProvablySafeArrayIndex(const moon::Expr* expression, uint64_t length, const std::unordered_map<std::string,uint64_t>& knownUpperBounds)`**
- Calls the function above to obtain the upper bound, and returns whether the bound exists and `bound <= length`.
- Callers: the safe array indexing paths in `CodeGeneratorExpressions.cpp` (generateIndex / related borrow paths), used to skip redundant `rt_array_index_or_abort` runtime checks.

Design note: the comment at the top of the file emphasizes that a non-negative bitwise AND mask has a static upper bound even when its source operand is signed; all other expressions keep their runtime checks. Safety is never derived from an incomplete range proof — a conservative, safety-first stance.

## Relationship to Surrounding Files and Pipeline Stages

- An optimization helper that belongs to the **code generation stage**.
- Depends on `../moonir/MoonIR.h` (`moon::Expr`, `moon::BinaryExpr`, etc.) and the standard library `<optional>`/`<string>`/`<unordered_map>`.
- The header is included by `CodeGeneratorExpressions.cpp`; `CodeGenerator` supplies the map of known upper bounds via `mLocalKnownUpperBounds`.

## Further Reading

1. `CodeGeneratorRangeAnalysis.h` — the function declarations and their semantic comments.
2. `generateIndex`/`generateBorrow` in `CodeGeneratorExpressions.cpp` — safe indexing and bounds checking.
3. `mLocalKnownUpperBounds` in `CodeGenerator.h` — the source of known upper bounds and their invalidation rules.

---

# src/codegen/CodeGeneratorRangeAnalysis.h — Pure-Function Header for Array Index Range Analysis

## What This File Does

`CodeGeneratorRangeAnalysis.h` lets the code generator decide whether an array index is statically provable as safe. It declares two pure functions (in the `luna::codegen` namespace): one derives the upper bound of a Luna expression used as an index, and the other uses that bound to decide whether the index is guaranteed to fall within a given length. The backend uses it to **eliminate redundant runtime bounds checks** and never infers safety from an "incomplete proof".

For C++ readers: this is a typical "compile-time lightweight range analysis" interface. It produces no IR and only answers two boolean questions — "Does this index have a known upper bound?" and "Is it guaranteed not to go out of bounds?" — acting as a query helper for the optimizing middle-end.

## Key Structs, Classes, and Enums

This file contains no structs/classes/enums; it only exposes two namespace-level free functions:
- `std::optional<uint64_t> knownArrayIndexUpperBound(const moon::Expr* expression, const std::unordered_map<std::string,uint64_t>& knownUpperBounds)`: if known, returns the expression's exclusive upper bound (i.e., max value + 1).
- `bool isProvablySafeArrayIndex(const moon::Expr* expression, uint64_t length, const std::unordered_map<std::string,uint64_t>& knownUpperBounds)`: returns true when an upper bound exists and `bound <= length`.

The `knownUpperBounds` parameter is a map from local variable names to exclusive upper bounds, sourced from `CodeGenerator`'s `mLocalKnownUpperBounds`.

## Key Functions and Methods

**`knownArrayIndexUpperBound`** (implemented in CodeGeneratorRangeAnalysis.cpp). It computes an upper bound for three kinds of inputs:
- `IntLiteralExpr`: the bound of a non-negative literal `v` is `v+1`; negative literals return `nullopt`.
- `IdentifierExpr`: looks up an existing upper bound by name in the `knownUpperBounds` map.
- `BinaryExpr` (see the .cpp for details): returns `rhs+1` only when `rhs` is a non-negative literal and the operator is `BitAnd` (bitwise AND), because `x & mask` is guaranteed to lie in `[0, mask]`.
Callers: `isProvablySafeArrayIndex`. Callees: it only reads the AST (`moon::Expr`) and does not use IR.

**`isProvablySafeArrayIndex`**: calls the function above to obtain the upper bound and compares `bound <= length`. When it returns true, the caller may omit the runtime bounds check.
  Callers: the safe indexing-related paths in `CodeGeneratorExpressions.cpp` (used to skip redundant `rt_array_index_or_abort` checks).

## Relationship to Surrounding Files and Pipeline Stages

- A supporting optimization of the **code generation stage**.
- Depends on `../moonir/MoonIR.h` (`moon::Expr` and its subclasses).
- The header is included at the top of `CodeGeneratorExpressions.cpp`, which calls this function at safe array indexing sites to decide whether to skip the runtime check.
- Semantic constraint: a check is skipped only when the proof holds, ruling out "safety inferred from an incomplete range"; this is a conservative, safety-first strategy.

## Further Reading

1. `CodeGeneratorRangeAnalysis.cpp` — implementation details of the two functions.
2. `generateIndex`/`generateBorrow` in `CodeGeneratorExpressions.cpp` (the safe indexing path).
3. LLVM operators: the `Operator` enum corresponding to bitwise AND (`BitAnd`) at the MIR level.

---

---
title: src/codegen/CodeGeneratorRuntimeDescriptors.cpp
path: src/codegen/CodeGeneratorRuntimeDescriptors.cpp
stage: Code Generation (CodeGen) — runtime declaration descriptor emission
language: C++
---
