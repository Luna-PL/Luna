---
kind: source-file-guide
module: sema
source: src/sema/ControlAnalyzer.cpp
lang: en
audience: Readers familiar with C/C++ who want to understand Luna's 0.3 slot/fragment control analysis
---

# `ControlAnalyzer` — static slot/fragment control analysis

`ControlAnalyzer` implements Luna 0.3's module-level slot declarations, nominal fragment
targets, lexical single-fragment `apply`, and continuation once/many control-path checks. It
contains no dynamic slot/apply candidate scope or plugin dispatch path.

The main entry points are:

- `declareSlot` builds a nominal slot type and registers a module-level declaration.
- `finalizeSlot` resolves and verifies the slot's optional default fragment.
- `analyzeSlotDecl` rejects local slot declarations, which are outside the 0.3 surface.
- `analyzeSlotInvoke` resolves the module-level slot, checks arguments and the continuation,
  then chooses the innermost static apply or the default fragment. No binding is the identity
  fragment.
- `analyzeApply` resolves one nominal fragment and installs it in `mApplyScopes` only for the
  lexical body.
- `analyzeFragmentForSlot` checks parameter ownership contracts, captures, and continuation
  paths. Once fragments cannot resume more than once or abort after resuming; many fragments
  cannot replay linear captures.

`enterSlotScope` and `exitSlotScope` keep only the static slot and apply stacks aligned.
`selectFragment` resolves a source declaration key and diagnoses unknown fragments.

The AST and ownership checker consume the resolved nominal fragment fields; MoonIR lowering
uses the same static binding. Runtime generation switching is handled at the typed EV004 host
boundary, not by this analyzer.
