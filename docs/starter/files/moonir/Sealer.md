# src/moonir/Sealer.cpp

Implementation of Sealer.h: converts "structured bodies" into "verified CFGs" in bulk, atomically.

## What This File Does

The core sealFunctionBodies follows a **two-phase commit**-style flow:

1. **Preparation phase**: iterates over module.declarations (top-level FunctionDecl + ImplDecl::methods); for each "concretely executable" function:
   - isConcreteExecutable filters: excludes extern, selector, unreachable kernel, and uninstantiated generics;
   - Validates "exactly one body" (body and controlFlow are mutually exclusive — an error if both are true or both are false); skips if controlFlow already exists;
   - Builds the graph with ControlFlowBuilder; on failure, collects the builder/verifier errors into mErrors;
   - On success, stages into pending (not written immediately).
2. Commit phase: only if mErrors is empty, move all pending controlFlow into the Module and release the bodies; otherwise return false and publish none.

## Key Functions and Methods

| Function | Purpose |
| --- | --- |
| bool sealFunctionBodies(Module&) | The main body of the prepare + commit two phases. |
| isConcreteExecutable(function) (anonymous) | Determines whether a function is a "sealable executable body". |

## Relationship to Surrounding Files and Pipeline Stages

- Calls ControlFlowBuilder::build and Verifier::verify(graph, module).
- Stays consistent with MoonIR.h through the constraint that a FunctionDecl must have exactly one of the two (body ↔ controlFlow).
- Stage: the sealing step after Lowering and before Verifier/ContainerModel.

## Further Reading

- Interface: src/moonir/Sealer.h.
- Graph construction: src/moonir/ControlFlowBuilder.h. Compile-time constraints: src/moonir/MoonIR.h.


---

---
title: Sealer interface: transactional sealing of function bodies
file: src/moonir/Sealer.h
namespace: moon
stage: MoonIR sealing (CFG construction)
---

# src/moonir/Sealer.h

Declares Sealer: atomically replaces structured function bodies (body) with a verified, normalized ControlFlowGraph.

## What This File Does

This is the **atomic transaction boundary** from body to sealed body: it invokes ControlFlowBuilder to build graphs for all concretely executable functions, and only after all graphs are constructed and verified does it uniformly swap body for CFG — ensuring the Module never falls into an intermediate state of "half bodies, half CFGs".

- As the comments state: "recipes" such as generic, selector, deferred-kernel, and fragment remain compiler input, and are only handled after a dedicated canonicalization pass is complete (currently only concrete functions are handled).
- C++ analogy: a commit-style changeset — build and verify everything first, commit only on success; if any step fails, roll back as a whole and publish no bodies.

## Key Structs and Classes

| Class | Purpose |
| --- | --- |
| class Sealer | Main entry point for sealFunctionBodies + errors(). |

## Key Functions and Methods

| Function | Purpose |
| --- | --- |
| bool sealFunctionBodies(Module&) | Iterates over top-level declarations and impl methods, building/verifying each concretely executable one, and atomically replaces body→controlFlow once all succeed. |
| errors() const | Returns the accumulated list of error strings. |

## Relationship to Surrounding Files and Pipeline Stages

- Calls ControlFlowBuilder (graph construction) and Verifier (verification).
- Operates on: FunctionDecl / ImplDecl::methods of the MoonIR Module.
- Upstream: the Module with bodies produced by Lowering; downstream: a Module with sealed CFGs for containerization.

## Further Reading

- Implementation: src/moonir/Sealer.cpp.
- Graph construction: src/moonir/ControlFlowBuilder.h/.cpp.
- Verification: src/moonir/Verifier.h.


---

---
title: Verifier implementation: MoonIR integrity validation
file: src/moonir/Verifier.cpp
namespace: moon
stage: Gate validation before sealing / containerization / loading
---
