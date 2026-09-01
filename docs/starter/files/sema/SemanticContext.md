# src/sema/SemanticContext.cpp — Semantic Analysis Main Pipeline (Multi-Pass Scheduling) and Forwarding

> In one sentence: implements the multi-pass analysis pipeline of `SemanticContext::analyze` (linkage name assignment → declaration collection → impl validation → body analysis → inference finalization), plus the "wiring code" that forwards each component's interface to its bound implementation.

## What This File Does

This is Sema's "conductor." `analyze(Program*)` drives the five components in dependency order, ensuring forward references are independent of declaration order:

1. Initialize/reset all state (const scopes, slot/apply scopes, and the various registries).
2. Register compiler-known Core constraints: canonical `Drop` and `From` identities owned by `org.luna.core`.
3. Package use-alias table (`mPackageAliases`).
4. **Linkage name assignment**: iterate over all declarations and generate a `generatedSymbolName` for each (`main` is special-cased; repeated base linkages use `isolatedLinkageName` keyed by metadata plus normalized callable source signature), and check for package-level name/signature collisions.
5. `declareMeta` (metadata schemas registered first).
6. `declareConstraint` (constraint names registered first; where-clause order is irrelevant).
7. Pre-bind nominal types: `Type::makeStruct`/`makeEnum` write into `mDeclaredTypes`/`mSymTable` (supports forward references).
8. `declareTrait` → `declareStruct`/`declareEnum` (including `validateMetadata`) → `declareFunction`/`declareFragment`/`declareImpl`.
9. `analyzeTrait` → `analyzeImpl` (runs before ordinary bodies so the `Drop` constraint does not depend on source order) → `analyzeFunction`/`Struct`/`Enum`/`Meta`/`Constraint`.
10. Inference finalization: `defaultUnconstrainedNumeric()` → per-declaration `checkUnresolved` → `mInferenceRoots` → `materializeInferredTypes`.

The second half is a large set of forwarding methods: `declareXxx`/`analyzeXxx`/`resolveTypeAST`/`constrain`/`evaluateConstXxx` and friends on `SemanticContext` all forward to the `mXxxAnalysis` pointers (bound in the constructor).

