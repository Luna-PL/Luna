# src/sema/OwnershipChecker.cpp — Ownership Check Implementation

> One-line overview: the full implementation of `OwnershipChecker` (data-flow style static checking): the variable Valid/Moved/Freed state machine, loan/conflict management, linear resource consumption, branch/loop state merging, and automatic insertion of implicit `free` calls.

## What This File Does

This is the concrete algorithm behind the ownership checker (about 2,000+ lines). It simulates the state transitions of program execution: for each scope it maintains a variable state table (`mScopes`) and a loan table (`mLoansInScope`), performs statement-by-statement forward propagation, and handles return-terminated paths, if/match/loop branch merging, lambda captures, GPU events, and multiple resume/abort cycles of fragment continuations.

Design notes (see the .h comments): `FlowResult { ok, fallsThrough }` distinguishes "normal continuation" from "return termination" — resources consumed by a return branch must never be merged with the state of code that is still reachable; loans have a lexical lifetime and are released before the source variable is automatically released.

## Key Structs, Classes, and Enums

The structs are declared in `OwnershipChecker.h`: `OwnState`, `Place`, `Loan`, `VarInfo`, `FlowResult`, `CheckerState`. This file contains the algorithm implementations for all of these structures.

## Key Functions and Methods

Entry points:

- `check(program, symTable)`: records the symbol table, collects `mFragments`, and pushes the root apply/slot scopes; iterates over declarations — `FunctionDecl` (non-extern) and `ImplDecl::methods` — calling `checkFunction` on each; returns `mErrors.empty()`.
- `checkFunction(decl)`: parameters are non-owning views by default (in `define`, `isHeapAllocated` is set to true only when the type is Owned and requires cleanup); calls `checkBlock(body)`; on fall-through, runs `validateLinearScope`; appends the implicit `FreeStmt` returned by `collectFreesAtScopeExit()` to the end of the body (`isImplicit=true`, `action=cleanupActionForType`).
- `checkBlock(block)`: pushes a scope and the apply/slot stacks; calls `checkStmt` statement by statement; bails out early on `!ok`; statements after a return termination are unreachable (break on `!fallsThrough`); `releaseLoansInCurrentScope` ends this block's loans; `validateLinearScope` (fall-through only); collects `collectFreesAtScopeExit` and inserts the implicit `FreeStmt` at the end of the block only on fall-through; `exitScope` and return.
- `checkStmt(stmt)`, `checkExpr(expr)`: dispatch to specialized methods: `checkLetStmt` / `checkForStmt` (iterator source ownership) / `checkReturnStmt` (marks the return value as pending release and cleans up on return) / `checkMatchStmt` (merging state across arms) / `checkAbortStmt` (abort inside a fragment) / `checkCallExpr` (argument consumption, loans, cross-call in-flight) / `checkVariantConstruct` / `checkRecordLiteral` / `checkLaunchExpr` (GPU events and in-flight resources).
- `checkSlotInvoke(slot)`: resolves the apply or the default fragment; with `usesDynamicDispatch`, for each candidate fragment: `restoreState(before)` → `checkFragment(candidate, slot, multiShot)`, collecting fall-through states and checking that all dynamic candidates have "identical effects" (same loan/variable state), otherwise reporting an error; for Many (multi-shot), first snapshots and checks the continuation body once — if the captured state is consumed (`continuationConsumesCapturedState`), it reports that the fragment "cannot be resumed multiple times".
- `checkFragment(fragment, slot, multiShot)`: saves the current slot/fragment context and enters a scope binding the fragment's parameters; `checkBlock(fragment->body)`; for an interceptor, `checkBlock(slot->continuation)` runs after the body falls through; collects the `CheckerState` at every "exit point" (including abort/return paths); merges them pairwise with `mergeFallthroughStates`; rolls back with `restoreState`; returns a value reflecting whether it falls through.
- `continuationConsumesCapturedState(before)`: compares the state before and after the snapshot; if a variable changes from `Valid` to non-`Valid`, it has been consumed.
- `captureState`/`restoreState`: snapshot/rollback of `CheckerState`.
- `sameVarState`/`sameLoanState`/`sameApplyState`/`sameSlotState`: compare whether two sets of states are identical (state, borrow counts, in-flight counts, event resources, sets of moved places, etc.) to decide whether branches can be merged.
- Loan management: `acquireLoan`/`beginInFlightBorrow`/`releaseLoan`/`releaseLoansInCurrentScope` (loans are released before their source is automatically released), `finishEvent`.
- `consume(place/var, action)`: move/release (diagnosing use-after-move/duplicate free when necessary); `checkWriteTarget`; `extractPlace`/`placesOverlap`/`isPlaceAvailable`/`hasConflictingLoan` (mutable borrow conflicts)/`allDirectFieldsMoved`/`typeOfPlace`.
- `collectFreesAtScopeExit`/`collectFreesAtReturn`/`collectFreesAtFragmentExit`/`validateLinearScope`/`validateLinearReturnPath`: free collection and validation of linear resource consumption.
- `usageFromTypeAST`/`isReferenceExpr`/`isDeviceBuffer`/`isEvent`; `lookup`/`define`; `error`/`setDiagnosticLocation`.

