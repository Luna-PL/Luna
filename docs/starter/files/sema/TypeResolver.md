# src/sema/TypeResolver.cpp — Type Resolution and Generic Specialization Implementation

> In one sentence: the full implementation of `TypeResolver`: type AST → `Type`, `auto`/inferred variables, generic function specialization (monomorphize), type constraint checking, and writing inferred results back.

## What This File Does

This is the core implementation of the "type" track within Sema (about 1180 lines), divided into several major parts:

1. Type resolution: `resolveTypeAST` translates a type AST into a `Type`; compared with `TypeSystem.cpp::resolveType`, it additionally performs symbol table/declaration lookup (consulting `mSymTable`/`mDeclaredTypes`) and nominal type instantiation.
2. Generic specialization: `monomorphize` plus the file-private class `MonomorphizationCloner` — deep-copies the generic function body's AST, replaces type parameters with concrete types, and produces independently compilable instances.
3. Constraints and validation: `constrain`/`requireBool`/`requireNumeric`/`requireInteger`/`checkUnresolved`.
4. Bidirectional conversion: `typeToAST` (Type → AST) and `materializeInferredTypes` (backfills all AST nodes after inference completes).

C++ analogy: `monomorphize` ≈ manual template instantiation: copy the function body AST and substitute the template parameters; `MonomorphizationCloner` ≈ a deep copier that can duplicate every node of Block/Stmt/Expr/Type.

## Key Structs, Classes, and Enums

- `MonomorphizationCloner` (file-private class):
  - Holds `typeBindings` (type parameter name → concrete type).
  - `cloneBlock`/`cloneStmt`/`cloneExpr`/`cloneType`/`cloneParam`: deep-copy the corresponding AST nodes while substituting types in fields such as `inferredType`/`resultType`/`resolvedType` (`substituteNominalType`).
  - `failure()`: records the failure reason when an unsupported node is encountered (`fail(category)`).
  - Supports all statements/expressions: let/return/if/match/while/for/free/slot/apply, lambda/call/launch/variant/record/try/move/borrow, etc.
- `TypeResolver`: the only public class; holds a `TypeContextAccess mContext`.

## Key Functions and Methods

- `findMatchingImpl`: looks up `mImpls[traitId][typeId][methodName]` and returns `FunctionDecl*`.
- `monomorphize(generic, concreteTypes)`:
  - Constructs a `luna::instantiation::Request` (generic declaration id + concrete type id list), generates a cache key with `Instantiator::keyFor`, and returns directly on a hit in `mInstantiatedFunctions`.
  - Reports an error if a previous attempt failed (`State::Failed`); otherwise creates a `FunctionDecl` instance (name = `entry.instanceId`), builds the type parameter bindings, copies the parameter/return types (substituting inferred variables and type parameters), and clones the function body with `MonomorphizationCloner`.
  - On success, caches the instance in `mGeneratedInstances` and `mInstantiatedFunctions` and calls `Instantiator.complete(requestKey)`.
- `resolveTypeAST`: first checks the AST's `resolvedType` cache (already concretized after specialization), then consults bindings/symbol table/builtin names/special types (raw/Result/device_buffer/array/slice/event/metadata_view/declaration_view/declaration_ref); when a nominal type is found, it calls `instantiateNominal`, writes back the `resolvedType` cache, and calls `recordDeclarationReference` (IDE navigation).
- `instantiateNominal`: `substituteNominalType` plus recording `typeArgs`.
- `declaredType`: `auto` → `fresh()`; otherwise `resolved(resolveTypeAST(...))`.
- `resolved`: expands via `ConstraintSolver::resolve`; for nominal types refreshes the Drop resource contract from the declaration (avoiding cached Rc-shaped instances retaining stale Copy contracts).
- `constrain`: `never` passes through; when `unify` fails, emits `Type constraint failed in <context>: <reason>`.
- `requireBool`/`requireNumeric`/`requireInteger`: mark inferred variables; reports an error when a concrete type does not match.
- `checkUnresolved`: reports "cannot infer" when `hasUnresolved`.
- `typeToAST`: converts a resolved Type back into a TypeAST (for concretizing `auto` annotations and specialization parameters).
- `materializeInferredTypes`: walks function/impl declarations, lambdas, and all kinds of expressions and statements, replacing every `inferredType`/`resultType`/`closureType`/iterator protocol field with the concrete type after `resolved`; replaces `auto` annotations with the result of `typeToAST`.

## Relationship to Surrounding Files and Pipeline Stages

- Injected by `SemanticAnalyzer` as `mTypeAnalysis`; `SemanticContext`'s `resolveTypeAST`/`constrain`/`requireXxx`/`materializeInferredTypes` all forward to it.
- Depends on `Inference.h` (`ConstraintSolver`) and on `SemanticContext`'s `mConstraints`/`mDeclaredTypes`/`mImpls`/`mSymTable`/`mInstantiator`.
- `SemanticContext::analyze` calls `defaultUnconstrainedNumeric()` in the finalization stage, then per-declaration `checkUnresolved`, then `materializeInferredTypes`.
- Output: the fully populated AST (`inferredType`/`resolvedSymbolName`/concrete type annotations) is handed directly to MoonIR generation.

## Further Reading

- `TypeResolver.h` (the interface), `SemanticContext.h` (`TypeAnalysis` and runtime state).
- `Inference.h`/`TypeSystem.cpp` (the constraint solver).
- `SemanticAnalysisSupport.h` (helpers such as `substituteNominalType`).
- `DeclarationCollector.cpp` (declaration collection and `mDeclaredTypes` population).


