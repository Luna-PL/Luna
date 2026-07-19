# Interceptors, contexts, and slots

## 中文说明

片段机制明确区分三件事：`interceptor` 正常完成后自动继续，`context` 通过
`resume()` 控制续体，`abort()` 显式跳过续体，`return` 只结束当前片段。槽会把
参数、发射次数和所有权效果写进契约；静态路径尽量内联，动态路径才保留分派成本。

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
otherwise invalidate captured linear state. Dynamic runtime selection currently
accepts only single-shot declarations.

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

See [dynamic_plugins.md](dynamic_plugins.md) for the linked runtime selection
boundary and the Alpha v1 external shared-library ABI. External plugins are
currently limited to host-only, single-shot interceptors with explicit
parameters; static contexts retain the stronger stack-safe CPS implementation.
