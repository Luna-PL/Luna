# Luna 0.2 Alpha Builtin Type Inventory

> Document category: Alpha reference and implementation-status matrix
> Applies to: Luna 0.2.1
> Status: Active; stability is labeled per table entry
> Normative status: source spelling, type domains, and usage rules are normative; layout numbers are Internal Alpha ABI
> Initial implementation audit: `d0ab31c` (2026-07-31)

This is the authoritative inventory of current types. It does not equate `TypeKind` with
user-visible types, and distinguishes compiler builtins, user declarations, standard-library
declarations, Compiler recipes, and Sema-only state.

## 1. Layout convention

Sizes and alignments below come from the current 64-bit compiler/MoonIR Alpha layout engine:

- they are not a permanent promise for every target;
- they are not sufficient for crossing the C ABI;
- `type_size::<T>()` currently uses this convention;
- `unit`/`never` have value size 0, while alignment queries return 1;
- pointer-represented does not mean interchangeable with an arbitrary C pointer.

## 2. Source-visible atomic builtins

| Spelling | Domain/identity | Default usage | Size/alignment | Current semantics and status |
|---|---|---:|---:|---|
| `i8` | Value/Builtin | Copy | 1/1 | Signed 8-bit integer; Frozen for Alpha |
| `i16` | Value/Builtin | Copy | 2/2 | Signed 16-bit integer; Frozen for Alpha |
| `i32` | Value/Builtin | Copy | 4/4 | Signed 32-bit integer and default integer literal; Frozen for Alpha |
| `i64` | Value/Builtin | Copy | 8/8 | Signed 64-bit integer; Frozen for Alpha |
| `u8` | Value/Builtin | Copy | 1/1 | Unsigned 8-bit integer; Frozen for Alpha |
| `u16` | Value/Builtin | Copy | 2/2 | Unsigned 16-bit integer; Frozen for Alpha |
| `u32` | Value/Builtin | Copy | 4/4 | Unsigned 32-bit integer; Frozen for Alpha |
| `u64` | Value/Builtin | Copy | 8/8 | Unsigned 64-bit integer; Frozen for Alpha |
| `usize` | Value/Builtin | Copy | 8/8 | Current 64-bit unsigned size type; non-64-bit policy is not frozen |
| `isize` | Value/Builtin | Copy | 8/8 | Current 64-bit signed size type; non-64-bit policy is not frozen |
| `f32` | Value/Builtin | Copy | 4/4 | IEEE backend floating-point surface; Frozen for Alpha |
| `f64` | Value/Builtin | Copy | 8/8 | Default floating-point literal; Frozen for Alpha |
| `bool` | Value/Builtin | Copy | 1/1 | Condition and logical type; not currently open as a C FFI type |
| `string` | Value/Builtin | Affine | 8/8 | Owned, pointer-represented string; formatting API is not frozen |
| `cstr` | Value/Builtin | Copy | 8/8 | C-style string-pointer boundary; does not own target bytes |
| `unit` | Value/Builtin | Copy | 0/1 | No meaningful return value; Frozen for Alpha |
| `never` | Value/Builtin | Copy | 0/1 | Unconstructible bottom type; Frozen for Alpha |
| `event` | Value/Builtin | Linear | 4/4 | Launch-completion event; must be awaited/transferred; heterogeneous surface Experimental |

Although `event` is recognized by the type parser, normal values come from `launch`;
users cannot construct a valid device-event constant.

## 3. Source-visible type constructors

