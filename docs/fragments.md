# Slots and fragments

English | [简体中文](fragments.zh-CN.md)

Luna models an injection point as a nominal module-level `slot` and its handler
as a `fragment` that explicitly targets that slot. Declaring a slot is the
opt-in to controlled interception; a slot is not a function or transferable
value. A fragment is not interchangeable with a function reference because it
may receive the compiler-owned slot continuation through `resume`.

```luna
slot observed(value: i32);

fragment audit(value) for observed {
    print(value);
    resume;
}

fn main() -> i32 {
    apply audit {
        observed(41) {
            print(42);
        }
    }
    return 0;
}
```

Slots and fragments are module-level declarations. A fragment binds every slot
parameter and inherits the target's types and ownership contract. The target is
nominal: two same-shaped slots remain different injection points.

## Single-shot continuation

The first contract is unit-result and single-shot. `resume;` enters the next
selected fragment or the invocation's base continuation at most once. If the
continuation completes normally, execution returns to the statements after
`resume`. Natural fragment fallthrough or `return;` before `resume` discards the
unconsumed continuation.

```luna
fragment measured[label: i32](value) for observed {
    print(label);
    let start = monotonic_now();
    resume;
    print(monotonic_now() - start);
}
```

The continuation belongs to the invoking function:

- `return value;` in the continuation returns from that function and bypasses
  post-resume fragment code;
- `?` in the continuation has the same outer propagation behavior;
- the continuation cannot be stored, returned, forged, or resumed twice;
- fragment-local names are not visible in the continuation.

Cleanup obligations are explicit on canonical CFG edges. Fragment locals that
survive across `resume` are cleaned on normal completion and outer escape.

## Apply

`apply fragment[environment arguments] { ... }` constructs and installs a
fragment for a lexical region. Square brackets are the fragment construction
environment; parentheses remain exclusively the Slot invocation contract.
Environment arguments are evaluated once on entry to the apply region and are
reused by every matching Slot invocation. Fragment bodies do not implicitly
capture locals from the apply site. The Apply region owns Copy or affine
environment fields; every Fragment activation receives shared-borrow views.
An affine place therefore requires an explicit `move` at construction, while
an owning rvalue may be passed directly. Linear fields are rejected because a
reusable borrowed environment cannot prove exactly-once consumption. Field
ownership is inferred from its type, so ownership modifiers are not accepted
in the environment parameter list. A slot with no active binding runs its base
continuation directly.

```luna
apply measured[7] {
    observed(10) {
        perform_work();
    }
}
```

Static composition can be inlined and carries no Runtime descriptor or dispatch
cost. The confirmed runtime plan extends the same operand position to a verified
`RuntimeFragmentRef<S>` rather than adding `dynamic apply`.

## Public candidates

`export slot` publishes a stable injection contract. An `export fragment`
targeting an exported slot is a candidate implementation of that exact SlotId.
Candidate membership comes from the verified nominal relationship and
ContractId, never from user metadata.

Metadata attached through a public schema is selection policy for the host.
The typed candidate query returns immutable generation-pinned snapshots of
public executable Fragments for one exact SlotId/ContractId. The host selects
None or one candidate per exact Slot, constructs an immutable BindingSet, and
publishes it only at a Runtime safe point. Reflection, metadata filtering, and
descriptor validation do not run on ordinary Slot dispatch.

See the confirmed
[Slot/Fragment Runtime Injection Plan](slot_fragment_runtime_plan.md) for the
descriptor, lifetime, host-policy, and staged implementation boundaries.

## Implementation status

The unified `slot`, `fragment ... for`, and `resume;` syntax is connected to the
single-shot static Sema, MoonIR, JIT, and AOT path. Legacy categories, `many`,
`abort`, declaration defaults, and Slot/Fragment `runtime` modifiers are no
longer legal source constructs. Their semantic fields and lowering branches
have also been removed. The 0.3 container preserves the old wire offsets only
as reserved canonical values and rejects legacy values while decoding.

Runtime Fragment factory descriptors and `RuntimeFragmentRef<S>` are available
at the host ABI boundary. Source-level explicit Copy and affine environments
are lowered through Sema and MoonIR with cleanup on normal Apply exit, outer
`return`, and `?` propagation. Exported controls now have a retention-independent
`PUBLIC_CONTROL` descriptor capability; exact Fragment target/environment facts
survive verified container loading, and candidate snapshots filter public
executable bindings by exact Slot contract. Slot argument records and the
Runtime-owned opaque single-shot activation are also frozen. The initial public
execution ABI accepts Copy Slot contracts and Copy Fragment environments;
static apply continues to support affine environments. Exported Fragments now
reuse static CFG composition through a hidden container-reachable function;
LLVM emits verified factory/destroy/execute wrappers and publishes them as
executable candidates. Immutable BindingSet construction, safe-point atomic
activation, pinned snapshots, None/One dispatch, host-ordered chains, and
immutable local Slot overrides are implemented. The explicit execution-context
C ABI propagates continuation escape without global/TLS Runtime state, and the
artifact-cost gate covers exported executable materialization versus erased
static composition. An unbound exported Slot invocation is now preserved in the
sealed CFG as a verified `RuntimeSlot` terminator with an exact declaration
reference, frozen argument Record TypeId, packed operand, continuation entry,
and completion edge. Static `apply` composition remains ahead of this path, and
unbound private Slots remain erasable. Effect-directed context propagation,
including a verifier-recomputed least fixed point over exact direct calls, is
now represented and container-preserved as `requires_fragment_context`.
Affected internal LLVM functions now receive one hidden leading context
parameter and exact direct calls forward it; unaffected functions retain their
original ABI. A context-requiring `runtime fn` now publishes its ordinary
Function descriptor with the `FRAGMENT_CONTEXT` ABI flag, and its binding
preserves that flag for host-side typed lookup. This reuses the existing
runtime catalog instead of creating a second entry registry. An ordinary
`export fn` still promises its declared public ABI and is therefore rejected
when it requires the hidden capability; context-requiring function values also
remain rejected until the indirect-call ABI exists. Continuation
dispatch now outlines synchronous captures into an explicit stack frame,
packs the frozen argument record, calls the stable Runtime ABI, and writes
capture mutations back before following the completion edge. A cleanup-free
or resource-bearing enclosing `return`/`?` is carried through the frame and the
distinct escaped status. Canonical cleanup edges, Result switches, and case
bindings execute inside the callback before escape; selected generated
Fragments have been exercised through `resume` against that callback. A
continuation can call another function that reaches a dynamic Slot; its explicit
context is forwarded. Lexically nested Slots now recursively outline their
callbacks, propagate the same context and return storage, and write back
transitive captures, including affine resources. An inner escape performs the
sealed cleanup of enclosing resources before propagating its return; a selected
Fragment that omits `resume` instead reaches the post-Slot cleanup path. A
structural IR gate counts dynamic dispatch and checks static composition for erasure.
