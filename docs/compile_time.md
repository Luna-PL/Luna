# Compile-time Computation and Reflection

Luna supports `const`, `constexpr`, and compile-time reflection. The compiler folds
`constexpr` calls when all arguments are evaluable and processes `type_of`, field queries,
and complete type descriptions during type checking. The current evaluator has a recursion
depth limit and cannot disguise runtime input as a compile-time value.

Luna has immutable compile-time bindings and compile-time functions:

```luna
constexpr fn double(n: i32) -> i32 { return n * 2; }
const let answer = double(21);
```

`const let` initializers must be evaluable during compilation. `constexpr fn` calls are
folded when all arguments are compile-time values. The evaluator supports literals, immutable
bindings, arithmetic/bitwise/comparison/logical expressions, recursive `constexpr` calls
(depth limit: 128), local `let` bindings, returns, and compile-time `if` branches.

Closure *types* omit the old `fn` prefix: `(i32, string) -> bool`. Lambda expressions keep
`fn`, for example `fn(x: i32) -> i32 { return x + 1; }`.

Reflection queries are compile-time built-ins. They take either a type argument
(`query::<Point>()`) or, where applicable, a value (`type_of(point)`). Their results are
normal compile-time strings, integers, or booleans:

| Query | Result |
| --- | --- |
| `type_of::<T>()` | Full type spelling, including type arguments |
| `type_kind::<T>()` | `struct`, `enum`, `reference`, `integer`, etc. |
| `type_id::<T>()` / `type_shape::<T>()` | Stable semantic type identity / structural shape identity |
| `type_domain::<T>()` | `value`, `meta`, or `compiler` |
| `type_nominal::<T>()` | Nominal declaration identity, or an empty string |
| `type_size::<T>()` | Static layout size in bytes |
| `type_is_struct::<T>()`, `type_is_enum::<T>()`, `type_is_nominal::<T>()`, `type_is_structural::<T>()`, `type_is_meta::<T>()`, `type_is_reference::<T>()` | Type predicates |
| `type_same::<A, B>()` / `type_same_shape::<A, B>()` | Exact semantic identity / structural-shape relation |
| `type_abi_compatible::<A, B>()` | Conservative target-independent ABI compatibility precursor |
| `type_field_count::<T>()` | Struct/record field count |
| `type_field_name::<T>(index)` / `type_field_type::<T>(index)` | Field metadata; index must be a compile-time integer |
| `type_variant_count::<T>()` | Enum variant count |
| `type_variant_name::<T>(index)` / `type_variant_field_count::<T>(index)` | Enum-variant metadata |

The compiler emits diagnostics for non-constant `const` initializers, invalid reflection
targets, and out-of-range reflection indexes.

## Named constraints

`constraint` declares a C++-concept-style named compile-time boolean predicate over types:

```luna
constraint SmallValue<T> =
    type_size::<T>() <= 8;

constraint PlainSmallValue<T> =
    !type_is_meta::<T>() && SmallValue::<T>();

fn keep<T>(value: T) -> T where PlainSmallValue<T> {
    return value;
}
```

Constraints may compose other constraints and use boolean, comparison, and compile-time
type-reflection operations. They are substituted and evaluated at generic instantiation
sites. A false predicate rejects the instantiation at the `where` boundary; a predicate
that cannot be evaluated is also a compile error. Constraint declarations are discharged
before MoonIR and have no runtime representation or fallback check.

Trait bounds and constraints are intentionally distinct:

```luna
fn first<T>(value: T) -> T where T: Sequence { return value; }
fn second<T>(value: T) -> T where SmallValue<T> { return value; }
```

The first asks for an implemented behavior; the second asks the compiler to prove a named
proposition. C++-style `requires` expressions that test whether arbitrary source expressions
are well formed are not part of this initial constraint release.

## Static declaration reflection

Known declarations can be reflected without metadata or selection:

```luna
let known = declaration_of::<(i32) -> i32>(answer);
print(declaration_id(known));
print(declaration_signature(known));
```

The optional callable type disambiguates ordinary overloads. The resulting
`declaration_ref<T>` exists only during compilation and is erased after its static queries
are folded. If name and signature still leave an open declaration family, use a static selector
instead.
