# src/sema/CompileTimeEvaluator.cpp — Compile-Time Evaluator Implementation

> One-line summary: the complete implementation of `CompileTimeEvaluator`: reflection-call evaluation, const scopes, constexpr function interpretation, and constraint/selector evaluation.

## What This File Does

This is Sema's "mini-interpreter" (~1120 lines): it evaluates compile-time expressions directly on the AST. It does not generate code; it only computes values that are determinable at compile time and writes them back into fields such as `CallExpr::compileTimeValue`/`resultType`.

## Key Structs, Classes, and Enums

No new types; it uses `CompileTimeContextAccess`. The file contains many lambda helpers (`kindName`, `containsTypeParameter`, `constIndex`, `resolveArgument`, etc.).

## Key Functions and Methods

- `analyzeReflectionCall(call, name)`:
  - Binary type relations: `type_same`/`type_same_shape`/`type_abi_compatible` (requires 2 type arguments); evaluates and writes `compileTimeValue`, returns `TyBool`.
  - Unary reflection: `type_of`/`type_kind`/`type_id`/`type_shape`/`type_domain`/`type_nominal`/`type_size`/`type_alignment`/`type_is_*`/`type_field_count`/`type_field_name`/`type_field_type`/`type_variant_count`/`type_variant_name`/`type_variant_field_count`.
  - Argument forms: `<Type>()` or a single-value argument; `type_size`/`type_alignment` do not freeze on type arguments (before generic specialization).
  - Return types: counting family → `TyI32`; `type_is_*` → `TyBool`; the rest → `TyString`.
- `analyzeDeclarationReflectionCall(call, name)`:
  - `declaration_of`: the argument must be a static named identifier; filters within `mFunctionFamilies` (optionally matching against callable signatures) with uniqueness validation; produces `Type::makeDeclarationRef` and writes `compileTimeDeclarationId`/`resolvedSymbolName`.
  - `declaration_id`/`declaration_type`, etc.: the argument must be a `declaration_ref`; the id is taken from the `compileTimeDeclarationId` of a nested `CallExpr` or from the symbol table's `compileTimeDeclarationId`; returns `TyString`.
- `enterConstScope`/`exitConstScope`: `mConstScopes.emplace_back()`/`pop_back()` (with a fallback root scope).
- `defineConst`/`lookupConst`: write/read the const scope stack.
- `evaluateConstExpr(expr, locals)`:
  - Literals are returned directly; identifiers are looked up in `locals` first, then in the global const scopes;
  - `CallExpr`: an already-evaluated reflection (`compileTimeValue`) is returned directly; otherwise it looks up `mConstexprFunctions` and calls `evaluateConstFunction`;
  - Unary (`-`/`!`/`~`) and binary (arithmetic/bitwise/comparison/logical) operators are evaluated via type branches; unsupported operations return `nullopt`.
- `evaluateConstFunction(function, args)`: only for `isConstexpr`; depth limit 128; binds parameters into `locals` and executes via `evaluateConstBlock`.
- `evaluateConstBlock(block, locals, result)`: statement by statement: `let` evaluates and stores into `locals`; `return` evaluates to produce the result; `if` recurses on the condition; other statements fail.
- `evaluateConstraintExpr(expr, bindings, active)`: constraint predicate evaluation: literal/const lookups, unary, binary (short-circuit `&&`/`||`), nested constraint calls (`evaluateConstraint`), reflection predicates (`type_same` family/`type_is_*`/`type_field_count`, etc.).
- `evaluateConstraint(name, arguments, active)`: looks up `mConcepts` (with qualified-key fallback), binds type parameters, and evaluates the predicate; `active` guards against cyclic constraints.
- `evaluateSelectorExpr(expr, locals)`: selector expression evaluation: literals, identifiers, metadata field access (matched by `schemaId`), unary/binary, assignment (`=`/`+=`, etc.), `declaration_of`, `declaration_count`/`declaration_at`/`declaration_id`/`declaration_signature`, `metadata`/`declaration_has_metadata`, `select_unique`, metadata construction (schema-name call), constexpr function calls.
- `evaluateSelectorBlock(block, locals, result, returned)`: interprets the selector function body: `let`/`return`/`if` (including else-if chains)/`for` (iterating over declaration/metadata views)/`while` (limit 10000)/`ExprStmt`.
- `evaluateSelectorFunction(function, view, arguments, failure)`: executes a selector function with the `mActiveSelectorView` context (failure reasons are written to `failure`).

## Relationship to Surrounding Files and Pipeline Stages

- `SemanticContext`'s `analyzeReflectionCall`/`evaluateConstExpr`/`evaluateConstraint`/`evaluateSelectorXxx` are forwarded here.
- `DeclarationCollector::validateMetadata` relies on `evaluateConstExpr` to compute metadata argument values.
- Constraint evaluation is triggered while `BodyAnalyzer` analyzes where clauses/generic calls; selector evaluation works together with the `DeclarationView` of `src/selector/Selector.h` (`mActiveSelectorView`).
- Constant values are written to `CallExpr::compileTimeValue` (`SemanticConstValue`) for direct use by the MoonIR generation stage.

## Further Reading

- `CompileTimeEvaluator.h` (interface and type aliases).
- `src/selector/Selector.h` (declaration views).
- `SemanticContext.h` (the `CompileTimeAnalysis` interface).
- `docs/starter/compile_time.zh-CN.md` (overview of compile-time features).


---

---
kind: source-file-guide
module: sema
source: src/sema/CompileTimeEvaluator.h
lang: en
audience: Readers familiar with C/C++ who want to understand Luna's compile-time evaluation
---

