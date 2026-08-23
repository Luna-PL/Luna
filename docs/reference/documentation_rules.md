# Luna Documentation Recording Rules

> Document category: project policy
> Applies to: Luna 0.2.1 and later documentation
> Status: Frozen for Alpha
> Normative status: normative
> Initial implementation audit: `d0ab31c` (2026-07-31)

This document defines how Luna documentation separates facts, commitments, implementation,
and plans. All new language references, architecture notes, RFCs, tutorials, and release
notes must follow these rules.

## 1. Every document must declare metadata

Normative or reference documents must declare the following after the title:

```text
Document category: language contract / Alpha reference / implementation note / RFC / tutorial / release note
Applies to: applicable Luna version
Status: Frozen for Alpha / Implemented Experimental / Internal / Draft / Planned / Superseded
Normative status: normative / partially normative / non-normative
Implementation audit: last audited commit and date
```

Older documents may acquire metadata incrementally, but an old document without a status
declaration must not be automatically interpreted as a stable language commitment.

## 2. Four kinds of fact must remain separate

### 2.1 Language contract

Describe semantics that users and tools may depend on, such as:

- `never` is the bottom type for ordinary values;
- `Result` owns only the payload selected by its current tag;
- `?` performs the same cleanup on the error path as `return`;
- nominal identity cannot be erased because layout is equal.

Language contracts use normative words such as “must,” “must not,” and “may.” A contract
change requires migration notes, positive and negative examples, and version impact.

### 2.2 Current implementation

Describe behavior already delivered by the compiler but not fully frozen, such as:

- only one-hop, concrete-type `From<Source>` is currently supported;
- kernel data operations currently expose only `i32`;
- closure environments materialize Copy captures and explicit Affine/Linear move captures;
  borrowed captures remain rejected.

Current implementation notes must state limitations and must not present future work as
delivered capability.

### 2.3 Internal representation

Describe implementation choices in Sema, MoonIR, LLVM, or Runtime, such as:

- `TypeKind::InferenceVar`;
- inline ADT tag storage;
- a compiler-fused Iterator recipe;
- the responsibility of a source file or class.

Unless a versioned ABI says otherwise, internal representation is not a source-compatibility
commitment. An implementation note must link to the relevant language contract; it must not
define language meaning in reverse from implementation detail.

### 2.4 Planned capability

Describe designs that are not implemented or not yet adopted, such as:

- generic `From`;
- language-level safety adapters for Runtime/GPU;
- task-local panic;
- a remote package registry.

Planned capabilities must use `Planned` or `Draft` and must not be mixed with current
commands, syntax, or examples as if they were usable features.

## 3. Meaning of status words

| Status | Meaning |
|---|---|
| `Frozen for Alpha` | A contract during 0.2 Alpha; changes require explicit migration notes |
| `Implemented Experimental` | Implemented and tested, but may change before the Alpha line ends |
| `Internal` | Compiler or Runtime fact; no source-compatibility promise |
| `Draft` | Under discussion and not adopted |
| `Planned` | Direction agreed, but no complete implementation |
| `Superseded` | Replaced by a designated newer document; retained only for historical rationale |

“Stable” must name its scope. “Runtime ABI v1 is stable,” “0.2 Alpha source semantics are
frozen,” and “internal layout is fixed in the current compiler” are three different
commitments.

## 4. Normative words

- **Must / must not**: an implementation and every valid program must satisfy this;
- **Should / should not**: a strong convention; deviations must explain why;
- **May**: permitted but not required;
- **Current**: describes only the implementation for the applicable version;
- **Planned / later**: not part of the current delivery.

Tutorials may use natural language, but their referenced material must state precise rules.
Examples cannot replace rules.

## 5. Sources of truth and conflict handling

Documentation is not a duplicate code comment, and code must not silently change language
behavior without a documentation change. Verify in this order:

1. adopted semantic baselines and language contracts marked `Frozen for Alpha`;
2. executable positive/negative examples and MoonIR/ABI regressions;
3. current implementation;
4. Alpha references and topic notes;
5. RFCs, roadmap, and historical design documents.

If a contract disagrees with implementation, record an implementation defect or pending
semantic decision; do not hide the discrepancy by rewriting a tutorial. If two normative
documents conflict, designate one as authoritative and mark the other `Superseded` or
downgrade it to a non-normative note.

## 6. Minimum record for type documentation

