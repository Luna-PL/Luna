# Metadata-based release selection

Luna no longer has compiler-defined SemVer tags, channels, `latest`, or
postfix `name@tag(...)` selection. Versioning is an ordinary library policy
built from typed Metadata and a Selector.

## Declare and attach metadata

```luna
meta release {
    channel: string;
    major: i32;
    minor: i32;
    patch: i32;
}

@release("stable", 1, 2, 0)
fn parse() -> i32 { return 120; }
```

`release(...)` is a typed metadata value. A plain attachment is available
during compilation and is removed from the runtime artifact. Metadata does not
change a function's callable type, but it gives same-named declarations stable
candidate identities.

## Define a selector

The initial selector protocol uses compiler-owned `declaration_view` and
`declaration_ref` types. The first parameter is declared explicitly but
injected by the compiler at each selection site:

```luna
fn choose_release(view: declaration_view, channel: string,
                  major: i32, minor: i32, patch: i32) -> declaration_ref {
    return select_unique(view, release(channel, major, minor, patch));
}
```

`select_unique` validates that the result belongs to the injected declaration
family and that exactly one declaration matches. A missing or ambiguous result
is a compile error for static selection.

## Static selection

The formal syntax and its prefix sugar are equivalent:

```luna
let f = select parse with choose_release("stable", 1, 2, 0);
let g = @choose_release("stable", 1, 2, 0) parse;

f();
@choose_release("stable", 1, 2, 0) parse();
```

Static selection is completed before MoonIR verification. MoonIR contains the
chosen declaration identity, so the operation has no runtime selection cost.
An unqualified call to a family with more than one candidate is rejected; the
compiler never chooses a declaration by source order or an implicit latest
rule.

## Runtime visibility and dynamic selection

Prefix an attachment with `runtime` when a runtime operation must inspect it:

```luna
runtime@release("stable", 1, 2, 0)
fn parse() -> i32 { return 120; }

fn resolve(major: i32) -> i32 {
    let f = dynamic select parse with
        choose_release("stable", major, 2, 0);
    return f();
}
```

`runtime@...` also gives its declaration a minimal Runtime Descriptor.
`dynamic select` embeds a finite descriptor set, performs a runtime uniqueness
check, and binds one function pointer. Only the explicit dynamic operation pays
that dispatch cost. Compile-time-only metadata cannot be inspected by a
dynamic selector.

`dynamic` is a superset of `runtime` and reserves richer reflection/evolution
rights. Metadata remains read-only in the current runtime boundary; future
replacement and weaving must create a newly verified declaration version.

## Current boundary

Metadata attachments are accepted on functions, fragments, structs, enums,
traits, and implementations. The executable `select` expression currently
binds callable function families. Type/trait/fragment family selection will use
the same Selector component and declaration-view protocol rather than
reintroducing special version syntax.

See [`examples/versioning.luna`](../examples/versioning.luna) for static
selection and [`examples/dynamic_select.luna`](../examples/dynamic_select.luna)
for the explicit runtime path.
