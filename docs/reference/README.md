# Luna Semantic Reference

> Document category: reference entry point
> Applies to: Luna 0.3.0 development, with an explicitly frozen 0.2 baseline
> Status: Active
> Normative status: this page defines navigation only; each subdocument declares its own status
> Initial implementation audit: `d0ab31c` (2026-07-31)

This directory contains the current itemized language reference and the frozen Luna 0.2
Alpha migration baseline. It separates language commitments, the current implementation, internal
representation, and future plans so that an incidental compiler implementation is not
mistaken for a permanent language rule.

## Reading order

1. [Documentation recording rules](documentation_rules.md): document categories, status,
   normative scope, and change rules;
2. [Type-system reference](type_system.md): current type domains, identity, relations, inference,
   ownership, and layout model;
3. [Builtin type inventory](builtin_types.md): the authoritative inventory of source-level
   builtins, compiler intrinsics, and internal types;
4. [Error-model contract](error_model.md): `Result`, `?`, `panic`, and Runtime/FFI
   error boundaries.
5. [0.2 Alpha semantic baseline](semantic_baseline_0.2.md): frozen migration evidence;

Additional rationale and implementation notes:

- [Current architecture](../architecture.md)
- [Adopted architecture decisions](../decisions.md)
- [Runtime ABI](../runtime_abi.md)
- [Testing and regressions](../testing.md)

Architecture and decision documents cannot override a 0.2 Alpha contract that is frozen in
this directory. When a conflict is found, follow the
[documentation recording rules](documentation_rules.md) and record the discrepancy; do not
silently choose one behavior.

## A0 scope

A0 inventories semantics, separates contracts, and freezes documentation. It does not
expand the language surface as a side effect of documentation work. The current scope
includes:

- type domains, type identity, and structural/nominal relations;
- source-visible builtin types and compiler intrinsic types;
- the relationship between ownership relation, usage cardinality, and types;
- `Result`, `?`, `panic`, and Runtime/FFI error boundaries;
- the separation of language semantics, MoonIR contracts, and physical layout;
- status labels for implemented, experimental, internal, and planned capabilities.

Selectors, fragments, GPU, packages, and the standard library remain documented by their
respective topic guides. Only the portions that intersect the type or error model enter
this baseline.
