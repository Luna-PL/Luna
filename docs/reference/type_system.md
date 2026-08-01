# Luna 0.2 Alpha Type-System Reference

> Document category: language contract and Alpha reference
> Applies to: Luna 0.2.1
> Status: core model Frozen for Alpha; individual surfaces are labeled by section
> Normative status: type domains, identity, relations, usage, and formation rules are normative; internal layout is not public ABI
> Initial implementation audit: `d0ab31c` (2026-07-31)

This document defines the common vocabulary and rules of Luna's current type system. See the
[builtin type inventory](builtin_types.md) for the status of every source type. See
[architecture decisions D003/D004](../decisions.md) for the rationale.

## 1. Type is not layout or ownership

For every value, Luna answers at least four separate questions:

1. **Type**: which values and operations are allowed;
2. **Identity**: whether it is semantically or structurally the same as another type;
3. **Ownership contract**: who owns it and how many times it may be consumed;
4. **Layout**: how the current target and ABI represent it.

Two types may have the same layout but different identity. One type may be owned at one
location and borrowed at another. `move` changes ownership state; it does not create a
new type.

## 2. Type domains

| Domain | Meaning | May enter verified runtime MoonIR |
|---|---|---|
| Value | Ordinary execution value | Yes |
| Meta | Metadata schema/instance | Compile-time by default; explicit retention requires a separate runtime encoding |
| Compiler | Declaration views, type parameters, fusion recipes, and other compiler values | No by default |
| Inference | Unresolved type variable | No |
| Error | Diagnostic-recovery placeholder | No |

The domain is part of type identity. A Meta schema with exactly the fields of a Value
struct is still a different type.

## 3. Identity modes

| Mode | Typical source | Identity rule |
|---|---|---|
| Builtin | `i32`, `bool`, `never` | Determined by builtin kind and parameters |
| Structural | Default `struct`, default `enum`, `Result`, functions | Determined by complete structural shape |
| Nominal | `nominal struct/enum`, traits | Determined by package/module declaration identity |
| MetaSchema | `meta` schema | Always has schema declaration identity |
| CompilerIntrinsic | Views, type parameters, Iterator recipes | Determined by compiler contract, not ordinary user layout |
| Inference/Error | Sema state | Does not form a publishable type identity |

### 3.1 TypeId

`TypeId` is semantic type identity. A default structural type derives its TypeId from its
canonical structure; a nominal type includes declaration identity and generic arguments.

### 3.2 ShapeId

`ShapeId` describes structural shape, including:

- field/variant names and order;
- field and payload types;
- generic instantiation arguments;
- shared/mutable reference categories;
- function parameters and return type;
- callable parameter/return relations and usage;
- slot/fragment control kind and Once/Many cardinality.

Equal shape after ignoring a nominal brand does not permit implicit assignment.

### 3.3 ABI compatibility

`type_abi_compatible` is currently a conservative, target-independent compatibility
relation. It cannot replace TypeId or authorize crossing nominal, FFI, or ownership
boundaries. Final machine compatibility remains constrained by target data layout and
versioned ABI.

## 4. Source type categories

### 4.1 Scalar and special builtins

The compiler directly recognizes integers, floats, `bool`, `string`, `cstr`, `unit`,
`never`, and `event`. See the [builtin type inventory](builtin_types.md) for widths,
usage, and layout.

### 4.2 Parameterized builtins

Current source syntax can form:

```text
raw<T>
&T
&mut T
rc<T>
arc<T>
array<T, N>
slice<T>
Result<T, E>
device_buffer<T>
(P1, P2, ...) -> R
```

`affine T` and `linear T` may occur in bindings, parameters, returns, and callable
contracts. They qualify usage, not the TypeId of `T`.

### 4.3 User-declared types

- `struct`: default structural product;
- `nominal struct`: nominal product;
- `enum`: default structural sum;
- `nominal enum`: nominal sum;
- `trait`: always declaration identity;
- `meta`: always MetaSchema identity.

The source language currently has no separate `record` declaration syntax.
`TypeKind::Record` is the compiler's internal anonymous structural-product form; it must
not be used to claim that record literals or record types are already a language feature.

