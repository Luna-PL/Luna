# Types, structural identity, and nominal identity

Luna separates a type's semantic identity from its source spelling, declaration
metadata, and target-specific physical layout.

## Structural types by default

`struct` and `enum` declarations are structural by default:

```luna
struct Point { value: i32; }
struct Pixel { value: i32; }

fn read(point: Point) -> i32 { return point.value; }

fn main() -> i32 {
    let pixel = new Pixel(42);
    return read(pixel);
}
```

`Point` and `Pixel` have the same `TypeId` because their product shapes are
identical. Their names remain useful for lookup, diagnostics, reflection, and
declaration metadata, but the names do not create type identity.

The initial structural relation is exact and order-sensitive. Field/variant
names, order, element types, reference mutability, callable parameters,
results, and continuation contracts all participate in `ShapeId`. Luna does
not silently reorder or project values to claim zero-cost compatibility.

Callable ownership is also part of its structural shape. `fn view(x: T)` and
`fn take(affine x: T)` are not interchangeable even when their LLVM parameter
layout is identical.

Recursive structural declarations are rejected in the initial model. Use an
explicit nominal declaration for a recursive type.

## Explicit nominal types

Use `nominal` when a declaration must create a distinct semantic type:

```luna
nominal struct UserId { value: i64; }
nominal struct OrderId { value: i64; }
```

These types have the same shape and may have compatible physical layouts, but
they have different `TypeId` values and are not implicitly assignable. Layout
compatibility never erases a nominal safety boundary.

Traits and Metadata schemas always have declaration identity. A function
declaration also has a stable `DeclarationId`, while its callable type remains
structural.

## Type domains

Luna distinguishes:

- Value types used by ordinary execution;
- Meta types created by `meta` schemas and normally evaluated at compile time;
- Compiler types such as `declaration_view<T>`, `declaration_ref<T>`, and
  `metadata_view<M>`. These are real iterable/queryable static values even
  though they have no default runtime representation.

Inference variables and error-recovery types are Sema implementation state and
are rejected by the MoonIR verifier.

A retained `runtime@metadata(...)` instance receives a runtime encoding and
descriptor, but retention does not change the Metadata schema's type identity.
Metadata attached to one structural declaration is not inherited by another
declaration merely because the two declarations have the same shape.

## Explicit relations and reflection

The compiler exposes distinct compile-time relations:

```luna
type_same::<A, B>()
type_same_shape::<A, B>()
type_abi_compatible::<A, B>()
```

The first is semantic type equality. The second ignores a nominal brand and
compares structure. The third is currently a conservative, target-independent
precursor that accepts identical Value shapes; it will be refined by the
target Layout Engine.

Identity reflection is available through:

```luna
type_id::<T>()
type_shape::<T>()
type_domain::<T>()
type_nominal::<T>()
type_is_structural::<T>()
type_is_nominal::<T>()
type_is_meta::<T>()
```

`TypeId` and `ShapeId` are stable compact identifiers backed by canonical
payloads. Moon containers and MoonRuntime must recompute and validate the
payload rather than trusting a hash alone.

See [the type identity RFC](Arch/Type_System_Identity_RFC.md) for the compiler
model and migration plan.

## Inline sum layout and matching

Enums and `Result<T, E>` use MoonIR inline-ADT ABI v1: an eight-byte tag
storage region followed by an eight-byte-aligned payload region sized for the
largest variant. Variant order fixes tag values; field order and aligned byte
offsets are recorded in the frozen MoonIR type table. Recursive inline sums
are rejected unless recursion crosses a pointer-, reference-, `rc`-, `arc`-,
or nominal-struct boundary.

`match` evaluates its scrutinee once, checks every variant exactly once, and
binds only the active payload. A move-only sum requires `match move`; cleanup
then follows the selected arm and destroys only the active fields.

Trait implementations are globally coherent across all loaded packages. An
ordinary impl is legal only when its package owns the trait or the nominal
target type. `Drop` requires a locally owned nominal target; `From<Source>`
requires the package to own Source or Target. Structural targets therefore
cannot bypass a foreign trait's orphan boundary.

## Ownership and usage are separate

Every value contract separates its ownership relation (`owned`, shared
borrow, or mutable borrow) from its usage cardinality (`copy`, `affine`, or
`linear`). `move` is an ownership transition, not a type category.

```luna
affine let optional_owner = acquire();
linear let required_owner = acquire_required();

fn take(affine value: Resource) -> affine Resource {
    return value;
}

let next = take(move optional_owner);
```

Affine values may be discarded and are cleaned up when they own heap storage.
Linear values must be consumed on every reachable exit. Move-only owning
parameters require an explicit `move` at the call site. Field/index Places are
tracked independently, so moving `pair.left` does not invalidate
`pair.right`.

See [the ownership and affine RFC](Arch/Ownership_Affine_Model_RFC.md) for the
complete contract and MoonIR rules.

## Result values

`Result<T, E>` is a structural tagged value for recoverable failure. Its usage
is derived from both variants: it is Copy only when both payloads are Copy,
Affine when either payload is Affine, and Linear when either payload is
Linear. Cleanup reads the tag and destroys only the active payload.

Postfix `?` unwraps `Ok` and returns `Err` from the current Result-returning
function after running the same path-sensitive cleanup as an explicit return.
The error type is either identical or converted through one exact static
`From<Source> for Target` implementation. Exhaustive Result `match` binds the
active payload; `match move` transfers a move-only Result. Neither mechanism
can implicitly cross a fragment/slot boundary. See the
[Result and panic RFC](Arch/Error_Result_Panic_RFC.md).
