# Iteration, Pipelines, and Container Boundaries

Luna currently targets dynamic extensibility, limited control-flow enhancement, and systems
performance. Iteration therefore uses an ordinary lazy, fusible control-flow model. It does
not introduce full algebraic effects or an independent effect summary; the compiler derives
implementation and safety facts into read-only `sysmeta`.

## Arrays and borrowed slices

`array<T, N>` is a fixed-length, inline-storage value type, with `N` a non-negative
compile-time integer. Array literals infer element type and length, and all elements must
share one type:

```luna
let values: array<i32, 4> = [10, 20, 30, 40];
values[1] = values[0] + 5;
```

Indexing accepts integers only. Constant out-of-bounds access is rejected at compile time;
dynamic out-of-bounds access is checked at runtime and terminates rather than becoming
undefined access.

`values[start..end]` creates a read-only `slice<T>`, equivalent to
`slice(borrow values, start, end)`. A slice is a non-owning `{data, length}` borrowed
view over a half-open range, requiring `start <= end <= length`. The source array cannot
be written while the slice lives, and slice indexing also performs dynamic bounds checks.
The stable surface has no `slice_mut<T>`; `raw<T>` and `device_buffer<T>` do not
implicitly become arrays or slices.

## Current surface

Arrays provide all three ownership modes:

```luna
values.iter()       // Iterator<&T>
values.iter_mut()   // Iterator<&mut T>
values.into_iter()  // Iterator<T>, transfers elements by value
```

Read-only slices provide only `view.iter() -> Iterator<&T>` and direct shared iteration.
They cannot create mutable references or transfer ownership of elements; those operations require
an owning array receiver.

Integer half-open ranges use `range(start, end)`. Lazy adapters include:

```luna
iterator.map(fn(T) -> U)
iterator.filter(fn(T) -> bool)
iterator.take(count)
```

Terminal operations include:

```luna
iterator.fold(initial, fn(Acc, T) -> Acc)
iterator.for_each(fn(T) -> unit)
iterator.count()
iterator.collect::<Target>()
```

`for item in iterator { ... }` is also terminal. Direct array iteration yields values;
direct slice iteration yields shared references. `range` is half-open, so its upper bound is
excluded.

A user type can implement the Core protocol and enter `for` directly:

```luna
impl core::iter::Iterator<i32> for Counter {
    fn next(iterator: &mut Counter)
        -> core::option::Option<i32> {
        // Return Some(item) or None
    }
}

let iterator = new Counter(...);
for item in iterator { ... }
```

Ordinary member calls use static trait dispatch, so `iterator.next()` and
`(move collection).into_iter()` resolve to the unique concrete impl symbol without a
vtable. When a collection does not implement Core `Iterator`, `for item in collection`
statically finds the unique Core `IntoIterator` impl, consumes the local collection, and
calls `into_iter` once. The compiler owns the hidden iterator state created by the
conversion; it performs the corresponding cleanup on normal `None` or function early
return. The protocol source must currently be a local binding.

`collect::<Target>()` statically selects the target type's unique Core
`FromIterator<Item, Builder>` impl:

```luna
impl core::iter::FromIterator<i32, SumBuilder> for CollectedSum {
    fn begin() -> affine SumBuilder {
        return new SumBuilder(0);
    }

    fn push(builder: &mut SumBuilder, affine item: i32) -> unit {
        builder.total += item;
    }

    fn finish(affine builder: SumBuilder) -> affine CollectedSum {
        let total = builder.total;
        return new CollectedSum(total);
    }
}
```

Before fusing the loop, the compiler calls `begin` once, `push` once per element that
passes the adapters, and `finish` once at the end. The target type must be explicit with
turbofish syntax. Sema checks exact Core trait identity, Item consistency, the complete
method set, and the affine ownership contract; no vtable is used.

## Implementation model

An adapter chain forms a temporary compiler-domain iterator recipe in the frontend. During
canonical CFG construction, a `for` consumer expands its verified source, adapter order,
input/output element types, and borrow mode into one ordinary loop. It creates no intermediate
array for `map` or `filter`, does not depend on `Vec`, and does not call an iterator Runtime ABI.