Every builtin type or type constructor must record at least:

| Field | Content |
|---|---|
| Source spelling | How users write it; state explicitly if it is not writable |
| Type domain | Value, Meta, Compiler, Inference, or Error |
| Identity mode | Builtin, Structural, Nominal, MetaSchema, CompilerIntrinsic |
| Formation rules | Parameter count/kinds, constants, and recursion limits |
| Value source | Literal, constructor, call result, or compiler-only |
| Type relations | How TypeId, ShapeId, and nominal identity participate in equality |
| relation/usage | Ownership relation and Copy/Affine/Linear rules |
| Cleanup | Drop, release, reference counting, or none |
| Layout | Whether language guarantees it; label internal ABI explicitly |
| Conversion | Implicit, contextual, and explicit conversions |
| Boundaries | Whether FFI, kernel, constexpr, and Runtime allow it |
| Stability | Contract, experimental, internal, or planned |
| Evidence | Relevant tests, MoonIR verification, or ABI regressions |

`TypeKind` is not a source-type inventory. An enum member may represent a user type,
compile-time value, lowering recipe, inference state, or error-recovery placeholder; the
documentation must classify it first.

## 7. Examples and tests

- Every language contract needs at least one positive example and one relevant negative example;
- ownership, cleanup, or ABI rules require JIT/AOT or MoonIR regression coverage;
- examples should use syntax parsable by the current CI toolchain;
- planned syntax may appear only in code blocks labeled `Draft`/`Planned`;
- runnable tutorial examples should reuse test fixtures or state their synchronization owner;
- performance numbers must record environment, sampling method, and limitations, and must not
  be turned into semantic commitments.

## 8. Versioning and change records

A semantic change must complete all of the following:

1. update the semantic baseline or state explicitly that it remains unchanged;
2. update references and status matrices;
3. add or update positive, negative, and required ABI tests;
4. record user-visible impact in `CHANGELOG.md`;
5. provide migration notes for frozen behavior;
6. verify that English and Chinese entry points still reference the same facts.

A code-only refactor that does not change semantics should update component mappings in the
implementation notes, not rewrite the language contract.

## 9. Links and duplication

- Each rule has one authoritative definition;
- tutorials, overviews, and READMEs summarize and link to the authority;
- RFCs explain “why,” while references explain “what is true now”;
- physical layout links to one layout section instead of copying numbers across tutorials;
- standard-library types and compiler builtins remain separate even when their names resemble each other;
- adopted RFC rationale should be condensed into `docs/decisions.md`, rather than maintained
  in parallel with the current reference;
- a superseded document should be deleted after unique content is migrated, inbound links are
  updated, and link checks pass; Git history is the archive, not a maintained `docs/archive/`;
- temporary status, fix records, and phase completion belong in `CHANGELOG.md`, not in a
  permanent topic page.

## 10. Documentation maintenance gate

Every documentation change must confirm that:

- all relative links exist and every active topic is reachable from at least one entry point;
- no existing authoritative rule is redefined;
- planned capability and current implementation remain clearly separate;
- performance numbers retain their environment and conclusion boundaries;
- semantic facts agree with the corresponding regressions, implementation, and release record.

## 11. Bilingual documentation rules

“Bilingual” applies to human-facing Markdown documents and user-visible help. It does not
apply to source code, ASTs, test fixtures, lockfiles, original license text, or machine
configuration; copying these into translations would create two inconsistent sources of truth.
User-visible diagnostics and CLI help in source must retain stable English identifiers. Chinese
translations belong in documentation or a future localization resource layer, not in a second
copy of compiler logic.

The default document filename must be English, for example `docs/alpha_release.md`. The
Chinese counterpart uses the same name with the `.zh-CN.md` suffix, for example
`docs/alpha_release.zh-CN.md`. Each pair must share version, status, normative status, and
implementation-audit metadata, and must link to each other. The English file is the default
entry point for CI, release notes, and README files.

The existing `getting_started.en.md` is a historical transitional name. After the migration,
English should become `getting_started.md` and Chinese `getting_started.zh-CN.md`; only one
short-lived redirect document with a migration note may remain under the old name.

A translation must faithfully express the same authoritative rule: code blocks, type spelling,
error codes, commands, paths, and version strings must not be translated; explanatory prose
may be translated. When either language version is added or changed, update the other as well,
and keep both discoverable in the file guide.
