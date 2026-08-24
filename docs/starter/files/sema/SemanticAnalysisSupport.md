# src/sema/SemanticAnalysisSupport.h — Sema Shared Inline Utilities

> In one sentence: a set of header-only inline utilities — declaration identity/linkage name generation, type substitution, metadata keys, qualified-name splitting, and more — shared by the analysis components.

## What This File Does

Several semantic analysis components all deal with matters such as "a declaration's stable identity", "how names are assembled into linkage names", and "type parameter substitution". This header concentrates them into inline functions (avoiding duplicate definitions across translation units); there is no .cpp.

Key points:

- **Declaration identity**: `nominalDeclarationIdentity` produces a stable identity of the form `package::module::kind::symbol` (where `kind` is `struct`/`enum`/`trait`/`meta`/`fn`, etc.).
- **Linkage names**: `metadataDeclarationName` generates a `base__meta_<hash>`-suffixed linkage name for declarations carrying metadata; `isolatedLinkageName` generates an isolated linkage name of the form `__luna_<hash>_<sourceName>`.
- **Stable hashing**: `stableMetadataHash` is an FNV-1a 64-bit hash.
- **Metadata keys**: `metadataExpressionKey` converts constant expressions into stable string keys.
- **Name utilities**: `qualifiedDeclarationKey`/`splitQualifiedName`/`effectivePackageId`/`nominalTypeOwner`.
- **Type utilities**: `substituteNominalType` (recursively substitutes type parameters), `reachesInlineType` (detects inline recursive layouts).

C++ analogy: this is the equivalent of a compile-time utility function library: name mangling helpers + template parameter substitution.

## Key Structs, Classes, and Enums

No classes or enums; everything is inline free functions (mostly pure string processing, except for `substituteNominalType`).

## Key Functions and Methods

- `displayTraitRef(trait)`: returns the trait's name.
- `nominalDeclarationIdentity(program, kind, symbol, decl)`: assembles the stable identity `owner(::module)::kind::symbol`.
- `stableMetadataHash(value)`: FNV-1a 64-bit.
- `metadataExpressionKey(expr)`: constant expression → a key with an `i:`/`f:`/`b:`/`s:`/`id:`/`expr@` prefix.
- `metadataDeclarationName(base, decl)`: linkage name for declarations with metadata (`base__meta_<hash>`).
- `effectivePackageId(program, decl)`: package id precedence — declaration/program/`main`.
- `nominalTypeOwner(type)`: extracts the owner before `::` from `nominalId`.
- `qualifiedDeclarationKey(packageId, modulePath, name)`: assembles the fully qualified key.
- `splitQualifiedName(name)`: splits by `::`.
- `reachesInlineType(current, target, active)`: detects whether target is inline-reachable (array/Result/enum recurse; product types and pointer/reference/shared are representation-level barriers).
- `isolatedLinkageName(key, sourceName)`: `__luna_<hash>_<sourceName>`.
- `substituteNominalType(type, bindings)`: deep-copies `Type` and recursively substitutes `TypeParam`, preserving all semantic properties such as array/slice/closure/contract/metadata view/slot/fragment.

## Relationship to Surrounding Files and Pipeline Stages

- Included and used by `SemanticContext.cpp`, `DeclarationCollector.cpp`, `TypeResolver.cpp`, `BodyAnalyzer.cpp`, `CompileTimeEvaluator.cpp`, and others.
- The declaration identity/linkage name logic pairs with the linkage name assignment (`isolatedLinkageName`) in `SemanticContext::analyze`.
- `substituteNominalType` is a key tool for specialization (`TypeResolver::monomorphize`/`instantiateNominal`) and trait method signature matching (`BodyAnalyzer::analyzeImpl`).

## Further Reading

- `TypeResolver.cpp` (`MonomorphizationCloner` uses `substituteNominalType`).
- `SemanticContext.cpp` (linkage name assignment).
- `DeclarationCollector.cpp` (identity/key generation).


---

---
kind: source-file-guide
module: sema
source: src/sema/SemanticAnalyzer.cpp
lang: en
audience: Readers who know C/C++ and want to read Luna's semantic analysis assembly code
---
