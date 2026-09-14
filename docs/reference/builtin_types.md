# Luna 0.3 Development Builtin Type Inventory

> Document category: development reference and implementation-status matrix
> Applies to: candidate Luna 0.3.0
> Status: Active development; stability is labeled per table entry
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
| `array<T, N>` | Value/Structural | One `T` and non-negative compile-time integer `N` | Derived from `T` | Inline `N * size(T)`; Frozen core |
| `slice<T>` | Value/Structural | Exactly one `T` | Copy handle plus source shared loan | 16-byte `{data,length}`; currently read-only |
| `Result<T, E>` | Value/Nominal Core declaration | Exactly two payload types | `join(usage(T), usage(E))` | `org.luna.core::result::Result`; inline ADT v1 |
| `device_buffer<T>` | Value/Structural builtin constructor | Exactly one element type | Linear | 16-byte `{data, length}` capability on 64-bit targets; device operations currently stable mainly for `i32` |
| `(P...) -> R` | Value/Structural | Parameter sequence and return type | Copy function value; contract is part of shape | 8-byte code pointer for capture-free functions; Copy-only captured closures use the C016 inline environment representation |
| `affine T` | Not an independent type | Usage contract only | Affine | TypeId remains `T` |
| `linear T` | Not an independent type | Usage contract only | Linear | TypeId remains `T` |

`raw<T>` does not carry an allocator domain. Only an externally declared
`linear raw<T>` return contract expresses an ownership obligation; the FFI declarer
remains responsible for matching release.

## 4. Declaration-formed types

| Source | Domain/identity | Default usage | Current representation/status |
|---|---|---|---|
| `struct` | Value/Nominal | Affine | Pointer-represented product; declaration identity cannot be erased |
| `enum` | Value/Nominal | Upper bound of payload usage | Inline ADT v1 plus declaration identity |
| `trait` | Compiler/Nominal | Not an ordinary runtime value | Static resolution contract |
| `meta` schema | Meta/MetaSchema | Compile-time value | No ordinary runtime representation by default |
| type parameter/`Self` | Compiler/CompilerIntrinsic | Determined by instantiated type | Must be instantiated or legally retained as template fact before MoonIR |
| slot type | Value/Structural control contract | Not ordinary owning data; internal handle defaults Copy | Host-only; Once/Many is part of shape |
| fragment type | Value/Structural control contract | Not ordinary owning data; internal handle defaults Copy | Host-only; interceptor/context is part of shape |

Named-product Affine usage currently comes from its exclusive heap representation. Distinct named
products always have distinct TypeIds even when `type_same_shape` reports equal shapes.

## 5. Compiler intrinsic types visible at compile time

| Spelling/internal name | Domain/identity | User-writable | Runtime | Status |
|---|---|---|---|---|
| `metadata_view<M>` | Compiler/CompilerIntrinsic | Yes, one Meta schema argument required | Erased by default | Implemented Experimental |
| `declaration_view<T>` | Compiler/CompilerIntrinsic | Yes, 0 or 1 callable argument | Erased by default | Implemented Experimental |
| `declaration_ref<T>` | Compiler/CompilerIntrinsic | Yes, 0 or 1 callable argument | Erased by default | Implemented Experimental |
| compiler Iterator recipe | Compiler/CompilerIntrinsic | Not a public named type constructor | No stable iterator ABI | Implemented Experimental |
| `{ x: T, y: U }` record | Value/Structural | Yes; no `record` keyword | Inline, name-canonicalized aggregate | Implemented 0.3 development |

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
| `resource::Clone` | Nominal trait | Ordinary static trait/method resolution; no `clone` intrinsic |
| `Rc<T>` / `Arc<T>` | Nominal structs | Generic/Drop/trait rules only; counting policy belongs to Core/Runtime |

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
| Core `Rc`/`Arc` | Yes | No | No | Reflected as ordinary nominal types |
| device buffer/event | Yes | No | Dedicated ABI | No |
| Meta/Compiler views | Compile time | No | No | Yes |

This matrix describes the set allowed in 0.2; it does not promise that future versions will
always reject a category at a given boundary.

## 9. Predefined type names

`i32`, `i64`, `f32`, `f64`, `bool`, and `string` are no longer lexer keywords.
Like every other predefined type name, they lex as `Identifier` and enter the
normal named-type parser path. This is a name-resolution change only: their
intrinsic `TypeKind`, `Ty*` singleton, layout, operators, canonical TypeId,
MoonIR identity, and literal-default behavior are unchanged.

`PredefinedTypes` is the authoritative registry for all atomic names (`i8`
through `never`, plus `event`) and for the arity/formation rules of `raw`,
`Result`, `device_buffer`, `array`, `slice`, `metadata_view`, `symbol_set`,
`declaration_view`, and `declaration_ref`. Both the semantic resolver and the
standalone type helper consume this registry. Atomic entries are installed in
the root type namespace, and `SymbolTable::defineType` refuses to replace any
registered predefined name.

Predefined names are immutable in the type namespace. A struct, enum, trait, or
metadata declaration with such a name, and a type parameter that would shadow
one, is rejected with `SEM0004`. The registry is consulted before generic
bindings during error recovery, so an invalid shadow never changes the meaning
of a predefined type use.

## 10. Known gaps

- layout and boundary allow-lists remain separate from the centralized name/formation registry and require cross-layer review when new types are added;
- target-dependent `usize/isize` semantics are not frozen beyond the 64-bit model;
- integer-constant width selection lacks complete range diagnostics;
- inline ADT payload strategy above 8-byte alignment is not frozen;
- non-Copy closure environments and cross-function Iterator-adapter Drop layout are not delivered; Copy-only closure environments are delivered under C016;
- public formatting, encoding, and standard-library APIs for `string` are not frozen;
- `device_buffer<T>` formation is generic, but current device operations remain mainly fixed to `i32`;
- a device buffer's element length is part of its value ABI. Runtime operations
  validate the `(data, length)` pair against the live-allocation registry, and
  kernel references lower to separate pointer and length parameters;
- callable ownership shape needs a fuller assignment/unification negative matrix;
- unified well-formedness rejection for Meta/Compiler arguments in parameterized Value
  containers still needs to be completed.

These gaps must be handled as implementation or specification work; they must not be hidden
by removing the affected type from the inventory.

## 11. Evidence entry points

- type identity: `tests/fixtures/type_relations.luna`
- type domains: `tests/fixtures/type_domains_reflection.luna`
- structural/nominal relations: `tests/fixtures/structural_*.luna`
- ownership: `tests/fixtures/ownership_*.luna`
- array/slice: `tests/fixtures/safe_arrays.luna`, `tests/fixtures/slice_*.luna`
- Result/errors: `tests/fixtures/result_*.luna`
- builtins, TypeIds, and layout: `tests/builtin_types_test.cpp`
- MoonIR boundary: `tests/moonir_canonical_test.cpp`
- Core Rc/Arc: `tests/rc_arc_core.cmake`, `tests/fixtures/rc_arc_core_app/`

When a row changes status, update this table, the semantic baseline, the relevant tests, and
the changelog together.
