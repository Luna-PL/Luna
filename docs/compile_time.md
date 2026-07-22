# Compile-time computation and reflection

## 中文说明

Luna 支持 `const`、`constexpr` 和编译期反射。编译器会在参数全部可求值时折叠
`constexpr` 调用，并在类型检查阶段处理 `type_of`、字段查询和完整类型描述。
当前求值器有递归深度限制，不能把运行时输入伪装成编译期值。

Luna has immutable compile-time bindings and compile-time functions:

```luna
constexpr fn double(n: i32) -> i32 { return n * 2; }
const let answer = double(21);
```

`const let` initializers must be evaluable during compilation. `constexpr fn`
calls are folded when all arguments are compile-time values. The evaluator
supports literals, immutable bindings, arithmetic/bitwise/comparison/logical
expressions, recursive `constexpr` calls (depth limit: 128), local `let`
bindings, returns, and compile-time `if` branches.

Closure *types* omit the old `fn` prefix: `(i32, string) -> bool`. Lambda
expressions keep `fn`, for example `fn(x: i32) -> i32 { return x + 1; }`.

Reflection queries are compile-time built-ins. They take either a type argument
(`query::<Point>()`) or, where applicable, a value (`type_of(point)`). Their
results are normal compile-time strings, integers, or booleans:

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

The compiler emits diagnostics for non-constant `const` initializers, invalid
reflection targets, and out-of-range reflection indexes.
