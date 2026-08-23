# Interceptors, contexts, and slots

The fragment model distinguishes three behaviors: an `interceptor` continues
automatically after normal completion, a `context` controls its continuation
through `resume()`, and `abort()` explicitly skips the continuation.
`return` ends only the current fragment. Slots record parameters, emission
cardinality, and ownership effects in their contracts; static paths are kept
inline whenever possible, while dynamic paths retain dispatch cost.

Luna does not infer a fragment's control behavior from how many times
`resume()` happens to occur. The declaration and the slot both carry an
explicit contract.

## Interceptors

An interceptor performs straight-line work and forwards the continuation once
when it completes normally. It cannot call `resume()` and requires no retained
continuation frame:

```luna
interceptor audit(value: i32) {
    print(value);
}

slot interceptor pipeline(value: i32) default audit;
pipeline(7) { print(8); }
```

`abort()` is the only way for an interceptor to discard the continuation.

## Contexts

A context explicitly controls a retained continuation. `resume()` enters the
slot continuation and returns to the next statement in the context after that
continuation completes. `abort()` explicitly discards the slot continuation.
If a context path reaches its end without `resume()`, it is an implicit abort;
the compiler does not require boilerplate `abort()`:

```luna
context profile {
    let start = monotonic_now();
    resume();
    print(monotonic_now() - start);
}

apply work(profile) {
    slot context work { perform_work(); }
}
```

`return` ends the current fragment immediately. It is distinct from
`abort()`: abort is the explicit decision not to enter the slot continuation,
while return is the fragment's own termination path. Code after `resume()`
executes only after the continuation completes normally.
The lowering creates a stack-resident continuation frame with a normal path and
a function-return path. A `return` inside the slot continuation stores its
result in that frame and bypasses the context's post-resume code, so it cannot
silently run cleanup or effects that follow the slot.

Multi-shot behavior is explicit:

```luna
context many replay {
    resume();
    resume();
}
```

`context many` is rejected when replay would consume, free, mutably borrow, or
otherwise invalidate captured linear state. Dynamic runtime selection supports
the same rule over a finite set of statically linked candidates.

## Slot contracts

Every slot declares which category it accepts:

```luna
slot interceptor observe(value: i32);
slot context transaction(value: i32);
slot context many replayable(value: i32);
dynamic slot context runtime_hook(value: i32);
```

Binding an interceptor to a context slot, or the reverse, is a type error.
`interceptor(...)` and `context(...; once|many)` are distinct structural types.
The legacy `fragment` declaration and an unqualified `slot name` are rejected
with migration diagnostics.

## Ownership effects

The compiler checks every normal-resume and abort exit at the slot boundary.
All exits that reach code after the slot must agree on linear ownership,
borrows, and device in-flight state. A fragment-local linear value must be
consumed before `abort()`.

`resume()` accepts no arguments. It restores the original captured frame, so a
context cannot forge replacement continuation inputs.

## Lowering and performance

Statically known interceptors are lowered as straight-line inlined code followed
by the continuation. Statically known contexts use a stack frame and explicit
continuation CFG blocks; there is no generic heap continuation object or
indirect call. LLVM can eliminate the frame's temporary state at `-O2`/`-O3`
when the continuation has no observable return transfer.

Dynamic apply performs one runtime candidate selection and branches among a
finite, linked, type-checked candidate set. Candidate bodies remain inline in
their branches. Unknown names abort with a diagnostic; arbitrary function
pointers are never accepted.

```luna
context trace(value: i32) { print(value); resume(); }
context audit(value: i32) { print(value + 2); resume(); }

dynamic slot context pipeline(value: i32);
dynamic apply pipeline(trace, audit) {
    pipeline(41) { print(42); }
}
```

## Runtime selection

Dynamic apply selects from a finite set of linked, type-checked candidates. The
first candidate is the deterministic fallback. A slot name maps to an
environment key:

```text
slot: pipeline       -> LUNA_FRAGMENT_PIPELINE
slot: request-log    -> LUNA_FRAGMENT_REQUEST_LOG
```

An unknown candidate reports an error instead of invoking an arbitrary pointer.
Selection occurs at each slot invocation, but every candidate must preserve the
same linear, borrow and device in-flight state. Dynamic slots remain host-only.
The environment is useful for tests and deployment configuration; it is not a
security boundary.

## External shared-library plugins

Alpha plugin ABI v1 supports host-only, single-shot interceptors with explicit
parameters:

```text
LUNA_FRAGMENT_PIPELINE=external_trace
LUNA_FRAGMENT_PLUGIN=/path/to/libtrace.so
luna run app.luna
```

Windows uses the corresponding `.dll` path. A plugin exports
`luna_fragment_plugin_descriptor_v1`; the runtime validates its plugin
id/version, slot name, ABI hash, parameter layout, once/many capability, effect
flags and entry point before registration.

The entry receives read-only pointers to explicit slot arguments and returns a
declared `continue` or `abort`. The host then executes the statically generated
continuation. Libraries remain loaded for the process lifetime, and duplicate
`(slot, fragment, contract)` registrations are rejected.

External `context`, `resume()`, `many`, lexical capture and retained argument
pointers are not allowed. They require a persistent continuation frame and a
proven lifetime model; exposing the current stack frame as a shared-library
callback would be unsound. Runtime ABI v1 defines host/module service tables,
but plugin v1 does not implicitly receive them. A future v2 must accept an
explicitly authorized module context.