| Spelling | Domain/identity | Formation | Default usage | Current representation/status |
|---|---|---|---|---|
| `raw<T>` | Value/Structural builtin constructor | Exactly one `T` | Copy; explicit contract may make it a Linear owner | 8-byte raw pointer; FFI supported |
| `&T` | Value/Structural | One `T` | Copy handle; SharedBorrow relation | 8 bytes; loan checked |
| `&mut T` | Value/Structural | One `T` | Copy handle; MutableBorrow relation | 8 bytes; exclusive loan |
| `rc<T>` | Value/Structural builtin constructor | Exactly one `T` | Affine | 8-byte reference-counted handle; `clone` explicitly retains |
| `arc<T>` | Value/Structural builtin constructor | Exactly one `T` | Affine | 8-byte atomic reference-counted handle; `clone` explicitly retains |
| `array<T, N>` | Value/Structural | One `T` and non-negative compile-time integer `N` | Derived from `T` | Inline `N * size(T)`; Frozen core |
| `slice<T>` | Value/Structural | Exactly one `T` | Copy handle plus source shared loan | 16-byte `{data,length}`; currently read-only |
| `Result<T, E>` | Value/Structural | Exactly two payload types | `join(usage(T), usage(E))` | Inline ADT v1; core semantics Frozen |
| `device_buffer<T>` | Value/Structural builtin constructor | Exactly one element type | Linear | 8-byte handle; device operations currently stable mainly for `i32` |
| `(P...) -> R` | Value/Structural | Parameter sequence and return type | Copy function value; contract is part of shape | 8-byte code/closure-entry representation; closure environment not frozen |
| `affine T` | Not an independent type | Usage contract only | Affine | TypeId remains `T` |
| `linear T` | Not an independent type | Usage contract only | Linear | TypeId remains `T` |

`raw<T>` does not carry an allocator domain. Only an externally declared
`linear raw<T>` return contract expresses an ownership obligation; the FFI declarer
remains responsible for matching release.

## 4. Declaration-formed types

| Source | Domain/identity | Default usage | Current representation/status |
|---|---|---|---|
| `struct` | Value/Structural | Affine | Pointer-represented product; fields determine shape identity |
| `nominal struct` | Value/Nominal | Affine | Pointer-represented; declaration identity cannot be erased |
| `enum` | Value/Structural | Upper bound of payload usage | Inline ADT v1 |
| `nominal enum` | Value/Nominal | Upper bound of payload usage | Inline ADT v1 plus declaration identity |
| `trait` | Compiler/Nominal | Not an ordinary runtime value | Static resolution contract |
| `meta` schema | Meta/MetaSchema | Compile-time value | No ordinary runtime representation by default |
| type parameter/`Self` | Compiler/CompilerIntrinsic | Determined by instantiated type | Must be instantiated or legally retained as template fact before MoonIR |
| slot type | Value/Structural control contract | Not ordinary owning data; internal handle defaults Copy | Host-only; Once/Many is part of shape |
| fragment type | Value/Structural control contract | Not ordinary owning data; internal handle defaults Copy | Host-only; interceptor/context is part of shape |

Default product Affine usage comes from its exclusive heap representation. Structs with equal
shape may share a TypeId; nominal products remain distinct even with equal layout.

## 5. Compiler intrinsic types visible at compile time

| Spelling/internal name | Domain/identity | User-writable | Runtime | Status |
|---|---|---|---|---|
| `metadata_view<M>` | Compiler/CompilerIntrinsic | Yes, one Meta schema argument required | Erased by default | Implemented Experimental |
| `declaration_view<T>` | Compiler/CompilerIntrinsic | Yes, 0 or 1 callable argument | Erased by default | Implemented Experimental |
| `declaration_ref<T>` | Compiler/CompilerIntrinsic | Yes, 0 or 1 callable argument | Erased by default | Implemented Experimental |
| compiler Iterator recipe | Compiler/CompilerIntrinsic | Not a public named type constructor | No stable iterator ABI | Implemented Experimental |
| anonymous `Record` | Value/Structural internal form | No separate source syntax currently | Pointer-represented | Internal |

`declaration_view` is a set-valued static-selection view; `declaration_ref` is a
resolved single-declaration reference. Neither is a reflection object suitable for ordinary
FFI or long-term storage.

## 6. Sema/MoonIR-preparation internal state

