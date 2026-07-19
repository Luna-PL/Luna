# Version tags

## 中文说明

版本标签使用 `name@tag(x.y.z)`。调用时写 `name@tag()` 表示选择该标签下的最新
版本，写出完整版本号则固定到历史版本。函数、类型、trait、interceptor、context
和闭包都必须遵守同一套选择规则；版本选择发生在编译期，不会产生运行时分派。

Luna declarations can carry one semantic-version tag. A tag identifies an
independent release channel, while its numeric value is always
`major.minor.patch`:

```luna
fn parse @stable(1.3.2)(input: string) -> i32 { /* ... */ }
context trace @dev(2.0.0) { resume(); }
struct Message @stable(1.1.0) { code: i32; }
```

Tags are supported on functions, interceptors, contexts, structs, enums, traits, and
anonymous closures. Reusing a source name is legal only when the complete
`name@tag(version)` identity differs. Each tagged declaration receives a
hygienic internal symbol, so multiple versions cannot collide during LLVM
lowering.

## Selecting a function version

Place a selector after a function name:

```luna
parse@stable()              // newest stable declaration
parse@stable(1.1.2)         // exactly stable 1.1.2
parse@dev(1.3.3)            // exactly dev 1.3.3
```

An empty selector chooses the greatest numeric version in that tag. There is
no `@latest` keyword. Version comparison is numeric by component, so `1.10.0`
is newer than `1.2.0`.

Because the selector's parentheses are not runtime call arguments, a function
with ordinary parameters adds a second argument group:

```luna
transform@stable()(input)          // newest stable, called with input
transform@stable(1.3.2)(input)     // pinned version, called with input
```

Versioned generic functions use the normal compile-time monomorphization path:

```luna
fn identity @stable(1.0.0)<T>(value: T) -> T { return value; }
identity@stable()(42)
```

The chosen version is resolved during semantic analysis. It creates no runtime
lookup, branch, allocation, or dispatch overhead.

## Fragments and types

Versioned interceptors and contexts use the same selector inside `apply` and `default`:

```luna
apply checkpoint(trace@stable()) {
    slot context checkpoint { print(1); }
}

slot context checkpoint(value: i32) default trace@stable();
```

Named types can select a structural version in annotations:

```luna
fn handle(message: Message@stable()) -> i32 { /* ... */ }
fn migrate(old: Message@stable(1.0.0)) -> i32 { /* ... */ }
```

Different tagged type versions are nominally distinct even when their fields
have the same layout. A bare type name resolves only to an untagged declaration;
select a tag explicitly when the type has only versioned declarations.

For a package export, the public signature keeps the same identity, for example
`parse@stable(1.3.2)`. Private declarations remain absent unless explicitly
marked with `export`.

## Traits, implementations, and constraints

Traits are versioned compile-time interfaces. Their version is part of
coherence: an implementation of version 1 is never considered an
implementation of version 2.

```luna
trait Transform @stable(1.0.0) {
    fn transform(value: i32) -> i32;
}

impl Transform@stable() for i32 {
    fn transform(value: i32) -> i32 { return value; }
}

fn keep<T>(value: T) -> T where T: Transform@stable() {
    return value;
}
```

`impl Trait@stable(...) for Type` and `where T: Trait@stable(...)` resolve the
selector once during semantic analysis and retain that exact TraitId. A bare
reference to a trait that has only versioned declarations is rejected rather
than silently selecting a release. The implementation must provide every method
required by that exact trait version.

## Diagnostics

The compiler rejects an absent tag, an absent exact version, malformed versions,
and ambiguous use of a version selector on a non-function expression. A declared
version must always be complete (`x.y.z`); partial versions such as `1.3` are
not accepted.