### 4.4 Control and compile-time types

Slots, fragments, Metadata views, declaration views/refs, and compiler Iterator recipes
have specialized contracts. An internal `TypeKind` entry does not make them ordinary
storable data.

## 5. Type-formation rules

### 5.1 Named and generic types

A named type first resolves current type parameters and `Self`, then package/module-visible
declarations, and finally builtin names. Generic instantiations must provide the arguments
required by the declaration; those arguments participate in final type identity.

### 5.2 References

When `T` is a formed type, `&T` and `&mut T` are distinct:

- `&T` has a shared-borrow relation;
- `&mut T` has a mutable-borrow relation;
- shape unification cannot interchange them;
- a reference does not own `T` and cannot outlive its source loan.

### 5.3 Arrays and slices

`array<T, N>` requires:

- exactly one element type;
- `N` as a non-negative compile-time integer in source;
- length as part of type identity and structural shape.

`slice<T>` is currently a read-only, non-owning `{data, length}` view. Creating one
establishes a shared loan on the source. `raw<T>` and `device_buffer<T>` do not become
slices implicitly.

### 5.4 Result

`Result<T, E>` has exactly two payload types usable in ordinary runtime value positions.
It is a structural sum; its active payload, usage, matching, and cleanup rules are defined
in the [error-model contract](error_model.md). Sema's complete negative well-formedness
matrix for Compiler/Meta payloads remains an A0 follow-up audit item.

### 5.5 Callables

Closure/function types are written as:

```luna
(i32, &string) -> bool
(affine Resource) -> affine Resource
```

Parameter and return types, relations, and usage are part of the callable's language-level
shape. Only after MoonIR verification may the backend erase static information unnecessary
for the machine calling convention.

### 5.6 Recursion

The first version rejects infinitely inline recursive structures. Recursion must cross a
compiler-recognized representation boundary, such as a nominal pointer-represented product,
reference, `raw`, `rc`, or `arc`. Structural equality and layout computation must not
expand recursively without bound.

## 6. Inference and `auto`

`auto` requests creation of an inference variable at that source position. It is not a
type that can be reflected, stored, or passed.

Sema collects constraints in the Inference domain:

- equality, assignment, parameter, and return positions produce unification constraints;
- numeric operations produce numeric constraints;
- conditions and logical operations produce bool constraints;
- unconstrained numeric inference variables default to `i32`;
- unresolved variables produce diagnostics;
- the MoonIR Verifier rejects `InferenceVar` and `Unknown`.

Using `i32` or `Unknown` during error recovery only permits continued diagnosis; it does
not give an erroneous program valid `i32` semantics.

## 7. Literals and conversion

### 7.1 Default types

| Literal | Default type |
|---|---|
| Integer | `i32` |
| Floating point | `f64` |
| `true`/`false` | `bool` |
| String | `string` |

### 7.2 Current contextual rules

Call-argument checking may represent an integer constant at the target width in a known
numeric parameter position; complete range diagnostics are not yet implemented. A string
literal may be used in a binding, parameter, or return position known to be `cstr`.

These are contextual literal-representation rules, not:

- implicit conversion between arbitrary integer types;
- implicit conversion from arbitrary `string` to `cstr`;
- a general cast for user values;
- a claim that ABI compatibility is type compatibility.

### 7.3 Ordinary numeric operations

- `+ - * / %` require numeric operands and unify them to one type;
- `& | ^ ~` require integer operands;
- shift value and count must both be integers; the result type is the left operand's type;
- `< <= > >=` require one numeric type and produce `bool`;
- `&& || !` require `bool`;
- `== !=` require operands that can unify and produce `bool`.

0.2 makes no general implicit numeric-promotion promise. A new conversion must first
define overflow, truncation, signedness, and constexpr behavior.

## 8. Relation and usage

### 8.1 Relation

| Relation | Meaning |
|---|---|
| owned | The current position is responsible for consumption/cleanup |
| shared_borrow | Read-only loan; no release responsibility |
| mutable_borrow | Exclusive writable loan; no release responsibility |

### 8.2 Usage