C++ analogy: the "translation-unit-level context + scheduler" of a compilation unit: first register all declarations (like C++'s declare-before-use), then run each kind of check, and finally do a unified inference pass.

## Key Structs, Classes, and Enums

No new types (the structs live in the .h); this file contains:
- File-local lambdas/state: `declaredNames`/`linkageNameCounts` (duplicate-name tracking), `rootEntryCount` (uniqueness check for `main`).
- `SemanticContext` private members borrowed inside methods (see the .h).

## Key Functions and Methods

- `SemanticContext()`: registers the built-in types and `print`.
- `analyze(Program*)`: the multi-pass pipeline described above; returns `mErrors.empty()`.
- Linkage name assignment details: `metadataDeclarationName(name, decl)` produces the base linkage name and `declarationSourceIdentity` adds a normalized callable source signature for functions; `isRootEntry` (`main`) → `"main"`; repeated base linkages or `main` conflicts use `isolatedLinkageName(familyKey + "::" + sourceIdentity, sourceLinkage)`. Exact metadata/signature duplicates still issue `Duplicate package declaration`; heterogeneous signatures receive distinct linkages.
- `resolveTraitRef(TraitRef&, useSite)`: special-cases `Drop`/`From` (compiler-reserved ids); otherwise looks up `mTraits` and calls `recordDeclarationReference`.
- `satisfiesTrait(traitId, type)`: dual lookup in `mImpls` + `mTraitMethods`.
- `recordDeclarationReference`/`recordResolvedReference`: write to `mDeclarationReferences` (IDE navigation), with validity filtering.
- `error(msg, line, col)`: picks a hint based on message keywords (undefined name/FFI/selector/not callable/integer/const/constraint/type_, etc.), enqueued via `diagnostic::format("semantic", ...)`.
- `setDiagnosticLocation`/`setDeclarationContext`: maintain the current diagnostic location and package/module context.
- `sourceDeclarationKey(name, diagnoseVisibility)`: handles `::`-qualified names, package aliases, and module paths, and performs visibility diagnostics.
- `lookupSymbol`/`lookupDeclaredType`: lexical bindings/built-ins take priority, then qualified-key table lookup.
- The rest are all forwards: `declareFunction`→`mDeclarationAnalysis->declareFunction`, `analyzeFunction`→`mBodyAnalysis`, `analyzeSlotDecl`→`mControlAnalysis`, `resolveTypeAST`→`mTypeAnalysis`, `evaluateConstExpr`→`mCompileTimeAnalysis`, etc.

## Relationship to Surrounding Files and Pipeline Stages

- `SemanticAnalyzer::analyze` forwards here; component implementations live in their own .cpp files.
- `SemanticContextAccess.h/.cpp` provide the component access surface.
- `SemanticAnalysisSupport.h` provides identity/linkage-name/key utilities (`nominalDeclarationIdentity`/`isolatedLinkageName`/`qualifiedDeclarationKey`, etc.).
- Output is consumed by MoonIR generation and the subsequent standalone checker.

## Further Reading

- `SemanticContext.h` (interface and fields).
- `DeclarationCollector.cpp` (declaration collection details), `BodyAnalyzer.cpp` (body analysis), `TypeResolver.cpp` (inference finalization).
- `SemanticAnalysisSupport.h` (linkage-name/identity utilities).


---

---
kind: source-file-guide
module: sema
source: src/sema/SemanticContext.h
lang: en
audience: Readers who know C/C++ and want to understand the core state of Luna's semantic analysis
---

# src/sema/SemanticContext.h — The "State Hub" of Semantic Analysis and Its Five Analysis Interfaces

> In one sentence: `SemanticContext` is the host of all shared semantic-analysis state (symbol tables, impls, traits, inferencer, diagnostics, fragment/slot tables, etc.), and also declares the five analysis interfaces (Body/Type/CompileTime/Declaration/Control Analysis) plus the `analyze(Program*)` main entry point.

## What This File Does

This is Sema's most central header file. It:

1. Declares the `SemanticContext` class — the "global state" of the semantic-analysis runtime: the fields shared by all components live here (`mSymTable`, `mImpls`, `mTraits`, `mConstraints`, `mErrors`, `mDeclarationReferences`, const/selector-related state, slot/apply state, etc.).
2. Declares the five pure-virtual analysis interfaces (`BodyAnalysis`, `TypeAnalysis`, `CompileTimeAnalysis`, `DeclarationAnalysis`, `ControlAnalysis`) — semantic analysis is split into five independently implementable components, and `SemanticContext` depends only on the interfaces.
3. Declares `SemanticContext`'s `analyze` main flow and numerous forwarding methods: each component has a same-named forwarding method on `SemanticContext` that routes calls to the bound component.
4. Declares several internal data structures: `FromConversion`, `FromIteratorImplementation`, `SlotInfo`.

C++ analogy: `SemanticContext` ≈ the compiler's "translation-unit context," and the five interfaces ≈ five single-responsibility subsystems (types, bodies, compile-time, declarations, control flow); the context wires them together via dependency injection.

## Key Structs, Classes, and Enums

The five interfaces (all pure virtual, implemented by their corresponding components):

- `class BodyAnalysis`: `analyzeFunction`/`Struct`/`Enum`/`Trait`/`Impl`, `analyzeStmt`/`Block`/`Expr`/`Call`/`MemberCall`/`IteratorCall`/`Launch`/`Select`, `statementAlwaysReturns`/`blockAlwaysReturns`.
- `class TypeAnalysis`: `findMatchingImpl`/`monomorphize`/`resolveTypeAST`/`instantiateNominal`/`declaredType`/`resolved`/`constrain`/`requireBool`/`Numeric`/`Integer`/`checkUnresolved`/`typeToAST`/`materializeInferredTypes`.
- `class CompileTimeAnalysis`: `analyzeReflectionCall`/`analyzeDeclarationReflectionCall`/const scope and lookup/`evaluateConstExpr`/`evaluateConstFunction`/`evaluateConstBlock`/constraint and selector evaluation.
- `class DeclarationAnalysis`: `declareFunction`/`Meta`/`Constraint`/`Struct`/`Enum`/`Trait`/`Impl`/`Fragment`, `analyzeConstraint`/`Meta`, `validateMetadata`, `isFFIType`/`validateFFIFunction`.
- `class ControlAnalysis`: `analyzeSlotDecl`/`SlotInvoke`/`Apply`, `analyzeFragmentForSlot`, `enter`/`exitSlotScope`, `selectFragment`.

`SemanticContext` internal structures:

- `struct FromConversion`: a one-to-one conversion record for the `From` trait (source/target/`FunctionDecl*`/symbol).
- `struct FromIteratorImplementation`: the builder protocol for `FromIterator` (item/builder/target + begin/push/finish).
- `struct SlotInfo`: a slot declaration summary (nominal declaration,
  parameter types/contracts, default fragment, kind, and cardinality).
- Field groups include symbol/type/trait/instantiation state, diagnostics,
  compile-time evaluation state, static `mSlotScopes`/`mApplyScopes`, fragment
  state, iterator state, and the active compile-time selector view.

## Key Functions and Methods

- Constructor `SemanticContext()`: registers the built-in types (i32/i64/f32/f64/bool/string) and the `print` built-in function.
- `analyze(Program*)`: the main entry point, driving the multiple passes (see the .cpp guide for details).
- `bodyAccess()`/`compileTimeAccess()`/`controlAccess()`/`declarationAccess()`/`typeAccess()`: return the component-specific access references (friend classes such as `BodyContextAccess`).
- Five `bind*` methods such as `bindBodyAnalysis`: inject the component implementations.
- `errors()`/`symTable()`/`declarationReferences()`: external accessors.
- Numerous forwarding methods: `declareXxx`→`mDeclarationAnalysis`, `analyzeXxx`→`mBodyAnalysis`/`mControlAnalysis`, `resolveTypeAST`/`constrain`/`requireXxx`→`mTypeAnalysis`, `evaluateConstXxx`/`enterConstScope`→`mCompileTimeAnalysis`, etc.
- Private utilities: `resolveTraitRef`/`typeIdentity`/`traitIdentity`/`satisfiesTrait`/`findMatchingImpl`/`monomorphize`/`error`/`setDiagnosticLocation`/`setDeclarationContext`/`sourceDeclarationKey`/`lookupSymbol`/`lookupDeclaredType`/`recordDeclarationReference`/`recordResolvedReference`.

## Relationship to Surrounding Files and Pipeline Stages

- `SemanticAnalyzer.cpp` assembly: constructs `SemanticContext` + the five components and binds them to each other.
- `SemanticContextAccess.h/.cpp`: the "access surface" exposing fields/methods to the components.
- Component implementations: `BodyAnalyzer` (BodyAnalysis), `TypeResolver` (TypeAnalysis), `CompileTimeEvaluator` (CompileTimeAnalysis), `DeclarationCollector` (DeclarationAnalysis), `ControlAnalyzer` (ControlAnalysis).
- The multi-pass scheduling of `analyze` (declaration collection → impl validation → body analysis → inference finalization) ties the whole Sema main line together.

## Further Reading

- `SemanticContext.cpp` (the `analyze` main flow), `SemanticContextAccess.h` (access encapsulation), `SemanticAnalyzer.h/.cpp` (facade).
- The .h/.cpp of each of the five components.
- Overview: `docs/starter/sema.zh-CN.md`.


---

---
kind: source-file-guide
module: sema
source: src/sema/SemanticContextAccess.cpp
lang: en
audience: Readers who know C/C++ and want to read the wiring implementation of the Sema components
---
