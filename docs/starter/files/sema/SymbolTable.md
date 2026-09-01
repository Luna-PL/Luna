# src/sema/SymbolTable.cpp — Implementation of the Symbol Table

> One-line summary: implements the interface declared in `SymbolTable.h`: entering and exiting the scope stack, reading and writing symbols/types/linkage symbols, and duplicate detection.

## What This File Does

This is the complete implementation of `SymbolTable` (about 70 lines). The constructor automatically calls `enterScope()` to establish the package-level root scope; subsequently, each function/block body pushes one scope on entry and pops one on exit, so that names declared within a lexical scope automatically become invalid when the block ends.

C++ analogy: it is equivalent to recording the names of a block with a local `std::unordered_map` inside a piece of code — when the block ends, the map is destroyed and the names disappear — except that here push/pop is controlled manually.

## Key Structs, Classes, and Enums

This file introduces no new types/enums; all data structures (`SymbolKind`, `SymbolInfo`, and the three tables of `SymbolTable`) are declared in `SymbolTable.h`.

## Key Functions and Methods

- Constructor `SymbolTable()`: calls `enterScope()`, ensuring that `mScopes` is non-empty and `mScopes[0]` is the root scope.
- `enterScope()`/`exitScope()`: `emplace_back()` pushes a scope; `exitScope` calls `pop_back()` only when `size()>1`, to avoid accidentally removing the root scope.
- `define(name, info)` / `defineAtRoot(name, info)`: first checks for duplicates with `count`, then writes to `mScopes.back()` / `mScopes.front()`; returns `false` on a duplicate name.
- `defineLinkage(name, info)` / `lookupLinkage(name)`: read and write `mLinkageSymbols` (linkage-level symbols, independent of scopes).
- `lookup(name)`: iterates over `mScopes` from `rbegin` to `rend`, returning the first hit; returns `nullptr` if not found.
- `lookupDepth(name)`: like `lookup`, but returns the depth of the scope in which the name lives (0-based) or `static_cast<size_t>(-1)`; closure capture analysis relies on it to distinguish "lambda-local bindings vs. outer free variables".
- `hasInCurrentScope(name)`: only queries `mScopes.back()`.
- `defineType`/`lookupType`: read and write `mTypeMap` (name → `TypePtr`).
- `visibleSymbols()`: merges the names of every scope into a single `unordered_map` snapshot (inner scopes shadow outer ones for the same name), for use by the outer capture analysis.

## Relationship to Surrounding Files and Pipeline Stages

- The `SymbolTable` instance is a member of `SemanticContext` (`mSymTable`) and is exposed to the outside by `SemanticAnalyzer` (`symTable()`).
- Declaration stage: `DeclarationCollector` writes declarations with `defineAtRoot`/`defineLinkage`/`defineType`.
- Body analysis stage: `BodyAnalyzer` calls `enterScope` when entering a function/block and `exitScope` when leaving, and uses `lookup` to bind identifiers.
- Ownership checking: `OwnershipChecker::check` receives a `SymbolTable&` directly to read contract information.

## Further Reading

- `SymbolTable.h`: data structures and interface declarations.
- `SemanticContext.cpp`: initialization and cleanup of scopes/symbol tables in `SemanticContext::analyze`.
- `DeclarationCollector.cpp` / `BodyAnalyzer.cpp`: the main writers and readers of the symbol table.


---

---
kind: source-file-guide
module: sema
source: src/sema/SymbolTable.h
lang: en
audience: Readers familiar with C/C++ who want to study the Luna compiler frontend
---

# src/sema/SymbolTable.h — A Scope-Aware "Name → Symbol" Container

> One-line summary: `SymbolTable` is the symbol table of semantic analysis: it binds names (variables/functions/types/fragments, etc.) to their corresponding symbols and records the scope depth at which each name lives; name resolution and closure capture analysis both rely on it.

## What This File Does

Luna semantic analysis needs to answer "what does the name `foo` refer to." `SymbolTable` provides a name → `SymbolInfo` table layered by lexical scope:

- `SymbolInfo` caches all the information computed for each symbol across the semantic stages (type, ownership contract, whether it is a generic template, etc.).
- `SymbolTable` hosts lookup with a scope stack plus a global type table plus a linkage-symbol table.

C++ analogy: it is like a "scope-qualified symbol dictionary" — analogous to a hash table that is local to a function and becomes invalid as soon as it goes out of scope, but one that also remembers "which scope it was defined in" to support lexical scoping.

## Key Structs, Classes, and Enums

- `enum class SymbolKind`: `Variable, Function, Fragment, Slot, Struct, Trait, TypeParam, Metadata` (symbol kinds).
- `struct SymbolInfo`: the complete semantic summary of a symbol:
  - `kind`: the symbol kind; `type`/`paramTypes`/`returnType`: the type and signature.
  - `typeParams`: the sequence of generic parameter names; `genericDecl`: the original declaration of the generic template (for specialization).
  - `isLinear`/`usage`/`relation`: the ownership contract (`luna::ownership::Usage/Relation`).
  - `isConst`/`isExported`/`isExtern`/`returnsLinear`/`returnUsage`/`paramContracts`, etc.
  - `compileTimeDeclarationId`: a `declaration_ref` value used by the frontend only and erased before MIR.
  - `isCompileTimeSymbolSet`/`isCompileTimeQueryDeclarationView` and their declaration-ID vectors preserve locally rebound query membership and order; query-reference/optional-payload flags enforce call/return non-escape.
- `class SymbolTable`:
  - `mScopes`: the scope stack (`vector<unordered_map<string, SymbolInfo>>`); `mScopes[0]` is the package-level root scope.
  - `mTypeMap`: the global "type/trait registry" (name → `TypePtr`).
  - `mLinkageSymbols`: linkage-level symbols (`defineLinkage`/`lookupLinkage`).

## Key Functions and Methods

- `enterScope()`/`exitScope()`: push or pop one scope, naturally implementing "block ends, names become invalid".
- `define`/`defineAtRoot`: define a symbol in the current innermost/root scope; returns `false` on a duplicate name.
- `defineLinkage`/`lookupLinkage`: read and write on `mLinkageSymbols` (for declarations with an established linkage name).
- `lookup(name)`, `lookupDepth(name)`, `lookupLinkage(name)`: look up ordinary/linkage symbols from innermost to outermost; `lookupDepth` returns the depth of the scope in which the name lives (counting from 0) or `SIZE_MAX`, for closure capture analysis to judge whether the name is a free variable from an outer scope of the lambda body.
- `hasInCurrentScope`: looks only at the innermost scope; `visibleSymbols()`: merges the names of every scope into a snapshot (used when capturing from an outer scope).
- `defineType`/`lookupType`: operate on `mTypeMap`.

## Relationship to Surrounding Files and Pipeline Stages

- It is a member of `SemanticContext` (`mSymTable`), and all analyzers share the same instance.
- The declaration stage (`DeclarationCollector`) writes into the table; the function/block-body stage (`BodyAnalyzer`) enters and exits `enterScope` to bind lexical names; types are registered via `defineType`.
- Lambda capture analysis distinguishing "lambda-local bindings vs. outer free variables" relies on `lookupDepth`.
- `OwnershipChecker` also reads `SymbolTable` to obtain names and contracts.

## Further Reading

- `SymbolTable.cpp` (implementation), `SemanticContext.h` (host of runtime state), `DeclarationCollector.h` (writer), `BodyAnalyzer.h` (main reader/writer).


---
---
kind: source-file-guide
module: sema
source: src/sema/TraitChecker.cpp
lang: en
audience: Readers familiar with C/C++ who want to study the trait check implementation
---
