# src/moonir/Optimizer.cpp

The current implementation of Optimizer.h: run() only calls canonicalize(module), which rebuilds the Module's index maps.

## What This File Does

Defines the MoonIR optimization boundary as: guarantee representational consistency first, then worry about performance. Currently canonicalize only performs index rebuilding:

- The lookup map is derived state and never enters the container;
- A deserialized or transformed module must rebuild its indexes here, to prevent stale frontend pointers from leaking into the backend.

- C++ analogy: a snapshot-rebuild step, similar to uniformly rebuilding the unordered_map indexes after loading.

## Key Functions

| Function | Purpose |
| --- | --- |
| Optimizer::run(Module&) | Clears errors, runs canonicalize, and decides the return value based on errors. |
| Optimizer::canonicalize(module) | Only calls module.rebuildIndexes(). |

## Relationship to Surrounding Files and Pipeline Stages

- Pairs with Optimizer.h (the interface) and MoonIR::rebuildIndexes.
- Pipeline stage: serializable post-processing that runs before Verifier.

## Further Reading
- OptimizationLevel / OptimizationRequest: see src/moonir/Optimizer.h.
- Module::rebuildIndexes: see src/moonir/MoonIR.cpp.


---

---
title: Optimizer interface: MoonIR-to-MoonIR optimization
file: src/moonir/Optimizer.h
namespace: moon
stage: MoonIR post-processing (before containerization)
---

# src/moonir/Optimizer.h

Declares the optimizer interface at the MoonIR level, deliberately kept independent from LLVM optimization; in the future this boundary can be reused by MoonRuntime after container verification.

## What This File Does

Provides the optimization entry point that normalizes the representation of a given Module. The current implementation only performs canonicalization (rebuilding indexes); true language-level transformations and runtime-hotspot versioning are reserved as MoonIR-to-MoonIR passes to be validated later, while LLVM optimization is the backend's responsibility.

- C++ analogy: the shell of a pass framework that currently contains only one index-rebuilding block.

## Key Structs, Classes, and Enums

| Member | Meaning |
| --- | --- |
| enum OptimizationLevel | None / Standard / Aggressive optimization levels. |
| enum OptimizationPurpose | AheadOfTime / JustInTime / RuntimeHotspot. |
| struct OptimizationRequest | A request combining level + purpose. |
| class Optimizer | run() is the main entry point; errors() retrieves diagnostics. |

## Key Functions and Methods

| Function | Purpose |
| --- | --- |
| bool run(Module&, const OptimizationRequest&) | Runs the optimization; currently only calls canonicalize. |
| void canonicalize(Module&) private | Rebuilds the Module's index maps, ensuring no stale frontend pointers remain after transformation or deserialization. |

## Relationship to Surrounding Files and Pipeline Stages

- Depends on MoonIR.h and diagnostics; sits in the optimization layer before Verifier / ContainerModel.

## Further Reading
- src/moonir/Optimizer.cpp: the only canonicalize implementation.
- src/moonir/Verifier.h: structural validation before and after optimization.
- src/moonir/ContainerModel.h: full serialization after optimization.

---

---
title: Printer implementation: module textualization and cost reporting
file: src/moonir/Printer.cpp
namespace: moon
stage: diagnostics / testing
---