# src/sema/ControlAnalyzer.cpp — Slot/Fragment/Continuation Analysis Implementation

> One-line overview: the full implementation of `ControlAnalyzer`: slot declarations/invocations, apply bindings, fragment contract verification and body analysis, and continuation once/many control-path checking.

## What This File Does

Implements the `ControlAnalyzer.h` interface (~500 lines). The core is:

1. Maintains three layers of scope stacks: `mSlotScopes` (slot declarations), `mApplyScopes` (static apply bindings), and `mDynamicApplyScopes` (dynamic candidate lists).
2. In `analyzeSlotInvoke`, resolves the active slot information, constrains arguments, analyzes the continuation body, then resolves the fragment and performs contract matching.
3. In `analyzeFragmentForSlot`, uses "control-path analysis" (the `ControlPaths` structure + recursive traversal) to check continuation consumption semantics.

The file defines the local structure `ControlPaths { active (set), aborted, returned, abortAfterResume }` as well as two recursive lambdas (`analyzePaths`/`analyzeStmtPaths`).

## Key Structs, Classes, and Enums

- File-local `struct ControlPaths`: `std::set<int> active` (current resume count per path, capped at 2), plus the `aborted`/`returned`/`abortAfterResume` flags.
- Uses `ControlContextAccess::SlotInfo` (`SemanticContext.h`).

## Key Functions and Methods

- `analyzeSlotDecl`: deduplicates slot names; resolves parameter types and contracts (`declaredType` + `parameterContractFor`); fills in `SlotInfo`; resolves the default fragment (`selectFragment` + contract verification); builds the structural type via `Type::makeSlot`; registers into `mSlotScopes.back()` and the symbol table.
- `analyzeSlotInvoke`:
  - Three branches decide the "active slot": implicit capture (`isImplicitCapture`, building the slot in place), inline interface (`interfaceParams`, resolving parameters and constraining them to local bindings), or a `lookupSlot` lookup.
  - Checks arity and `constrain`s each argument.
  - Saves `structuralType`/`resolvedParamNames`, takes the capture set via `visibleSymbols()`, and analyzes the continuation body.
  - Fragment resolution has three paths: dynamic candidates (`lookupDynamicApplied`, verifying contract consistency and calling `analyzeFragmentForSlot` for each), static apply (`lookupApplied`), and the default fragment; with no binding, it is treated as an identity fragment (resume once).
  - Verifies that the fragment's kind/cardinality matches the slot's contract.
- `analyzeApply`:
  - `selectFragment` picks the primary fragment; contracts are verified against known slots; for dynamic applies, the slot must be a `dynamic slot`, candidates must have `runtime`/`dynamic` retention, and `resolvedAlternativeFragmentNames` is collected; if there is a body, it runs `enterSlotScope` → writes `mApplyScopes`/`mDynamicApplyScopes` → `analyzeBlock` → `exitSlotScope`.