# src/sema/CompileTimeEvaluator.h — Compile-Time Evaluator (CompileTimeAnalysis Implementation)

> One-line summary: `CompileTimeEvaluator` implements `CompileTimeAnalysis`: compile-time evaluation of reflection calls (`type_of`, etc.), `const` bindings, `constexpr fn`, constraints, and selectors.

## What This File Does

Luna supports compile-time computation: `const` bindings, `constexpr fn`, metadata arguments, constraint predicates, and declaration-family filtering via `select ... with selector(...)`. `CompileTimeEvaluator` is the evaluation engine behind all of this:

- `analyzeReflectionCall`: built-in reflection functions such as `type_of`/`type_id`/`type_size`/`type_field_count` (calls with type arguments).
- `analyzeDeclarationReflectionCall`: declaration-level reflection such as `declaration_of`/`declaration_id`/`declaration_signature`.
- const scope management: `enterConstScope`/`exitConstScope`/`defineConst`/`lookupConst`.
- Constant expression evaluation: `evaluateConstExpr`/`evaluateConstFunction`/`evaluateConstBlock`.
- Constraint evaluation: `evaluateConstraintExpr`/`evaluateConstraint`.
- Selector evaluation: `evaluateSelectorExpr`/`evaluateSelectorBlock`/`evaluateSelectorFunction`.

C++ analogy: comparable to a compile-time implementation of C++'s constexpr evaluator plus a bit of reflection (`typeid`/`decltype` style), but running on the AST rather than on bytecode.

## Key Structs, Classes, and Enums

- `class CompileTimeEvaluator final : public CompileTimeAnalysis`: the only public type; private member `CompileTimeContextAccess mContext`.
- Type aliases: `ConstValue = SemanticConstValue` (`variant<int64_t, double, bool, string>`), plus `SelectorDeclarationValue`/`SelectorMetadataValue`/`SelectorDeclarationViewValue`/`SelectorMetadataViewValue`/`SelectorValue` (the selector value domain).
- The `CompileTimeAnalysis` interface (in `SemanticContext.h`): the full set of virtual methods for reflection/const/constraint/selector.

## Key Functions and Methods

(Semantics are covered in the .cpp guide; responsibilities are listed here.)

- `analyzeReflectionCall(call, name)`: binary type relations (`type_same`/`type_same_shape`/`type_abi_compatible`) or unary type reflection; evaluation writes `call->compileTimeValue` and returns `resultType` (i32/bool/string).
- `analyzeDeclarationReflectionCall(call, name)`: `declaration_of` (selecting a unique candidate within a family / matching by signature, producing a `DeclarationRef`) and other declaration reflections (`declaration_id`/`declaration_type`, etc.).
- `enterConstScope`/`exitConstScope`: push/pop the const scope stack.
- `defineConst(name, value)`/`lookupConst(name)`: define/look up compile-time immutable bindings.
- `evaluateConstExpr(expr, locals)`: evaluates constant expressions on the AST (literals, identifiers, reflection results, constexpr calls, unary/binary operations).
- `evaluateConstFunction(function, args)`: invokes a `constexpr fn` (depth limit 128, to prevent recursion blowup).
- `evaluateConstBlock(block, locals, result)`: executes a block sequentially (let/return/if) and produces the result.
- `evaluateConstraintExpr(expr, bindings, active)`: evaluates constraint predicates under type-parameter bindings (short-circuit and/or, reflection predicates, nested constraints).
- `evaluateConstraint(name, arguments, active)`: looks up `mConcepts` by constraint name and binds type parameters for evaluation (`active` guards against recursion).
- `evaluateSelectorExpr(expr, locals)`: selector expression evaluation (literals, metadata field access, `declaration_of`, `declaration_count/at/id/signature`, `metadata`/`declaration_has_metadata`, `select_unique`, metadata construction, constexpr calls).
- `evaluateSelectorBlock(block, locals, result, returned)`: interprets the selector function body (let/return/if/for/while/expr); `for` can iterate over declaration views and metadata views, `while` is capped at 10000 iterations.
- `evaluateSelectorFunction(function, view, arguments, failure)`: invokes a selector function with the `mActiveSelectorView` context.

## Relationship to Surrounding Files and Pipeline Stages

- Called via forwarding from `SemanticContext`'s `analyzeReflectionCall`/`evaluateConstExpr`/`evaluateConstraint`/`evaluateSelectorXxx`.
- Accesses `mConstScopes`/`mConstexprFunctions`/`mConcepts`/`mMetadataSchemas`/`mFunctionFamilies`/`mActiveSelectorView`, etc. through `CompileTimeContextAccess`.
- `BodyAnalyzer` triggers these evaluations while analyzing `let const`/metadata arguments/constraint calls; `DeclarationCollector::validateMetadata` uses `evaluateConstExpr` to compute metadata arguments.
- Selector evaluation depends on the `DeclarationView` from `src/selector/Selector.h`.

## Further Reading

- `CompileTimeEvaluator.cpp` (implementation).
- `SemanticContext.h` (the `CompileTimeAnalysis` interface and const/selector state).
- `src/selector/Selector.h` (declaration views and the selector runtime).
- The compile-time evaluation section of `docs/starter/sema.zh-CN.md`.


---

---
kind: source-file-guide
module: sema
source: src/sema/ControlAnalyzer.cpp
lang: en
audience: Readers familiar with C/C++ who want to read Luna's slot/snippet analysis implementation
---