| Usage | Meaning |
|---|---|
| Copy | May be reused |
| Affine | May be consumed at most once; an unconsumed value may be cleaned up by scope |
| Linear | Must be transferred, awaited, or consumed exactly once on every reachable path |

### 8.3 Default parameter rules

An unannotated Copy parameter owns its Copy value. An unannotated move-only parameter
retains shared-borrow view semantics. To consume the caller's value, the parameter must
explicitly use `affine` or `linear`, and the call site must perform the corresponding
`move`.

Reference parameters derive shared/mutable relation from `&T`/`&mut T`. Their usage is
Copy, but loan lifetimes remain checked.

## 9. Cleanup and compound types

Cleanup responsibility comes from owned-value usage and resource-management strategy:

- exclusive product/string: Drop/Deallocate;
- `rc<T>`: RcRelease;
- `arc<T>`: ArcRelease;
- Result/enum: inspect the tag, then clean only the active payload;
- array: ArrayDrop for elements that remain initialized;
- event: must be awaited or legally transferred;
- device buffer: must be explicitly released or legally transferred;
- borrow, raw Copy pointer, and scalar: do not release the source resource.

Compound usage follows `Linear > Affine > Copy`. A common path that uses only a Copy
variant cannot weaken this rule.

## 10. Current layout layers

Layout has three layers:

1. **Language semantics**: field/variant order, active payload, and identity boundaries;
2. **0.2 compiler/MoonIR Alpha ABI**: current 64-bit size, alignment, and inline ADT v1;
3. **Public Runtime/C FFI ABI**: only explicitly versioned types permitted across the boundary.

Current product types are pointer-represented; arrays and slices are inline; enum/Result
use 8-byte tag storage and an 8-byte-aligned payload. See the
[builtin type inventory](builtin_types.md) for exact numbers.

These numbers must not be generalized to 32-bit targets, cross-version Moon containers, or
the C ABI. `type_size` currently reports facts from layer 2.

## 11. Boundaries

### 11.1 C FFI

Integers, floats, `cstr`, `raw<T>`, `unit`, and references to supported scalars are
currently allowed. `bool`, `string`, products, enums, Result, shared handles, closures,
and device buffers are outside the current C ABI surface. Owning FFI returns require an
explicit `linear raw<T>`.

### 11.2 Kernels

Kernel parameters require explicit ABI types. The stable device surface is currently
primarily scalars and `&device_buffer<i32>`/`&mut device_buffer<i32>`. `string`, host
allocation, FFI, reflection, closures, and host continuations must not enter the current
device sublanguage.

### 11.3 Compile time

Constexpr currently handles scalar literals, immutable bindings, supported expressions, and
reflection results. Compiler/Meta values participate only when the compiler provides the
corresponding evaluation rule; they do not acquire a default runtime representation.

## 12. Standard-library types are not compiler builtins

`Option<T>`, error enums, `Iterator`, `IntoIterator`, `FromIterator`, `Map`,
`Filter`, and `Take` in `org.luna.core` have package/module declaration identity.

The compiler may recognize the unique Core trait and fuse it statically, but must not treat
a same-shaped user trait as the Core protocol. Internal `TypeKind::Iterator` denotes a
fusion recipe; it is not the `org.luna.core::iter::Iterator` trait and does not
automatically become a cross-function ABI.

## 13. Implementation mapping and future separation

| Responsibility | Current primary implementation |
|---|---|
| Type AST/syntax | `src/parser/AST.h`, `src/parser/Parser.cpp` |
| Type kinds and formation | `src/core/TypeSystem.h` |
| Inference/unification | `src/sema/TypeSystem.cpp` |
| Declaration resolution and builtin semantics | `src/sema/SemanticAnalyzer.cpp` |
| Identity and shape | `src/core/TypeRelations.cpp` |
| Relation/usage | `src/core/Ownership.h`, OwnershipChecker |
| Size and alignment | `src/core/TypeLayout.cpp` |
| Trusted type table | MoonIR/Verifier |
| Machine representation | CGHelpers/CodeGenerator |

Future code separation should follow these responsibilities and gradually establish a
centralized builtin-type registry. Refactoring must preserve this reference and the
[0.2 Alpha semantic baseline](semantic_baseline_0.2.md).