In canonical CFG construction an array contributes a compile-time constant bound, while a slice
contributes one verified `usize` `SliceLengthExpr` in loop initialization. This node is a basic
slice projection rather than an iterator operation; the source and its runtime bound are each
evaluated once.

A capture-free recipe may currently be bound to a local. At binding time it evaluates the
source, lambda function pointer, and `take` argument in source order, then stores source
pointers/Copy snapshots, indices, bounds, and adapter state on the stack. A binding may be
consumed by `for`, `fold`, `for_each`, `count`, or `collect`, and adapters may be
appended in the consuming expression; the whole chain still expands statically to one loop,
with no heap allocation, vtable, or iterator Runtime ABI.

For canonical `for` construction, the materialized Iterator binding is erased at that binding
point into ordinary source/index/limit/adapter locals. The index is also the affine
single-consumption witness and is moved into the eventual loop; this adds no separate runtime
token. Materialized terminal expressions remain on the legacy structured path until their
control-flow-expression canonicalization subphase.

This binding is an affine, single-consumption local and cannot currently be returned, passed,
or sent across an ABI. Borrow bindings keep the source loan until lexical scope end; a Copy
`into_iter` binding takes a value snapshot at binding time. A move-only `into_iter` binding
moves source ownership into hidden stack state and records initialization bits for each array
element. Terminal consumption, early return from `for` or an ordinary function, and leaving
scope without consumption clean only elements that remain initialized.

Source expressions, adapter arguments, and terminal arguments are evaluated left to right.
`filter` skips elements and `take` evaluates only elements that pass through it, so
adapter order has ordinary lazy-pipeline meaning.

## Ownership

- `iter()` holds a shared borrow during consumption.
- `iter_mut()` holds an exclusive mutable borrow for the complete `for` loop.
- A user Core Iterator state holds an exclusive mutable borrow for the complete loop;
  `next` advances it only through the resolved `&mut Self` protocol entry.
- A user protocol may have move-only `Item`. The `Some` tag is the initialization state
  for that iteration, and the item binding is reinitialized on each successful iteration.
  Normal loop fallthrough drops it once; early returns use path-sensitive cleanup once, and a
  moved-out item is not cleaned again.
- Arrays recursively inherit element Copy/affine/linear and Drop properties. Constructing a
  move-only array requires explicit `move` for each element. Direct array consumption or
  `into_iter()` records ownership in a hidden array snapshot and per-element initialization
  bits. Delivered slots are not cleaned by the array; the tail left by `take`, and
  undelivered slots on early return, are cleaned exactly once. Hidden linear state remains
  rejected.
- `filter` accepts move-only elements, but its predicate must borrow each element; rejected
  elements are cleaned immediately. `take` accepts move-only elements; at its limit it
  cleans the current element and then cleans unread source slots when the loop exits.
- Lambda bodies use the same path-sensitive ownership checks as ordinary functions. An
  owning affine parameter is cleaned on fallthrough and every return path, or transferred
  under a `-> affine T` return contract; an unconsumed linear parameter is rejected.
- `map` may consume move-only input, but its transform parameter must be explicitly owning.
  It may produce Copy or move-only output. Output ownership is explicit when `filter`/`take`
  rejects it, `for` receives it, or another owning adapter consumes it.
- `for_each` actions and `fold` reducers may receive move-only items, but their parameters
  must be explicitly owning. `count` directly cleans move-only items after counting.
- `fold` supports an affine move-only accumulator: the local initial value must be explicitly
  moved, the reducer owns the old accumulator and returns replacement ownership. The compiler
  clears the accumulator initialization bit before each call, sets it again after writing the
  replacement, then transfers the final result and clears state. Ignoring a move-only fold
  result is rejected. Linear accumulators are not hidden in this state yet.
- `fold`/`for_each`/`count` consume a move-only local array source with one hidden recipe
  state in MoonIR, cleaning undelivered slots before producing a result. Reusing the source
  afterward is use-after-move; linear sources cannot be hidden in this state.
- `collect` uses the same terminal recipe/drop state. Each pipeline item transfers ownership
  to `FromIterator::push`; `take`, `filter`, and the remaining source tail each clean
  exactly once. The builder exists only in a hidden stack slot in the current function;
  `finish` consumes it and gives an affine result to the caller. Ignoring that result is
  rejected by ownership checking.
