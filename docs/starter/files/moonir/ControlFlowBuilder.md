# src/moonir/ControlFlowBuilder.cpp

The complete implementation of ControlFlowBuilder.h (~6,300 lines): expands structured statements and expressions category by category into a canonical CFG, handling scoping, cleanup obligations, iterator recipes, fragment/slot/dynamic dispatch, and captured lambdas.

## What This File Does

The core entry point build() first clones the struct (preventing side effects and bounding nesting depth), creates the root region/scope/block, and declaratively:

1. If capture is needed, sets up the environment-parameter local and rewrites reads of captured names in the body to EnvLoad (rewriteCaptureReads*);
2. Registers each parameter as a Parameter local (and strengthens to Affine/Linear according to the usage of the frozen type);
3. Recursively lowers the whole statement tree via lowerSequence/lowerStatement, appending a TerminatorKind::Return at the end;
4. Calls canonicalizeCleanupTable() (deduplicates and normalizes the cleanup table);
5. On any error, leaves the graph unmarked as sealed and returns an empty result.

The anonymous namespace also provides the depth guard kMaxStructureDepth=4096 and the clone family (clonedNode/cloneStructured*).

## Key Functions and Methods

| Function | Purpose |
| --- | --- |
| build()×2 | Constructs the entire graph; the non-consuming variant clones first and is used for Sealer's atomic trial construction. |
| lowerSequence/lowerStatement | Expands sequences and individual statements. |
| lowerIf/lowerWhile/lowerFor/lowerMatch/lowerApply/lowerSlotInvoke/lowerResume | The various statement kinds. |
| normalizeControlFlowExpression/lowerIfExpr/TryExpr/ShortCircuit/Block/RecordAllocation/HeapAllocation | Normalizes control-flow expressions, passing the new Expr back via replacement. |
| parseIteratorRecipe/validateIteratorRecipe/bindIteratorRecipe/materializeIteratorRecipe | Iterator pipeline: parse the recipe, validate, bind, and materialize it into a local with a cursor. |
| containsIteratorTerminal/containsPendingControlFlow/containsPotentialEarlyExit/hoistOrderedOperand | Determines whether control-flow/iterator terminals that need early expansion during construction are present; hoists operands. |
| lowerCleanupObligations/canonicalCleanupOrder/canonicalizeCleanupTable | Cleanup obligations and canonical ordering. |
| bindExpr (set binding) | Binds locals/declaration references for an expression. |
| rewriteCaptureReads* | Rewrites captured references as EnvLoad. |

> The internal structures OpenBlock/BuiltBlock/FragmentContext/IteratorRecipePlan/MaterializedIteratorRecipe are described in detail in the .h guide.

## Relationship to Surrounding Files and Pipeline Stages

- Callers: Sealer::sealFunctionBodies calls build() for each concrete executable function.
- Validation: the output graph is handed to Verifier::verify(CFG), which checks region/scope/jump-cleanup invariants.
- Downstream: ContainerModel encodeCode serializes the CFG.
- Stage: the "construction half" of the sealing step.

## Further Reading

- src/moonir/ControlFlowBuilder.h: interface and internal structures.
- src/moonir/Sealer.cpp: the calling pattern for atomic trial construction.
- src/moonir/Verifier.h: graph structure checks.
- src/moonir/MoonIR.h: the ControlFlowGraph representation.

---

---
title: ControlFlowBuilder — the only construction bridge from structured statements to a canonical CFG
file: src/moonir/ControlFlowBuilder.h
namespace: moon
stage: CFG construction before MoonIR sealing
---

# src/moonir/ControlFlowBuilder.h

Declares the builder that compiles the structured statement tree (BlockStmt) produced by frontend Lowering into a single canonical ControlFlowGraph; it is the gray-box component with which Sealer seals function bodies.

## What This File Does

This is the sole bridge from "structured" to "canonical control flow." It consumes the frontend's structured body (which never remains in the sealed Module) and produces a canonical CFG composed of five tables: region, scope, block, local, and cleanup.

It does not merely expand statements; it is responsible for

- normalizing control flow for constructs such as sequences, if/while/for/match, block expressions, `?` (TryExpr), short-circuiting (&&/||), and record/heap allocation;
- parsing, validating, and materializing iterator recipes into "materialized iterators";
- graph construction for fragment apply, slot invoke, and dynamic dispatch candidates;
- synthesized environment parameters and EnvLoad rewriting for captured lambdas (C016 CL007);
- per-construct canonical cleanup ordering and obligation binding.

## Key Structs and Classes

| Member | Meaning |
| --- | --- |
| class ControlFlowBuilder | The main class; build()×2 is public, with a private lower* family of implementations. |
| struct OpenBlock | Construction-time cursor: the current block plus the active cleanups. |
| struct BuiltBlock | A constructed region/scope/entry/exit combination. |
| struct FragmentContext | Jump context for the cloned static fragment body (return/abort terminate the fragment, not the enclosing function). |
| struct IteratorRecipePlan / MaterializedIteratorRecipe | Iterator recipe: mode, source, range, step, and the materialized local/cursor. |

## Key Functions and Methods

| Function | Purpose |
| --- | --- |
| build(root, parameters, regionKind, module) ×2 | Constructs the entire graph; the non-consuming overload lets Sealer trial-construct and validate before removing the body. |
| setCaptureEnvironment(captures, closureType, envParamName) | Declarative: once enabled, build() sets the environment parameter and rewrites captured reads to EnvLoad. |
| lowerIf/While/For/lowFor/match/apply/slotInvoke/resume... | Structured expansion of the various statement kinds. |
| lowerIfExpr/TryExpr/ShortCircuit/Block/RecordAllocation/HeapAllocation | Normalization of control-flow expressions. |
| parseIteratorRecipe/validate/bind/materialize | The parse-validate-bind-materialize pipeline for iterator recipes. |
| lowerCleanupObligations/canonicalCleanupOrder/canonicalizeCleanupTable | Lowering and canonical sorting of cleanup obligations. |
| connectJump/pushBindings/popBindings/lookupLocal | Helpers for jump edges and lexical bindings. |

## Inputs / State

The builder maintains several scope stacks: mBindings, mMaterializedIterators, mSlotDefaults, mLexicalDynamicApplies, mDynamicApplyScopes, and mFragmentContexts, plus capture context such as mCaptureNames/mCaptureClosureType. mErrors collects failures; the graph is returned empty when not sealed.

## Relationship to Surrounding Files and Pipeline Stages

- Upstream: the Module with a structured body produced by Lowering.
- Its own stage: sealing — the example Sealer calls it together with Verifier, trial-constructing all candidate graphs, verifying each one, and only replacing the body with the CFG after all pass.
- Downstream: Sealer holds the graph; Verifier.verify(CFG) performs the structural checks; ContainerModel serializes the graph.

## Further Reading

- src/moonir/ControlFlowBuilder.cpp: the hundreds of functions in the implementation.
- src/moonir/Sealer.cpp: how to do atomic trial construction before building.
- src/moonir/Verifier.h: validation of the graph structure.
- src/moonir/MoonIR.h: CFG and the definitions of the various structures.
- src/moonir/MoonIR.h: CFG and the definitions of the various structures.

---

---
title: LunaLowerer implementation: from AST to MoonIR Module
file: src/moonir/Lowering.cpp
namespace: moon
stage: frontend → MoonIR lowering
---
