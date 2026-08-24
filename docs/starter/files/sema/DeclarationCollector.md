# src/sema/DeclarationCollector.cpp — Declaration Collection Implementation

> In one sentence: the complete implementation of `DeclarationCollector`: registering functions/metadata/constraints/fragments/structs/enums/traits/impls, and validating metadata and FFI shapes.

## What This File Does

Implements the `DeclarationCollector.h` interface (about 590 lines). The core idea: before analyzing any function body, register "declaration shapes" as queryable state (symbol table, `mDeclaredTypes`, `mImpls`, `mTraits`, `mMetadataSchemas`, etc.), so that forward references hold regardless of declaration order.

## Key Structs, Classes, and Enums

No new types; uses the fields of `DeclarationContextAccess` and `SemanticContext`. File-local lambda: `registerMethod` inside `declareImpl` (registers the symbol and contract of a single impl method).

## Key Functions and Methods

- `declareFunction`:
  - Organizes `mFunctionFamilies[sourceKey]` by `sourceKey = qualifiedDeclarationKey(package, module, name)` (overload families sharing the same name).
  - Handles where clauses (`resolveTraitRef` / constraint lookup); resolves return and parameter types (`declaredType` + typeParams bindings), computes `usage`/`relation` (`parameterContractFor`/`defaultUsageForType`), writes `paramContracts`.
  - The first function with the same name calls `defineAtRoot`; always writes `defineLinkage`; `isConstexpr` registers `mConstexprFunctions`; extern/exported functions with an ABI call `validateFFIFunction`.
- `declareMeta`: deduplicates schemas; field types via `declaredType`; `Type::makeMetadata(identity, fields)` registers `mMetadataSchemas` and the symbol table; constructs the metadata constructor `SymbolInfo`.
- `declareConstraint`: registers `mConcepts[sourceKey]`, deduplicating type parameters.
- `analyzeConstraint`: enters scope, binds type parameters → `analyzeExpr(predicate)` → requires bool/inferred variable → exits scope.
- `analyzeMeta`: `checkUnresolved` per field.
- `validateMetadata(Decl*)`: for each attachment: looks up the schema (`sourceDeclarationKey`), sets `resolvedSchemaId`, checks the argument count, `evaluateConstExpr` evaluates each argument and `constrain`s it to the schema field by value type, advances `retention` (Runtime/Dynamic upgrades).
- `declareFragment`: registers `mFragments`; resolves parameters and contracts; `Type::makeFragment` struct type; `defineAtRoot`.
- `isFFIType`: after `resolved`, whitelists by `TypeKind` (integers/floats/cstr/raw/unit/recursing into references), otherwise reports an error.
- `validateFFIFunction`: ABI must be C; extern/export/constexpr/generic are mutually exclusive; linear return must be `linear raw<T>`; `isFFIType` for each parameter and the return value.
- `declareStruct`/`declareEnum`: fetch/create `mDeclaredTypes[identity]` (pre-bound in `SemanticContext::analyze`), clear fields/variants, then refill via `resolveTypeAST`; register symbol table and type table.
- `declareTrait`: rejects the reserved names `Drop`/`From`; `traitIdentity` sets `resolvedTraitId`; registers `mTraits`/`mTraitTypeParams`/`mTraitOwners`; registers via `Type::makeTrait`.
- `declareImpl`:
  - Resolves `resolvedTraitId`/target type/owner; orphan rules (`From`: at least one of target/source is in the package; `Drop`: must own the target; ordinary trait: owned by the trait or the target).
  - `From` special case: single source type parameter, `mFromConversions` registration, method symbols `FromTraitId__<src>__for__<tgt>__<method>`.
  - `FromIterator` special case: resolves item/builder/target, collects begin/push/finish into `mFromIteratorImplementations`.
  - General case: checks the trait type parameter count, deduplicates `mImpls[traitId][targetId]`, registers each method (`traitId__for__targetId__methodName`).

## Relationship to Surrounding Files and Pipeline Stages

- Driven by the declaration pass of `SemanticContext::analyze` (the various declare/analyze methods in the `.cpp`).
- Consumes `DeclarationContextAccess` (`mContext`) to read and write `SemanticContext` state.
- Produces output consumed by `TypeResolver` (type resolution/specialization), `BodyAnalyzer` (impl body validation, Drop contracts), and `ControlAnalyzer` (fragment registration).
- `resolveTraitRef`/`typeIdentity` etc. are forwarded to `SemanticContext` via access.

## Further Reading

- `DeclarationCollector.h` (interface).
- `SemanticContext.cpp` (multi-pass scheduling).
- `SemanticAnalysisSupport.h` (`nominalDeclarationIdentity`/`effectivePackageId`/`nominalTypeOwner`).
- `BodyAnalyzer.cpp` (`analyzeImpl` consumes the impl table for method signature validation).


---

---
kind: source-file-guide
module: sema
source: src/sema/DeclarationCollector.h
lang: en
audience: Readers familiar with C/C++ who want to understand Luna's declaration collection.
---

