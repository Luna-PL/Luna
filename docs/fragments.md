# Interceptors, contexts, and slots

Luna 0.3 models a control hook as a nominal module-level `slot` and its
implementation as an `interceptor` or `context` that explicitly targets that
slot. A slot invocation always carries a lexical continuation body, and an
`apply` always carries a lexical body.

```luna
slot interceptor observed(value: i32) default audit;

interceptor audit(value: i32) for observed {
    print(value);
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

Slots and fragments are declared at module scope. A fragment must bind every
slot parameter with the same type, ownership relation, and usage. Its control
form must also match the target slot. The target gives both declarations a
nominal contract identity; two same-shaped slots are still different types.
Declaration order does not affect a `default` binding.

## Single-shot control contract

Luna 0.3 freezes unit-result, single-shot control. `context many`, dynamic
slot/apply syntax, local slot declarations, and blockless apply are rejected
with migration diagnostics.

| Operation | `interceptor` | `context` |
| --- | --- | --- |
| Natural fallthrough | Enters the continuation exactly once | Discards an unconsumed continuation; after `resume()`, completes the fragment |
| `resume()` | Rejected | Enters the continuation once, then returns to the next fragment statement if it completes normally |
| `return;` | Ends the fragment and skips the continuation | Ends the fragment and skips an unconsumed continuation; after `resume()`, ends post-resume fragment execution |
| `abort()` | Explicitly discards the continuation | Explicitly discards an unconsumed continuation |

Fragment return is unit-only: `return value;` is rejected. `abort()` after a
single-shot context has resumed is also rejected because the continuation has
already been consumed. A context path may omit `resume()`; reaching its end is
an implicit discard rather than a missing-control error.

`return;` and `abort()` can have the same destination before a continuation is
entered, but remain different canonical operations. Return is normal
fragment-local termination. Abort records an explicit continuation-discard
decision and is invalid once a single-shot continuation has been consumed.

## Continuation boundary

The lexical body belongs to the invoking function, not to the fragment:

- `return value;` in the continuation returns from the enclosing function and
  bypasses context code after `resume()`.
- `?` in the continuation propagates from the enclosing `Result` function and
  has the same bypass behavior.
- `?` inside a fragment is rejected because it would otherwise propagate
  across the slot boundary implicitly; handle the `Result` explicitly there.
- Fragment-local names are not visible in the continuation, even when a local
  shadows a name in the invoking scope.

These rules avoid a second hidden meaning for `return` or `?`.

## Ownership and cleanup

Every edge leaving a fragment carries explicit cleanup obligations:

- interceptor locals are cleaned before natural forwarding, `return;`, or
  `abort()` reaches its target;
- context locals live across `resume()` and are cleaned when the context exits,
  including an enclosing-function return or `?` from the continuation;
- continuation locals are cleaned on their ordinary function/block exit;
- fragment-local exits do not consume enclosing resources implicitly; all
  paths that reach code after the slot must agree on ownership, borrow, and
  in-flight device state;
- a still-valid linear fragment local must be consumed before a fragment exit.

Cleanup order is represented on canonical CFG edges and verified before code
generation. Static composition therefore needs neither a heap continuation nor
runtime dispatch.

## Apply and defaults

`apply fragment { ... }` derives its target slot from the fragment declaration.
Within the lexical body, matching slot invocations use that fragment. Outside
the body, a slot uses its declared default; a slot with no active binding and no
default behaves as the identity operation and runs its continuation directly.

```luna
slot context measured();

context profile for measured {
    let start = monotonic_now();
    resume();
    print(monotonic_now() - start);
}

fn run() -> unit {
    apply profile {
        measured() {
            perform_work();
        }
    }
}
```

Qualified exported slot and fragment names follow ordinary package/module name
resolution. `symbols(slot_name)` may inspect a slot declaration, but a slot is
reflectable and non-callable: the query result is declaration metadata, not a
function value.

## Runtime boundary

Runtime-retained Slot and Fragment descriptors have stable declaration kinds,
nominal IDs, contract IDs, and strong fragment-to-slot references. This freezes
the representation needed by loaders and tooling without inventing a second
source language. These rows are currently non-callable descriptor/identity
evidence and contain no runtime continuation entry. `TBD-SF009` decides whether
0.3 freezes that promise and the exported/private slot-retention rule.

Luna 0.3 does not expose runtime typed-reference lookup/acquisition syntax.
Ordinary `apply fragment { ... }` is the sole apply spelling; extending its
fragment operand to an acquired typed reference depends on `TBD-SF007`.
Acquisition, ownership, lifetime, and runtime-apply ABI rules belong to
`TBD-SF010`.
The removed `dynamic slot` and `dynamic apply` spellings remain only in the
migration-error corpus. Their former external plugin ABI and environment-driven
dispatch runtime were deleted rather than carried into 0.3.