- `analyzeFragmentForSlot`:
  - Checks arity; saves/restores `mCurrentFragmentSlot`/`mCurrentFragmentDecl`/`mCurrentReturnType`.
  - Enters scope: binds parameters (`constrain` to the slot's parameter types, checking ownership contracts) and copies captured symbols.
  - Analyzes the body with `analyzeBlock(fragment->body, TyUnit)`.
  - Control-path analysis: performs a data-flow-style traversal of the body (resume increments the active count by 1, capped at 2; abort/return terminate a path; if/match/while/for merge paths), checking: a single-shot context must not `abort()` after `resume()` (`abortAfterResume`) and must not resume more than once; rebuilds the fragment's structural type; many fragments forbid linear captures (not replayable).
- `enterSlotScope`/`exitSlotScope`: push/pop the three stacks in sync (keeping the root layer as a floor).
- `selectFragment`: looks up `sourceDeclarationKey(name)` in `mContext.mFragments`; reports an error if unknown.

## Relationship to Surrounding Files and Pipeline Stages

- `SemanticContext` forwards entry points such as `analyzeSlotDecl` here.
- Reads and writes `SemanticContext`'s slot/apply/fragment state through `ControlContextAccess`.
- `DeclarationCollector::declareFragment` first registers `mFragments`; `BodyAnalyzer` forwards when it encounters the relevant statements; `TypeResolver` back-fills fragment structural types.
- The control-path analysis results (`structuralType`/`resolved*Name`) are consumed by MoonIR generation.

## Further Reading

- `ControlAnalyzer.h` (the interface).
- `SemanticContext.h` (`SlotInfo` and state fields).
- `DeclarationCollector.cpp` (fragment registration).
- `BodyAnalyzer.cpp` (call entry points).


---

---
kind: source-file-guide
module: sema
source: src/sema/ControlAnalyzer.h
lang: en
audience: Readers familiar with C/C++ who want to understand Luna's slot/fragment/continuation system
---

# src/sema/ControlAnalyzer.h — Control-Flow Analyzer (ControlAnalysis Implementation)

> One-line overview: `ControlAnalyzer` implements `ControlAnalysis`: it analyzes `slot` declarations and invocations, the fit of `apply` (fragment bindings) and fragments against slots, and the once/many semantics of continuations.

## What This File Does

Luna has control constructs akin to "effect handlers": a `slot` is a replaceable suspension point in the program, a `fragment` is an implementation of a suspension point (bindable to one or more slots), `apply` binds a fragment to a slot within a scope, and slot invocations (`resume()`/`abort()`) drive continuations. `ControlAnalyzer` performs the semantic analysis of these declarations:

- `analyzeSlotDecl`: registers `slot` declarations (parameters, contracts, default fragment, kind/cardinality).
- `analyzeSlotInvoke`: analyzes slot invocation sites (static/dynamic, argument constraints, continuation body, fragment resolution and contract matching).
- `analyzeApply`: analyzes `apply` statements (fragment binding, dynamic candidates, scopes).
- `analyzeFragmentForSlot`: binds a fragment to a specific slot and analyzes its body (parameter/contract verification, captures, continuation control paths).
- `enterSlotScope`/`exitSlotScope`: management of the slot/apply scope stacks.
- `selectFragment`: looks up a fragment by name and diagnoses unknown names.

C++ analogy: this resembles type-checking a "callback/hook mechanism": slot ≈ event point, fragment ≈ event handler, apply ≈ registration. On top of that, however, continuation (resume/abort) and single/multiple consumption semantics are layered in.

## Key Structs, Classes, and Enums

- `class ControlAnalyzer final : public ControlAnalysis`: the only public type; private member `ControlContextAccess mContext`.
- The `ControlAnalysis` interface (in `SemanticContext.h`): `analyzeSlotDecl`/`analyzeSlotInvoke`/`analyzeApply`/`analyzeFragmentForSlot`/`enterSlotScope`/`exitSlotScope`/`selectFragment`.
- The `SlotInfo` used is defined in `SemanticContext.h` (name/paramTypes/paramContracts/paramNames/defaultFragment/acceptedKind/acceptedCardinality/isImplicitCapture/isDynamic/structuralType).

## Key Functions and Methods

(Semantics are covered in the .cpp guide; responsibilities are listed here.)

- `analyzeSlotDecl(SlotDeclStmt*)`: resolves parameter types and contracts, registers `mSlotScopes`, builds the slot's structural type, registers symbols, and resolves the default fragment.
- `analyzeSlotInvoke(SlotInvokeStmt*, expectedReturn)`: resolves the active slot via the static / implicit-capture / inline-interface paths; constrains arguments; analyzes the continuation body; resolves the fragment (static apply / dynamic candidates / default); verifies contracts and calls `analyzeFragmentForSlot`.
- `analyzeApply(ApplyStmt*, expectedReturn)`: picks the fragment, verifies contracts against known slots, handles dynamic applies (multiple candidates, runtime requirements, `mDynamicApplyScopes`), and enters the slot scope to analyze the `body`.
- `analyzeFragmentForSlot(FragmentDecl*, slotName, paramTypes, contracts, captures)`: verifies arity and contracts, enters scope to bind parameters and captures, `analyzeBlock`s the fragment body, then performs **control-path analysis** (`ControlPaths`: active/aborted/returned/abortAfterResume) to check once/many semantics (a single-shot must not resume more than once and must not abort-after-resume, etc.), rebuilds the fragment's structural type, and forbids linear captures in many fragments.
- `enterSlotScope`/`exitSlotScope`: push/pop `mSlotScopes`/`mApplyScopes`/`mDynamicApplyScopes` together.
- `selectFragment(name, useSite)`: looks up `mContext.mFragments`; reports `unknown fragment` on failure.

## Relationship to Surrounding Files and Pipeline Stages

- Forwarded from `SemanticContext`'s `analyzeSlotDecl`/`analyzeSlotInvoke`/`analyzeApply`/`analyzeFragmentForSlot`/`enter/exitSlotScope`/`selectFragment`.
- Accesses `mSlotScopes`/`mApplyScopes`/`mDynamicApplyScopes`/`mFragments`/`mCurrentFragmentSlot`/`mCurrentFragmentDecl`, etc., through `ControlContextAccess`.
- `BodyAnalyzer` forwards `SlotDeclStmt`/`SlotInvokeStmt`/`ApplyStmt` here during statement analysis; `TypeResolver::materializeInferredTypes` back-fills the fragment structural-type fields.
- Consumes the `mFragments` registered by `DeclarationCollector::declareFragment`.

## Further Reading

- `ControlAnalyzer.cpp` (the implementation).
- `SemanticContext.h` (the `ControlAnalysis` interface and `SlotInfo`).
- `BodyAnalyzer.cpp` (call entry points).
- Language feature background: the slot/fragment design in `docs/reference/`.


---

---
kind: source-file-guide
module: sema
source: src/sema/DeclarationCollector.cpp
lang: en
audience: Readers familiar with C/C++ who want to read Luna's declaration-collection implementation
---
