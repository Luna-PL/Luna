# Luna 0.2 Alpha Semantic Baseline

> Document category: language contract
> Applies to: Luna 0.2.1
> Status: Frozen for Alpha
> Normative status: normative, but limited to the type, ownership, and error boundaries listed here
> Initial implementation audit: `d0ab31c` (2026-07-31)

This document records the A0 semantic freeze. During 0.2 Alpha, compiler refactoring and
documentation cleanup must not silently change these rules. Experimental language surface
not listed here remains managed by its own RFCs and topic documents.

## 1. Freeze objective

A0 freezes how the language understands values, types, ownership, and failure. It does not
freeze every API name or machine representation. It provides the common baseline for
documentation, code separation, and Alpha release work.

## 2. Type model

### 2.1 Type domains

Luna must distinguish:

- **Value**: ordinary execution-time value types;
- **Meta**: compile-time types for Metadata schemas and instances;
- **Compiler**: declaration views, Metadata views, type parameters, and compiler lowering recipes;
- **Inference**: unresolved Sema inference variables;
- **Error**: error-recovery placeholders.

The `Inference` and `Error` domains must not enter verified MoonIR. Compiler-domain
values may remain at runtime only when the relevant feature defines an explicit runtime
encoding.

### 2.2 Identity, shape, and layout

Luna must handle these separately:

- **TypeId**: semantic type identity;
- **ShapeId**: structural shape identity;
- **ABI/layout compatibility**: physical representation compatibility.

None may substitute for another. Equal layout must not cross nominal identity, and equal
names must not bypass package/module declaration identity.

`struct` and `enum` use structural identity by default; `nominal struct` and
`nominal enum` use declaration identity. Traits and Metadata schemas always have
declaration identity. Fields, variants, order, subtypes, reference mutability, and callable
ownership contracts all participate in structural shape.

### 2.3 Types and ownership are orthogonal

A type answers “what is this value?” The ownership contract has two independent dimensions:

- relation: `owned`, `shared_borrow`, `mutable_borrow`;
- usage: `copy`, `affine`, `linear`.

`move` transfers ownership state; it is not a type category. `affine` and `linear`
qualify the use contract of an owned value and do not create a new TypeId. Reference types
carry shared or mutable borrow relations; a borrow does not take responsibility for
releasing its source value.

### 2.4 Usage derivation

0.2 Alpha must follow these rules:

- scalars, raw pointers, references, `cstr`, function values, and ordinary values without
  owned resources default to Copy;
- exclusive heap values, `string`, ordinary product instances, `rc<T>`, and `arc<T>`
  default to Affine;
- `device_buffer<T>` and `event` default to Linear;
- compiler Iterator recipes default to Affine;
- `array<T, N>` derives usage from `T`;
- enums and `Result<T, E>` use the maximum usage of every possible active payload:
  `Linear > Affine > Copy`.

An explicit function parameter or return contract may require an owned Affine or Linear
value. An unannotated move-only parameter retains borrow-view semantics and must not
silently consume the caller's ownership.

### 2.5 Type checking and conversion

- `never` is the bottom type for ordinary values; a definitely diverging expression may
  satisfy any ordinary return position;
- `auto` requests inference in source and is not a runtime type;
- unresolved inference variables must be rejected before MoonIR;
- ordinary arithmetic, comparisons, and bitwise operations require the corresponding
  operand category and unify operands to one type;
- Luna provides no general implicit numeric promotion;
- integer and floating-point literals default to `i32` and `f64`;
- call argument positions may represent integer constants at the width of a known numeric
  parameter, and a string literal may be used at a known `cstr` position; neither
  contextual rule generalizes to arbitrary value conversion.

See the [type-system reference](type_system.md) for the complete rules.

## 3. Error model

0.2 Alpha freezes these decisions:

1. recoverable failure is an ordinary `Result<T, E>` value;
2. `?` performs path-sensitive cleanup on the `Err` path just like explicit `return`;
3. error conversion statically selects one unique direct `From<Source> for Target`
   implementation and performs no runtime search;