| Internal item | Domain/identity | Meaning | MoonIR |
|---|---|---|---|
| `InferenceVar` | Inference/Inference | Unresolved constraint variable | Must reject |
| `Unknown` | Error/Error | Diagnostic-recovery placeholder | Must reject |
| Source `auto` | Not a type | Requests an InferenceVar | Does not appear directly |

Temporarily writing a missing type as `i32` during recovery only permits more diagnostics;
it does not give the erroneous program valid semantics.

## 7. Standard-library declaration types

These types/traits are declared by `org.luna.core`; they do not have compiler-builtin
type identity:

| Name | Actual identity | Compiler cooperation |
|---|---|---|
| `option::Option<T>` | Nominal enum | `for` protocol verifies the unique Core Option variant |
| Core error enums | Nominal enum | Use generic enum/Drop/matching rules |
| `iter::Iterator<Item>` | Nominal trait | Unique Core trait for static `for` resolution |
| `IntoIterator<Item, Iter>` | Nominal trait | Implicit, unique static conversion |
| `FromIterator<Item, Builder>` | Nominal trait | Static builder protocol for `collect` |
| `Map/Filter/Take` | Nominal enum adapters | May correspond to compiler fusion recipes |

A user trait with the same shape and method names is not a Core trait. Package/module/nominal
identity is part of protocol selection.

## 8. Current boundary matrix

| Category | Ordinary host | C FFI | Kernel | Constexpr/reflection |
|---|---:|---:|---:|---:|
| integers/floats | Yes | Yes | Supported scalars | Yes |
| `bool` | Yes | No | Structured conditions | Yes |
| `string` | Yes | No | No | Literal/compile-time string |
| `cstr` | Yes | Yes | No | Limited |
| `raw<T>` | Yes | Yes | Not safe device memory | Limited |
| references | Yes | Supported scalar references only | Buffer borrow | Reflectable as a type |
| product/enum/Result | Yes | No | Currently no | Type reflection |
| array/slice | Yes | No | Current kernel ABI no | Type/constant information |
| rc/arc | Yes | No | No | Type reflection only |
| device buffer/event | Yes | No | Dedicated ABI | No |
| Meta/Compiler views | Compile time | No | No | Yes |

This matrix describes the set allowed in 0.2; it does not promise that future versions will
always reject a category at a given boundary.

## 9. Known gaps

- builtin types are not yet driven by a centralized registry, so Parser/Sema/layout/boundary sets may drift;
- target-dependent `usize/isize` semantics are not frozen beyond the 64-bit model;
- integer-constant width selection lacks complete range diagnostics;
- inline ADT payload strategy above 8-byte alignment is not frozen;
- closure environment and cross-function Iterator-adapter Drop layout are not delivered;
- public formatting, encoding, and standard-library APIs for `string` are not frozen;
- `device_buffer<T>` formation is generic, but current device operations remain mainly fixed to `i32`;
- callable ownership shape needs a fuller assignment/unification negative matrix;
- unified well-formedness rejection for Meta/Compiler arguments in parameterized Value
  containers still needs to be completed.

These gaps must be handled as implementation or specification work; they must not be hidden
by removing the affected type from the inventory.

## 10. Evidence entry points

- type identity: `tests/fixtures/type_relations.luna`
- type domains: `tests/fixtures/type_domains_reflection.luna`
- structural/nominal relations: `tests/fixtures/structural_*.luna`
- ownership: `tests/fixtures/ownership_*.luna`
- array/slice: `tests/fixtures/safe_arrays.luna`, `tests/fixtures/slice_*.luna`
- Result/errors: `tests/fixtures/result_*.luna`
- builtins and layout: `tests/builtin_types_test.cpp`, `tests/type_size_test.cpp`
- MoonIR boundary: `tests/moonir_verifier_test.cpp`

When a row changes status, update this table, the semantic baseline, the relevant tests, and
the changelog together.