---

---
kind: source-file-guide
module: sema
source: src/sema/TypeResolver.h
lang: en
audience: Readers who know C/C++ and want to understand Luna type resolution
---

# src/sema/TypeResolver.h — Type Resolver (TypeAnalysis Implementation)

> In one sentence: `TypeResolver` implements the `TypeAnalysis` interface: type AST parsing, generic specialization (monomorphize), inference constraints (constrain/requireXxx), and writing inferred results back into the AST (materialize).

## What This File Does

Semantic analysis delegates all "type-related work" to a `TypeAnalysis` virtual interface, of which `TypeResolver` is the only implementation. It holds a `TypeContextAccess` (see `SemanticContextAccess.h`), which gives it access to `SemanticContext`'s state (`mConstraints`, `mDeclaredTypes`, `mImpls`, `mSymTable`, etc.).

What happens here includes: parsing type ASTs into `TypePtr`, instantiating generic nominal types, finding the matching trait impl method, constraining two types, enforcing that a type satisfies bool/numeric/integer requirements, checking for unresolved inferred variables, converting a `Type` back into an AST (`typeToAST`), and writing concrete types back into the AST after inference completes (`materializeInferredTypes`).

C++ analogy: type resolution ≈ parsing a type-annotation string into an internal type representation and performing "template instantiation"; `constrain` ≈ a compile-time static-assertion-style type equality check.

## Key Structs, Classes, and Enums

- `class TypeResolver final : public TypeAnalysis`: the only public type. It has a single private member, `TypeContextAccess mContext` — all state lives in `SemanticContext` and is accessed through the access reference (a header-only composition pattern).
- The `TypeAnalysis` pure-virtual interface itself is declared in `SemanticContext.h` (including `findMatchingImpl`, `monomorphize`, `resolveTypeAST`, `instantiateNominal`, `declaredType`, `resolved`, `constrain`, `requireBool`/`requireNumeric`/`requireInteger`, `checkUnresolved`, `typeToAST`, `materializeInferredTypes`).

## Key Functions and Methods

(Signatures are in `TypeAnalysis` in `SemanticContext.h`; semantics are listed here.)

- `findMatchingImpl(traitName, typeName, methodName)`: finds an implementation method in the `mImpls` three-key table.
- `monomorphize(generic, concreteTypes)`: generic specialization — caches the request with `Instantiator`, clones the AST (`MonomorphizationCloner` deep-copies Block/Stmt/Expr/Type) and substitutes concrete types for type parameters, producing a `FunctionDecl` instance stored in `mGeneratedInstances`/`mInstantiatedFunctions`.
- `resolveTypeAST(ast, bindings)`: resolves a type AST: first checks the `resolvedType` cache, then bindings/symbol table/builtin names/special syntax types, and finally falls through to nominal type instantiation.
- `instantiateNominal(type, args)`: applies type parameter substitution (`substituteNominalType`) to a generic nominal type and records `typeArgs`.
- `declaredType(ast, bindings)`: `auto` or a missing type → a `fresh()` inferred variable; otherwise `resolved(resolveTypeAST(...))`.
- `resolved(type)`: expands inferred variables with `ConstraintSolver::resolve`; for nominal types it also refreshes the latest Drop resource contract from the declaration.
- `constrain(actual, expected, context)`: `never` is the bottom type and passes through; otherwise `unify` is used, and a failure produces a context-bearing diagnostic.
- `requireBool`/`requireNumeric`/`requireInteger`: mark inferred variables; otherwise directly check the concrete type and report an error.
- `checkUnresolved(type, context)`: reports "cannot infer" when `hasUnresolved`.
- `typeToAST(type)`: converts a `Type` back into a `TypeAST` (replacing `auto` annotations with the inferred concrete type).
- `materializeInferredTypes(program)`: walks functions/impls/lambdas/expressions/statements, replacing `inferredType`, `resultType`, `closureType`, and iterator protocol types with the resolved concrete types, and fills in the concrete type AST for `auto` annotations.

## Relationship to Surrounding Files and Pipeline Stages

- Constructed by `SemanticAnalyzer` and injected via `SemanticContext::bindTypeAnalysis`; `SemanticContext`'s `resolveTypeAST`/`constrain` etc. forward to it.
- Consumes `ConstraintSolver` (`Inference.h`/`TypeSystem.cpp`) for inference; consumes the `mDeclaredTypes`/`mImpls` populated by `DeclarationCollector`.
- `MonomorphizationCloner` is a file-private helper class dedicated to deep-copying the generic function body AST.
- The specialization request cache `luna::instantiation::Instantiator` lives in `SemanticContext`.
- `materializeInferredTypes` is one of the final stages of Sema: it hands the fully populated AST to MoonIR.

## Further Reading

- `SemanticContext.h` (the `TypeAnalysis` interface, `mConstraints`, `mInstantiator`).
- `SemanticContextAccess.h` (`TypeContextAccess`).
- `Inference.h`/`TypeSystem.cpp` (the constraint solver).
- `DeclarationCollector.cpp` (declaration collection and nominal type registration).


---

---
kind: source-file-guide
module: sema
source: src/sema/TypeSystem.cpp
lang: en
audience: Readers who know C/C++ and want to understand how Luna type resolution and inference are implemented
---
