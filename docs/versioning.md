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

The static selector protocol uses real compiler-built-in collection types:
`declaration_view<T>`, `declaration_ref<T>`, and `metadata_view<M>`. They are
ordinary typed values in a selector body, not placeholders recognized by an
AST pattern. The view parameter is explicit in the function declaration and
is supplied by the `select` expression, so the caller only writes policy
arguments:

```luna
fn choose_release(
    candidates: declaration_view<() -> i32>,
    channel: string,
    minimum_major: i32
) -> declaration_ref<() -> i32> {
    let best = declaration_at(candidates, 0);
    let best_major = -1;
    for candidate in candidates {
        for info in metadata::<release>(candidate) {
            if info.channel == channel && info.major >= minimum_major {
                if info.major > best_major {
                    best = candidate;
                    best_major = info.major;
                }
            }
        }
    }
    return best;
}
```

Selectors may use conditionals, loops, local bindings, assignment, metadata
field access, declaration reflection, and compile-time helper functions.
`select_unique(view, metadata_value)` remains an optional exact-match helper;
it is not a required syntax or a compiler-recognized selector body.

The built-in static API includes:

| Operation | Meaning |
| --- | --- |
| `for candidate in view` | Iterate every declaration in the finite candidate view |
| `declaration_count(view)` / `declaration_at(view, index)` | Count or index candidates |
| `metadata::<M>(candidate)` | Obtain an iterable `metadata_view<M>` |
| `declaration_has_metadata::<M>(candidate)` | Test whether schema `M` is attached |
| `declaration_id(candidate)` | Reflect the stable declaration identity |
| `declaration_signature(candidate)` | Reflect the stable callable type identity |

The compiler does not interpret the meaning of a metadata schema or impose
SemVer policy. After executing the selector it only verifies that the returned
`declaration_ref<T>` belongs to the supplied view. A missing return, an
out-of-view reference, or a non-evaluable static selector is a compile error.

## Static selection

The formal syntax and its prefix sugar are equivalent:

```luna
let f = select parse with choose_release("stable", 1);
let g = @choose_release("stable", 1) parse;

f();
@choose_release("stable", 1) parse();
```

Static selection is completed before MoonIR verification. MoonIR contains the
chosen declaration identity, so the operation has no runtime selection cost.
An unqualified call to a family with more than one candidate is rejected; the
compiler never chooses a declaration by source order or an implicit latest
rule. Metadata is useful for open declaration families, but it is not required:
a selector may use signature or declaration reflection, or any other
compile-time policy available to its body.

## Direct static declaration reflection

Selection is unnecessary when ordinary name and signature resolution already
identify one declaration:

```luna
let known = declaration_of::<(i32) -> i32>(parse);
print(declaration_id(known));
print(declaration_signature(known));
```

`declaration_of` produces a compile-time `declaration_ref<T>`. It is erased
after static reflection and does not create a runtime descriptor. If the name
and optional signature still identify multiple declarations, compilation
fails; `select` is the tool for that open-boundary case.

## Runtime visibility and host binding

Prefix an attachment with `runtime` when a runtime operation must inspect it:

```luna
runtime@release("stable", 1, 2, 0)
fn parse() -> i32 { return 120; }
```

`runtime@...` also gives its declaration a minimal Runtime Descriptor. No static
operation implicitly retains metadata, descriptors, selector code, or
reflection data; `runtime` must be written explicitly. Compile-time-only
metadata cannot be inspected through a Runtime descriptor. The former
`dynamic` retention and `dynamic select` exact-match protocol are rejected in
0.3; runtime switching belongs to typed EV004 host bindings.

## Current boundary

Metadata attachments are accepted on functions, fragments, structs, enums,
traits, and implementations. The executable `select` expression currently
binds callable function families. Type/trait/fragment family selection will use
the same Selector component and declaration-view protocol rather than
reintroducing special version syntax.

See [`examples/versioning.luna`](../examples/versioning.luna) for static
selection and [the host evolution API](evolution.md) for explicit runtime
binding and generation switching.