## Relationship to Surrounding Files and Pipeline Stages

- Runs independently after semantic analysis, taking as input the AST (with `inferredType`/ownership contracts already filled in) and the `SymbolTable`.
- Along with `TraitChecker`, it is one of the second-tier independent checkers outside Sema (neither is part of the `SemanticContext` component system).
- Uses `luna::ownership::Contract`/`CleanupAction` (from `src/core/TypeSystem.h`) and `resolveType`.
- The inserted implicit `FreeStmt` (`isImplicit=true`) enters the AST, and MoonIR generates destructor calls from it.

## Further Reading

- `OwnershipChecker.h` (interface/data structures).
- `luna::ownership` in `src/core/TypeSystem.h`.
- Binding/usage decisions in `BodyAnalyzer.cpp`.
- Language documentation: the ownership section of `docs/starter/sema.zh-CN.md`.


---

---
kind: source-file-guide
module: sema
source: src/sema/OwnershipChecker.h
lang: en
audience: Readers familiar with C/C++ (especially move semantics/smart pointers) who want to read Luna's ownership checker
---

# src/sema/OwnershipChecker.h — Ownership Checker (Move/Borrow/Free)

> One-line overview: `OwnershipChecker` is a second-pass checker independent of the main semantic analysis: it performs "data-flow style" ownership checking over the AST — tracking each variable's Valid/Moved/Freed state, loans and their conflicts, and the one-shot consumption of linear resources — and automatically inserts implicit `free` calls.

## What This File Does

Luna's ownership system resembles Rust's affine/linear types: Copy values can be copied, Affine values can be moved only once, and Linear resources (such as event and device_buffer) must be consumed exactly once. `OwnershipChecker` runs after semantic analysis (when types/symbols are ready) and checks every function and block:

- Variable state machine: `Valid` → `Moved`/`Freed`, preventing use-after-move and duplicate frees.
- Loan tracking: lifetimes and conflicts of shared/mutable references (`hasConflictingLoan`).
- Partial moves: field-level moves (prefix-overlap detection on `Place`) and "a value whose fields have all been moved is unavailable as a whole".
- Linear resources: collects what must be freed at scope exit/return (`collectFreesAtScopeExit`/`collectFreesAtReturn`) and automatically inserts an implicit `FreeStmt` at the end of the function body.
- Control-flow merging: merges branch states for if/match/loop (`mergeFallthroughStates`/`loopPreservesOuterState`), distinguishing "normal continuation" from "return-terminated paths" (`FlowResult.fallsThrough`).
- Closure/lambda captures: Copy local variables are copied into the lambda scope as capture candidates; Affine/Linear/borrowed captures are diagnosed.
- Fragment continuations: checks the consumption of resources inside a fragment body along resume/abort paths (`checkFragment`/`continuationConsumesCapturedState`).

C++ analogy: this is like using a "static analyzer" to simulate C++ unique_ptr semantics — who holds the value, when it is moved, when it is freed, and whether writes are allowed while it is borrowed. `Place` is analogous to a "storage location path"; `Loan` is analogous to an "active reference".

Note: this class does not participate in the `SemanticContext` component system (it implements no `*Analysis` interface); the driver constructs it separately and calls `check(Program*, SymbolTable&)`.

## Key Structs, Classes, and Enums

- `enum class OwnState { Valid, Moved, Freed }`: the state of a variable.
- `struct Place`: a storage location: `root` (variable name) + `components` (field names / constant indices / `[*]` / `*` dereference); prefix overlap is the basis for partial-move and borrow-conflict detection.
- `struct Loan`: one active loan: `Place source` + `isMutable`.
- `struct VarInfo`: the complete ownership state of a single variable: `state`, `type`, `isHeapAllocated`, `usage` (Copy/Affine/Linear), `relation` (Owned/Borrowed), `movedPlaces` (moved fields), `isReference`/`isMutableReference`, `sharedBorrows`/`mutableBorrow`, `inFlightReads`/`inFlightWrites`, `isGpuEvent`, `materializedIteratorOwnsSource`/`materializedIteratorSourceType`, `eventResources` (resources associated with an event), `name`.
- `struct FlowResult { bool ok; bool fallsThrough; }`: the result of checking a block: `ok` indicates success, `fallsThrough` distinguishes "normal continuation" from "return termination" — resources consumed by a return branch must not be merged with still-reachable code.
- `struct CheckerState`: a snapshot of the four stacks — scopes/loans/apply/slot (used for saving, restoring, and merging across branches).
- `class OwnershipChecker`: private members `mScopes`, `mLoansInScope`, `mErrors`, `mSymTable*`, diagnostic location, `mFragments`, `mApplyScopes`/`mSlotScopes`, `mCurrentSlotContinuation`, `mValidatingManyContinuation`, `mCheckingSlotContinuation`, `mCurrentFragmentAbortExits`, fragment scope base addresses, `mUnavailableLambdaCaptures`.