- Adapter lambdas must currently be capture-free; references to outer locals receive a clear
  diagnostic until closure-environment layout is complete. Pipelines are not open in device
  kernels.
- A materialized recipe is an affine, single-consumption value; a second `for` or terminal
  call is use-after-move. An `iter_mut()` binding retains an exclusive source loan for its
  lexical lifetime, while a Copy `into_iter()` binding is independent of later source
  mutation. Creating a move-only array binding moves the original source; hidden
  `[N x i1]` Drop state clears delivered bits and cleans the tail on consumption or any scope
  exit.

These constraints are staged safety boundaries, not the final Iterator abstraction.

## Is a container a builtin?

The iteration protocol does not require dynamic containers to become builtins. Fixed arrays
and borrowed slices remain language fundamentals because length, layout, bounds checks, and
borrow relations participate directly in type checking and code generation. Future `Vec<T>`,
lists, hash maps, and user containers should live in Core/standard library and connect through
stable `IntoIterator`/`Iterator` traits; the compiler should retain only necessary builtin
entry points and optimization recognition.

The following stable Core declarations are materialized:

- `org.luna.core::option::Option`
- `org.luna.core::iter::Iterator`
- `org.luna.core::iter::IntoIterator`
- `org.luna.core::iter::FromIterator`
- `org.luna.core::iter::{Map, Filter, Take}`

The current compiler recipe does not repeatedly call `next() -> Option<T>` at runtime,
because a statically known chain can be fused directly. Local materialization stores only
the stack state needed for static expansion; it does not rewrite itself into Core
`Map`/`Filter`/`Take` enums or trait calls. Core
`Iterator::next(&mut Self) -> Option<Item>` still defines the user protocol; cross-function
and cross-package adapter values should eventually use these stable Core types.

The user-protocol path is connected: Sema recognizes only the stable package identity
`org.luna.core::iter::Iterator), validates `next(&mut Self) -> Option<Item>`, and writes
the unique static `next` symbol, iterator type, Option type, and variant index to MoonIR.
LLVM calls `next` once per iteration and branches on `None`/`Some`. A same-shaped trait
with a method also named `next` is not mistaken for the iteration protocol.

This path intentionally coexists with compiler recipes: known arrays, slices, and ranges keep
fusing, while ordinary user state machines use protocol calls. Core `IntoIterator` can
produce an iterator through static member calls, and `for` inserts the unique static
conversion when the source does not directly implement Core `Iterator`. Sema writes the
conversion symbol, input type, hidden state name, and cleanup facts to MoonIR; LLVM does not
repeat trait lookup. Core `FromIterator` is also connected to compiler recipes through the
`begin/push/finish` builder protocol, so the recipe need not become a cross-ABI `Iter`.
MoonIR stores target and builder types and the three unique impl symbols; LLVM still emits one
loop with no intermediate container.

Current `FromIterator` impls must be concrete; generic impls await coherence support for
impl specialization. Item, builder, and target may not currently be linear, and `collect`
consumes only a local compiler recipe. Dynamic containers such as `Vec<T>` should be
provided by the standard library with their own builder and storage strategy, rather than
becoming builtins.

## Relation to effects and sysmeta

An iteration pipeline is ordinary, finite, structured control flow, not a continuation
handler, and has no resume or multi-resume semantics. It should not be modeled as an
algebraic effect. The current host-only recipe is compiler-derived `sysmeta` fact. If device
lowering, suspendable iterators, or coroutine adapters are opened later, the compiler should
continue deriving `maySuspend`, storage, and ABI facts rather than asking users to maintain
a parallel effect summary.

## Evolution order

1. Map the current compiler-internal materialized Drop state to a stable Core inline-adapter
   layout before allowing adapters as ordinary values across function or package boundaries.
2. Complete ownership, borrow, and Drop layout for closure environments before allowing
   capturing `map`/`filter`.
3. Open generic `FromIterator` impls after coherence supports impl specialization, with
   standard-library dynamic containers providing actual builders.
4. Only then connect asynchronous iteration to stackless coroutines, with `sysmeta`
  deriving suspension and frame requirements.
  deriving suspension and frame requirements.
