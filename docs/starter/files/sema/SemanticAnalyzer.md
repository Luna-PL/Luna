# src/sema/SemanticAnalyzer.cpp — Assembling the Semantic Analyzer

> One-line positioning: constructs `SemanticContext` plus the five analysis components and binds them to one another, then forwards `analyze`/`errors`/`symTable`/`declarationReferences` outward.

## What This File Does

This is the minimal implementation of `SemanticAnalyzer` (48 lines): the constructor performs the "assembly", and the remaining methods are all forwarders. The real semantic-analysis logic lives in `SemanticContext.cpp` and the five components.

## Key Structs, Classes, and Enums

Nothing new; see `SemanticAnalyzer.h`.

## Key Functions and Methods

- Constructor:
  - `make_unique<SemanticContext>()`;
  - In order, `make_unique<BodyAnalyzer>(mContext->bodyAccess())`, `TypeResolver(typeAccess())`, `CompileTimeEvaluator(compileTimeAccess())`, `DeclarationCollector(declarationAccess())`, `ControlAnalyzer(controlAccess())` — each component receives a "component-scoped access" reference (see `SemanticContextAccess.h`);
  - then `bindBodyAnalysis`/`bindTypeAnalysis`/`bindCompileTimeAnalysis`/`bindDeclarationAnalysis`/`bindControlAnalysis` back-fill the five components' pointers into `SemanticContext`, establishing the bidirectional references.
- `~SemanticAnalyzer()`: `= default`.
- `analyze(Program*)` → `mContext->analyze(program)`.
- `errors()` → `mContext->errors()`.
- `symTable()` → `mContext->symTable()`.
- `declarationReferences()` → `mContext->declarationReferences()`.

## Relationship to Surrounding Files and Pipeline Stages

- The compiler driver calls `SemanticAnalyzer::analyze` to complete semantic analysis; internally, `SemanticContext::analyze` drives the five components in multiple passes.
- `SemanticContextAccess.h/.cpp` is the "wiring" foundation here: each component's constructor accepts the corresponding `*ContextAccess`.
- The outputs (symbol table, errors, declaration references) are consumed by later stages (TraitChecker, OwnershipChecker, MoonIR, IDE).

## Further Reading

- `SemanticAnalyzer.h` (interface).
- `SemanticContext.h/.cpp` (the `analyze` main flow and multi-pass scheduling).
- `SemanticContextAccess.h/.cpp` (component access encapsulation).


---
---
kind: source-file-guide
module: sema
source: src/sema/SemanticAnalyzer.h
lang: en
audience: Readers familiar with C/C++ who want to understand Luna's semantic analysis entry point
---

# src/sema/SemanticAnalyzer.h — The Public Facade of Semantic Analysis

> One-line positioning: `SemanticAnalyzer` is Sema's public facade: it constructs `SemanticContext` and the five analyzers, and exposes `analyze(Program*)`, the error list, the symbol table, and the declaration references.

## What This File Does

The header declares the only public class of semantic analysis, `SemanticAnalyzer`, along with a data record `ResolvedDeclarationReference` (the "source reference → target linkage name" mapping used for IDE navigation). `SemanticAnalyzer` hides the internal structure through composition:

- It holds a `SemanticContext` (the state center).
- It holds the five components: `BodyAnalyzer`, `TypeResolver`, `CompileTimeEvaluator`, `DeclarationCollector`, `ControlAnalyzer`.
- Its public interface is minimal: `analyze(Program*)`, `errors()`, `symTable()`, `declarationReferences()`.

C++ analogy: this is a typical Pimpl/facade — callers (the driver, the CLI) deal only with `SemanticAnalyzer` and never see the collaboration among the five internal components.

## Key Structs, Classes, and Enums

- `struct ResolvedDeclarationReference`: `sourcePath`/`line`/`column`/`byteLength` (reference location) plus `targetLinkageName` (target linkage name).
- `class SemanticAnalyzer`: non-copyable (deleted copy constructor/assignment); private members: `unique_ptr<SemanticContext> mContext` and the five `unique_ptr` analyzers.
- Forward declarations: `SemanticContext`, `BodyAnalyzer`, `CompileTimeEvaluator`, `ControlAnalyzer`, `DeclarationCollector`, `TypeResolver` (keeps the header lightweight).

## Key Functions and Methods

- Constructor: `make_unique<SemanticContext>()`, constructs the five analyzers with `mContext->bodyAccess()/typeAccess()/compileTimeAccess()/declarationAccess()/controlAccess()`, then binds them back into `SemanticContext` with `bindXxxAnalysis` (establishing bidirectional references).
- `~SemanticAnalyzer()`: default (the owning pointers are released automatically).
- `bool analyze(Program*)`: forwards to `mContext->analyze(program)`, returning whether there were errors.
- `errors()`: forwards `mContext->errors()`.
- `symTable()` (two overloads): forwards `mContext->symTable()`.
- `declarationReferences()`: forwards `mContext->declarationReferences()`.

## Relationship to Surrounding Files and Pipeline Stages

- It is Sema's **entry object**: called by the compiler driver after the Parser and before MoonIR verification.
- `SemanticAnalyzer::analyze` internally delegates to `SemanticContext::analyze`, which drives the five components in multiple passes (declare → analyze) (see `SemanticContext.cpp`).
- The emitted `SymbolTable` and `declarationReferences` are used by later stages (OwnershipChecker, MoonIR generation, IDE).
- The accompanying standalone checkers `TraitChecker`/`OwnershipChecker` do not belong to this class; the driver invokes them separately after the main analysis.

## Further Reading

- `SemanticAnalyzer.cpp` (assembly), `SemanticContext.h` (internal state), `SemanticContextAccess.h` (inter-component access).
- Headers of the five components: `BodyAnalyzer.h`/`TypeResolver.h`/`CompileTimeEvaluator.h`/`DeclarationCollector.h`/`ControlAnalyzer.h`.


---
---
kind: source-file-guide
module: sema
source: src/sema/SemanticContext.cpp
lang: en
audience: Readers familiar with C/C++ who want to read the main flow of Luna's semantic analysis
---