## Key Functions and Methods

Entry points:

- `check(Program*, SymbolTable&)`: registers fragments, calls `checkFunction` for each function/impl method; returns `mErrors.empty()`.
- `errors()`: read-only list of diagnostics.

Main flow:

- `checkFunction(decl)`: builds parameters (borrowed views by default; ownership transfers only for explicit affine/linear parameters); `checkBlock(body)`; `releaseLoansInCurrentScope`; `validateLinearScope` (linear resources must be consumed when falling through); inserts an implicit `FreeStmt` at the end of the body when necessary (`collectFreesAtScopeExit`).
- `checkLambda(lambda)`: saves/isolates the outer state, copies Copy captures into the lambda scope, and reports C016/CL005-class diagnostics for Affine/Linear/borrowed captures.
- `checkBlock(block)`: enters a scope, runs `checkStmt` statement by statement, and on exit `releaseLoansInCurrentScope` + `collectFreesAtScopeExit` releases automatically.
- `checkStmt(stmt)`: dispatches on let/return/match/for/abort/slot invoke/apply/if/while/expr, etc. (large branches are split into specialized methods).
- `checkExpr(expr)`: expression-level checking (calls, variant construction, record, launch — each with a specialized method).
- Specialized methods: `checkLetStmt` (binding state/usage), `checkForStmt` (iterator source ownership), `checkReturnStmt` (releases along the return path), `checkMatchStmt` (merging state across arms), `checkAbortStmt`, `checkCallExpr` (argument consumption/loans), `checkVariantConstruct`/`checkRecordLiteral`, `checkLaunchExpr` (events and in-flight resources), `checkSlotInvoke`/`checkFragment` (continuation resource consumption).

State management:

- `enterScope`/`exitScope`/`releaseLoansInCurrentScope`.
- `acquireLoan(place, isMutable)`/`beginInFlightBorrow(name, isMutable)`/`releaseLoan`: acquiring and releasing loans (lexical lifetime — loans are released before the source variable is automatically released).
- `consume(place/var, action)`: move/consume (checking state and conflicting loans).
- `checkWriteTarget(expr)`: write-target checking (in-flight reads/writes, conflicting loans).
- `extractPlace(expr)`/`renderPlace`/`renderProjection`/`placesOverlap`/`isPlaceAvailable`/`hasConflictingLoan`/`allDirectFieldsMoved`/`typeOfPlace`: place derivation and conflict queries.
- `captureState`/`restoreState`/`mergeFallthroughStates`/`loopPreservesOuterState`/`sameVarState`/`sameLoanState`/`sameApplyState`/`sameSlotState`/`describeControlFlowDifference`: branch/loop state snapshots, merging, and difference diagnostics.
- `usageFromTypeAST`/`isReferenceExpr`/`isDeviceBuffer`/`isEvent`.
- `lookup`/`define`: reading and writing the variable table.
- `collectFreesAtScopeExit`/`collectFreesAtReturn`/`collectFreesAtFragmentExit`/`validateLinearScope`/`validateLinearReturnPath`: free collection and linearity validation.
- `error`/`setDiagnosticLocation`: diagnostics.

## Relationship to Surrounding Files and Pipeline Stages

- Independent of the `SemanticContext` component system; runs after semantic analysis (type/symbol binding) completes, consuming the filled-in `FunctionDecl::inferredType` and so on.
- Receives a `SymbolTable&` directly (shares the same symbol table as `SemanticContext`).
- Uses `src/core/TypeSystem.h` (`resolveType`/`TypeKind`) and `luna::ownership` (`Usage`/`Relation`/`Contract`/`parameterContractFor`/`defaultUsageForType`).
- The automatically inserted `FreeStmt` (`isImplicit = true`) enters the AST, letting MoonIR generate explicit destructor calls.
- Similar to `TraitChecker`: both are second-tier independent checkers outside Sema.

## Further Reading

- `OwnershipChecker.cpp` (implementation).
- The `luna::ownership` namespace in `src/core/TypeSystem.h` (Usage/Relation/Contract).
- `BodyAnalyzer.cpp` (binding/usage decisions such as `analyzeLetStmt` — an input to the ownership checker).
- Language feature background: the ownership/linear design in `docs/reference/`.


---

---
kind: source-file-guide
module: sema
source: src/sema/SemanticAnalysisSupport.h
lang: en
audience: Readers familiar with C/C++ who want to learn about Sema's general-purpose utility functions
---