4. `panic` is an explicit non-returning process abort; the language does not unwind its
   stack;
5. error handling does not implicitly create exceptions, effect rows, handlers, or
   continuations;
6. Luna `Result` and standard error ADTs do not cross the C ABI directly;
7. recoverable Runtime/FFI boundaries use stable machine fields, while diagnostic text is
   only an optional owned snapshot.

See the [error-model contract](error_model.md) for API status and open areas.

## 4. MoonIR trust boundary

Before MoonIR, the frontend must complete:

- type formation and inference solving;
- TypeId/ShapeId and nominal identity resolution;
- relation/usage checking;
- move, borrow, and path-state merging;
- active ADT payload and cleanup obligations;
- the error-conversion symbol and return type for `?`;
- host/device, FFI, and compile-time type boundaries.

MoonIR must preserve the verified facts required for safe backend code generation. The LLVM
backend must not rediscover traits, error conversions, ownership state, or cleanup
responsibilities.

## 5. Not frozen by this baseline

The following remain adjustable during the `0.2.1` maintenance line, but changes
must be labeled under the documentation rules:

- cross-version public FFI layout for Result/enums;
- internal layout strategies for non-64-bit targets and payloads aligned above 8 bytes;
- generic `From`, explicit static-call syntax, and error-source aggregation APIs;
- language-level adapters for `FfiError`, `RuntimeError`, and `GpuError`;
- task-local panic, panic capture, coroutine frames, and structured concurrency;
- closure environments and cross-function Core adapter Drop layout;
- the internal structure of compiler Iterator recipes;
- device operations for generic `device_buffer<T>` and additional kernel types;
- future `String`, `Vec<T>`, and other standard-library container APIs;
- selectors, external dynamic context, and multi-emission continuation ABI.

“Not frozen” does not mean implementation may change without documentation. Any
user-observable change still requires updates to the status matrix, tests, and changelog.

## 6. A0 implementation audit

This baseline was first checked against these implementation layers:

| Layer | Current fact |
|---|---|
| Parser | Recognizes usage qualifiers, references, function types, and builtin/named/generic type syntax |
| Sema | Resolves builtin types, inference, structural/nominal declarations, Result, and compile-time views |
| TypeRelations | Produces normalized descriptions for type identity and shape |
| Ownership | Records relation, usage, and cleanup actions independently |
| Layout | Computes current compiler/MoonIR value size, alignment, and inline ADT layout |
| MoonIR Verifier | Rejects Unknown/Inference and verifies type and cleanup facts |
| Codegen/Runtime | Implements JIT/AOT, Result cleanup, panic, and Runtime ABI v1 |

The audit also found these engineering issues for the code-separation phase:

- builtin-name resolution is scattered across generic `resolveType`, Sema, and some builtin-call logic;
- `TypeKind` mixes language types, Compiler recipes, Inference, and Error states;
- type printing, identity, layout, FFI, and kernel allow-lists lack a centralized registry;
- callable ownership identity is in the shape model but needs a more direct positive/negative assignment and unification matrix;
- Result, array, and ownership-wrapper domains/well-formedness still need a unified negative matrix to prevent Compiler/Meta types from entering ordinary Value containers;
- the current layout engine is a 64-bit Alpha model and must not be interpreted as a public ABI for all targets.

These issues do not change this contract; they guide later code separation and an executable
type registry.

## 7. Change gate

Changing a frozen rule in this document requires all of the following:

1. design rationale and affected boundaries;
2. updates to the type/error references and stability matrix;
3. positive, negative, and, where necessary, MoonIR/JIT/AOT/ABI tests;
4. a migration path;
5. a user-visible entry in `CHANGELOG.md`.

Code separation alone must not change this document. If separation exposes a mismatch between
implementation and this document, record the defect first, then decide separately whether to
fix the implementation or revise the next-version contract.
