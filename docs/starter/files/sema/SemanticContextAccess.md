# src/sema/SemanticContextAccess.cpp — Implementation of the Access-Face Factories and Forwarding

> One-line summary: implements the five `xxxAccess()` factories (constructing the `*ContextAccess` objects and binding context fields to references) and all forwarding methods (`mOwner.xxx(...)`).

## What This File Does

A pure "wiring" file: it provides the constructors for the five access classes (initializer lists binding `SemanticContext` fields to member references) and the forwarding methods. There is no analysis logic.

## Key Structs, Classes, and Enums

No new types are introduced.

## Key Functions and Methods

- The five factories such as `SemanticContext::bodyAccess()`: `return BodyContextAccess(*this);` and so on.
- Each access constructor, e.g. `BodyContextAccess(SemanticContext& context) : mOwner(context), mConcepts(context.mConcepts), ...` (one line per field).
- Forwarding method examples: `BodyContextAccess::constrain` → `mOwner.constrain(actual, expected, context)`; `analyzeApply` → `mOwner.analyzeApply(stmt, std::move(expectedReturn))`; `error` → `mOwner.error(...)`; `lookupSymbol` → `mOwner.lookupSymbol(name)`; etc.
- Each access overrides the full set of methods declared on it (see the .h).

## Relationship to Surrounding Files and Pipeline Stages

- Used by the `SemanticAnalyzer.cpp` assembly pipeline.
- Components (`BodyAnalyzer`, etc.) only read the context through an access, keeping field visibility minimal.
- Declared in the header `SemanticContextAccess.h`; implemented in this file.

## Further Reading

- `SemanticContextAccess.h` (declarations and the field list).
- `SemanticContext.h` (the referenced members).


---

---
kind: source-file-guide
module: sema
source: src/sema/SemanticContextAccess.h
lang: en
audience: Readers who know C/C++ and want to understand how the Sema component-splitting mechanism works.
---

# src/sema/SemanticContextAccess.h — Component-Level "Access-Face" Encapsulation

> One-line summary: defines the five `*ContextAccess` classes (`BodyContextAccess`/`DeclarationContextAccess`/`ControlContextAccess`/`TypeContextAccess`/`CompileTimeContextAccess`), exposing `SemanticContext`'s fields and methods on a per-component basis and implementing "field-level dependency injection".

## What This File Does

Semantic analysis is split into five components, but their shared state all lives in `SemanticContext`. This header defines the "access card" each component receives:

- Each `*ContextAccess` is a `friend` of `SemanticContext` and holds a `SemanticContext& mOwner`.
- Through `decltype(SemanticContext::mXxx)&` member references, it aliases the fields the component needs to the corresponding context fields (reference semantics — changes take effect directly on the context).
- Through forwarding methods, it exposes the context methods the component needs.

The purpose of this (per the header comments): keep the component algorithms' existing field names unchanged, while turning "adding a cross-component dependency" into a header-only change — components no longer need to access the whole context.

C++ analogy: like "scoped friend + reference aggregation": each component gets a view containing only the fields/methods it needs, similar to a field-level version of Interface Segregation.

## Key Structs, Classes, and Enums

- `class BodyContextAccess final`: the body-analysis component view.
  - Fields: `mConcepts`, `mConstexprFunctions`, `mConstraints`, `mCurrentFragmentDecl`, `mCurrentFunctionReturnUsage`/`mCurrentFunctionReturnsLinear`, `mCurrentModulePath`/`mCurrentPackageId`, `mCurrentReturnType`, `mDeclaredTypes`, `mFromConversions`, `mFromIteratorImplementations`, `mFunctionFamilies`, `mGeneratedInstances`, `mImpls`, `mInFunction`/`mInKernel`, `mInferenceRoots`, `mIteratorStateCounter`, `mMetadataSchemas`, `mProgram`, `mSawReturn`, `mSymTable`, `mTraitMethods`, `mTraits`.
  - Methods: `analyzeApply`/`analyzeSlotDecl`/`analyzeSlotInvoke`, reflection invocations, `constrain`/`declareFunction`/`declaredType`/const scoping, `error`, const/constraint/selector evaluation, `instantiateNominal`/`lookupSymbol`/`monomorphize`, reference recording, `requireBool`/`requireNumeric`/`requireInteger`, `resolveTypeAST`/`resolved`/`satisfiesTrait`/`setDeclarationContext`/`setDiagnosticLocation`/`sourceDeclarationKey`/`traitIdentity`/`typeIdentity`/`typeToAST`.
- `class DeclarationContextAccess final`: the declaration-collection component view (fields include `mFragments`, `mTraitOwners`, `mTraitTypeParams`, etc.; methods include `analyzeExpr`/`checkUnresolved`/`resolveTraitRef`, etc.).
- `class ControlContextAccess final`: the control-flow component view (`mApplyScopes`, `mCurrentFragmentSlot`, `mDynamicApplyScopes`, `mSlotScopes`, etc.; `analyzeBlock`/`analyzeExpr`, etc.).
- `class TypeContextAccess final`: the type component view (`mConstraints`, `mInstantiator`, `mInstantiatedFunctions`, `mQualifiedDeclarations`, diagnostic fields, etc.; `lookupDeclaredType`, etc.).
- `class CompileTimeContextAccess final`: the compile-time evaluation component view (`mActiveSelectorView`, `mConstEvaluationDepth`, `mConstScopes`, etc.; `analyzeExpr`/`resolveTypeAST`, etc.).
- Each access is a `friend class SemanticContext`, has a private constructor, and is created by `SemanticContext`'s `xxxAccess()` factories.

## Key Functions and Methods

- The five factory methods such as `SemanticContext::bodyAccess()` (implemented in the .cpp), returning the corresponding access value object.
- Each access's constructor: binds each needed context field to a member reference, one by one (initializer list).
- Each forwarding method: `return mOwner.xxx(...)` (e.g. `BodyContextAccess::constrain` → `mOwner.constrain`).

## Relationship to Surrounding Files and Pipeline Stages

- When `SemanticAnalyzer.cpp` constructs components it passes `mContext->bodyAccess()` etc.; it then re-binds with `bindXxxAnalysis`.
- Component implementations (`BodyAnalyzer`, etc.) read and write `SemanticContext` state through the access's field references.
- `SemanticContextAccess.cpp` implements the factory methods and forwarding.
- This encapsulation layer keeps the five components from depending directly on one another; they depend only on `SemanticContext`'s interface surface.

## Further Reading

- `SemanticContextAccess.cpp` (implementation).
- `SemanticContext.h` (the referenced fields and interfaces).
- Each component's .h/.cpp (consumers).


---

---
kind: source-file-guide
module: sema
source: src/sema/SymbolTable.cpp
lang: en
audience: Readers who know C/C++ and want to learn about the Luna compiler front end.
---