# src/sema/DeclarationCollector.h — Declaration Collector (DeclarationAnalysis Implementation)

> In one sentence: `DeclarationCollector` implements `DeclarationAnalysis`: it walks every declaration and registers "name → symbol/type" into `SemanticContext`, while validating declaration shapes such as FFI, metadata, and constraints.

## What This File Does

The first pass of semantic analysis is "declaration collection": before checking any function body, it registers the names, signatures, types, and contracts of all declarations into the symbol table and registries, ensuring forward references hold. `DeclarationCollector` is the component for this pass:

- `declareFunction/Meta/Constraint/Struct/Enum/Trait/Impl/Fragment`: registration for each declaration kind.
- `analyzeConstraint/analyzeMeta`: small post-declaration checks (predicate must be bool, metadata fields have no unresolved types).
- `validateMetadata(Decl*)`: validates any declaration's metadata attachments (schema exists, argument count, compile-time values).
- FFI checks: `isFFIType`/`validateFFIFunction`.

C++ analogy: this is roughly the stage in C++ compilation where "all declarations are scanned first to establish scope/type registration" (similar to first declaring, then defining, each namespace/class).

## Key Structs, Classes, and Enums

- `class DeclarationCollector final : public DeclarationAnalysis`: the only public type; private member `DeclarationContextAccess mContext`.
- The `DeclarationAnalysis` interface (in `SemanticContext.h`): `declareFunction`/`declareMeta`/`declareConstraint`/`analyzeConstraint`/`analyzeMeta`/`validateMetadata`/`declareFragment`/`isFFIType`/`validateFFIFunction`/`declareStruct`/`declareEnum`/`declareTrait`/`declareImpl`.

## Key Functions and Methods

(Semantics are described in the .cpp guide; responsibilities are listed here.)

- `declareFunction`: resolves parameter/return types (`declaredType` + bindings), computes ownership contracts (`parameterContractFor`/`defaultUsageForType`), writes `mFunctionFamilies`/`mSymTable.defineAtRoot`/`defineLinkage`, registers `constexpr` functions, and calls `validateFFIFunction` when necessary.
- `declareMeta`: registers metadata schemas (`mMetadataSchemas`), constructs the metadata type and its "constructor" symbol.
- `declareConstraint`: registers the constraint name and deduplicates type parameters.
- `analyzeConstraint`: analyzes the predicate after binding type parameters in a local scope, requiring its type to be bool/inferred variable.
- `analyzeMeta`: checks that field `inferredType` has no unresolved types.
- `validateMetadata`: validates attachment schema/argument count, `evaluateConstExpr` evaluates and `constrain`s to schema field types, handles retention upgrades.
- `declareFragment`: registers fragments, constructs the `Type::makeFragment` struct type and ownership contracts.
- `isFFIType`: recursively determines whether a type is representable in the C ABI (scalars, cstr, raw, references, etc.), otherwise reports an error.
- `validateFFIFunction`: validates that the ABI is C, that extern is not mixed with export/constexpr/generics, `linear raw<T>` returns, etc.
- `declareStruct`/`declareEnum`: resolves fields/variants into `TypeField`/`TypeVariant` and fills them into the pre-bound types in `mDeclaredTypes` (using `resolveTypeAST` + bindings).
- `declareTrait`: rejects reserved names (Drop/From), registers `mTraits`/`mTraitTypeParams`/`mTraitOwners`, registers the trait type.
- `declareImpl`: resolves the trait reference and target type, registers `mImpls`; special-cases `From` (one-to-one conversion table), `Drop` (orphan rules), and `FromIterator` (builder protocol table); the `registerMethod` lambda uniformly registers method symbols.

## Relationship to Surrounding Files and Pipeline Stages

- `SemanticContext::analyze` calls it during the declaration pass (declareMeta/Constraint/Struct/Enum/Trait/Function/Fragment/Impl).
- Accesses `SemanticContext` via `DeclarationContextAccess` (`mSymTable`/`mDeclaredTypes`/`mImpls`/`mMetadataSchemas`, etc.).
- Its registration results (`mDeclaredTypes`/`mImpls`/`mTraits`/symbol table) are consumed by `TypeResolver`/`BodyAnalyzer`/`ControlAnalyzer`.
- Uses utilities from `SemanticAnalysisSupport.h`: `nominalDeclarationIdentity`/`qualifiedDeclarationKey`/`effectivePackageId`, etc.

## Further Reading

- `DeclarationCollector.cpp` (implementation).
- `SemanticContext.h` (`DeclarationAnalysis` interface and registry fields).
- `SemanticAnalysisSupport.h` (identity/key utilities).
- `BodyAnalyzer.cpp` (consumes impls/traits for body checks).


---

---
kind: source-file-guide
module: sema
source: src/sema/Inference.h
lang: en
audience: Readers familiar with C/C++ who want to understand Luna's type inference.
---
