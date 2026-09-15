# Luna 0.3 Core Freeze Boundary

> Status: Candidate contract
> Scope: Luna 0.3 observable core behavior, excluding the explicitly open areas below

This document separates a contract freeze from a source-code freeze. Frozen
behavior may still receive correctness, security, diagnostic, performance, and
internal refactoring changes when those changes preserve the observable
contract. A breaking change to a frozen protocol, artifact, or ABI requires its
normal version transition.

The candidate becomes the active core freeze only after the complete
non-hardware test suite and all supported platform CI jobs pass for one exact
Luna source commit. That commit is then the only compiler input accepted for
the Toolchains and Lunax 0.2.0 release evidence.

## Frozen candidate surface

- core syntax and semantics other than Slot/Fragment declarations,
  composition, control flow, retention, and discovery;
- nominal types, records, enums, `Result`, generics, traits, type identity,
  contextual numeric literals, and Value/Meta/Compiler domain separation;
- ownership, borrowing, cleanup, Drop, Core `Rc`/`Arc`, and owned
  Copy/Affine/Linear closure captures;
- package/module/workspace identity, manifest kinds, dependency locking, and
  the documented `check`, `analyze`, `run`, and `build` command contracts;
- the C FFI subset and Runtime host, allocator, console, filesystem, foreign
  resource, and executable-memory service ABI v1;
- diagnostic JSONL v1 and the envelope, ordering, locations, and summary rules
  of analysis JSONL v1;
- Moon/Native container headers, proof records, stable identity fields,
  integrity rules, and the non-Slot/Fragment declaration payloads;
- `usize`/`isize` only for the currently supported 64-bit target model.

Compile-time metadata/catalog/query behavior is frozen for non-Slot/Fragment
declarations. Query-only values remain erased and may not be nested inside a
Value-domain type or cross an ordinary runtime boundary.

## Explicitly open or excluded

- all Slot/Fragment spelling, semantics, nesting/re-entry, continuation,
  runtime retention, discovery, and descriptor meaning;
- mutable-slice source spelling and the future `Vec`, owning `String`,
  `Read`/`Write`, formatting, and high-level I/O APIs;
- borrowed closure captures and a public cross-function Iterator ABI;
- non-64-bit `usize`/`isize` policy;
- wider hardware GPU support, concurrency/async surfaces, and whole-toolchain
  performance budgets.

The existing Fragment=2 and Slot=8 descriptor numbers and their record layout
are reserved and must never be reused. They do not freeze Slot/Fragment
semantics. An incompatible meaning requires a new ABI/container version or an
explicit negotiated capability.

## Protocol extension rule

`luna.analysis` version 1 fixes its JSONL envelope and record structure, but
string vocabularies such as `symbol_kind` are open enumerations. Consumers must
preserve an unknown value when possible or degrade it to an unknown/generic
symbol without failing the stream. New mandatory fields or changed meanings
require a protocol version increase.

## Candidate gates

Before recording the candidate commit:

1. numeric tokens must be parsed without exceptions, contextual integer
   literals must be range checked, and unrepresentable value layouts rejected;
2. every Value-domain constructor must reject concrete Meta/Compiler-domain
   arguments, with the same invariant rechecked by the MoonIR verifier;
3. the semantic negative matrix, complete non-hardware CTest suite, and Linux,
   macOS, and Windows CI must pass;
4. Toolchains and Lunax must consume the exact candidate commit rather than a
   branch or future tag.
