# Luna 0.3 Development Architecture

> Document category: architecture note
> Applies to: Luna 0.3.0 development
> Status: Active
> Normative status: non-normative; language behavior follows the current semantic reference and 0.3 design
> Implementation audit: 2026-08-09

This document describes the current system layers, compilation pipeline, and component
boundaries. The active rationale and implementation gates are collected in the
[0.3 overall design](luna_0.3_design.md); the older architecture decisions remain a 0.2 rationale baseline.

## Design principles

- **Static first**: work that can be proved and removed at compile time is not deferred to runtime.
- **Pay for what you use**: Runtime, Reflection, Registry, Dynamic, and GPU capabilities must
  be explicitly reached by a program.
- **Stable named identity, explicit shape**: named declarations are nominal by default;
  anonymous records and explicit ShapeId relations retain structural programming.
- **Ownership and usage are orthogonal**: Owned/Borrow describes relation, while
  Copy/Affine/Linear describes consumption count.
- **Lowest capable layer**: place each policy in the lowest layer that can implement it safely.
- **Specification / implementation separation**: reference documents define behavior; this file
  explains components.

## System layers

| Layer | Current responsibility | Does not own |
|---|---|---|
| Compiler | Parsing, type and trait checking, ownership proof, MoonIR, LLVM lowering | General Runtime policy |
| Core | Minimal compiler-recognized language protocols and builtin surface | Platform services and container policy |
| Standard Library | Replaceable types, algorithms, errors, and platform adapters | Implicit changes to language semantics |
| Runtime | Versioned ABI for allocation, console/filesystem host I/O, GPU, plugins, and error snapshots | Complete language reflection |
| Dynamic | Explicit runtime discovery, selection, and plugin-extension boundary | Default participation in ordinary programs |

Runtime and Dynamic are capability layers, not an object model automatically attached to every
value. Ordinary programs should not pay for registries, descriptors, or dynamic dispatch when
they do not use those capabilities.

The Runtime's built-in default remains minimal. A compiler-generated application entry
explicitly requests the native stdin/filesystem profile; an embedding host's previously
installed service table has precedence. Library modules do not acquire host capabilities by
being linked.

## Compilation pipeline

~~~text
source/package
    -> Lexer / Parser
    -> SemanticAnalyzer / TraitChecker / OwnershipChecker
    -> verified MoonIR
    -> MoonIR optimizer
    -> LLVM lowering
    -> ORC JIT or textual LLVM IR + native AOT linker
~~~

MoonIR is the only supported intermediate contract between frontend and backend. Verification
runs both before LLVM lowering and after optimization; JIT and AOT consume the same checked
MoonIR and share host optimization levels and Runtime ABI.

## Main components

This document defines stage relationships and data flow. The repository file and responsibility
guide defines the responsibility, permitted dependencies, and forbidden boundaries of every
physical directory and file. Refactoring files alone must not cross the current Confirmed/TBD boundary.

## Key identities

- **TypeId**: language type identity; nominal types cannot merge merely because layout matches.
- **ShapeId**: structural-shape relation; it does not replace TypeId.
- **Declaration identity**: package, module, declaration family, and metadata selection together
  determine declaration identity.
- **Runtime ABI identity**: defined by versioned C structures and explicit symbols; distinct
  from source type identity.

See the type-system reference for exact type rules and Runtime ABI for the Runtime boundary.

## Static, runtime, and dynamic selection

A static selector chooses a declaration at compile time from a type-correct candidate set. A
dynamic slot permits only compiler-verified finite candidates or external interceptors meeting
the Alpha plugin ABI. The system does not accept arbitrary native function pointers or treat
environment variables as a security boundary.

Metadata extends declaration data and selection protocols. Ordinary metadata does not
automatically enter declaration identity; explicit selector/discriminator rules are required
to distinguish declaration-family members.

## Heterogeneous boundary

The --gpu-target option determines which device code the artifact contains; the
LUNA_GPU_BACKEND setting determines which backend the program selects at execution. Neither
may silently replace the other. Only reachable kernels generate device code and Runtime
entries; default tests use the CPU simulator, while hardware ROCm/CUDA tests are additional
gates.

## Documentation boundary

- “What is true now”: the docs/reference directory.
- “Why this was decided”: decisions.md.
- “How to use it”: getting started, CLI, and topic guides.
- “What comes later”: roadmap.md.
- “What actually changed”: root CHANGELOG.md.

Historical drafts and handoff records are not retained in the active documentation tree; use
Git history for traceability.
