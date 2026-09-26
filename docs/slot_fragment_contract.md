# Slot/Fragment bounded contract evidence

[English](slot_fragment_contract.md) | [简体中文](slot_fragment_contract.zh-CN.md)

> Status: implemented bounded behavior and regression evidence, 2026-09-26.
> This records the SFR001/v1 implementation; it is not stable-release approval.

## SF008 bounded profile (2026-09-26)

The following rules distinguish static construction, runtime selection, and
continuation execution. They do not add syntax, change the v1 ABI, or authorize
arbitrary handler-body re-entry.

| Boundary | Implemented rule | Regression gate |
| --- | --- | --- |
| Static nested `apply` | The innermost lexical binding wins; leaving it restores the outer binding. It does not implicitly build a runtime chain. | `luna.semantic-regressions` |
| Static body expansion | Direct and mutual Fragment expansion cycles fail closed in Sema and the CFG builder, including forged structured MoonIR. | `luna.semantic-regressions`, `luna.moonir-canonical` |
| Static same-Slot continuation nesting | Finite nesting is valid. Inner discard skips only its own continuation; an inner continuation return escapes the invoking function and skips both handlers' post-resume code. | `luna.semantic-regressions` |
| Private unbound Slot | Identity lowers to a lexical block, including inside a handler; it does not introduce a runtime context dependency. | `luna.semantic-regressions`, `luna.moonir-canonical` |
| Runtime ordered chain | The host's explicit order determines `resume` progression, followed by reverse-order post-resume execution. Catalog order and loading order do not choose the chain. | `luna.runtime-fragment-v1` |
| In-flight snapshot lifetime | Dispatch owns its selected snapshot through complete handler unwind. A synchronous native callback may clear or replace the published C++ handle without releasing the active One/chain environments or generation; the replacement affects future calls only. Owned and borrowed environments are released before their final module lease, including escape and failure paths. | `luna.runtime-fragment-v1` |
| Factory generation lifetime | Owned construction pins the validated generation before invoking the factory. Synchronous clearing/replacement of the source binding cannot release that generation during construction or rejected-output cleanup, nor redirect the new reference to a different binding. Success transfers the original pin to the reference; failure destroys any non-null output once before releasing the generation pin. | `luna.runtime-fragment-v1` |
| Single-shot failure propagation | A failed resume on a live activation remains failed. Ignoring a repeated-resume error cannot turn its enclosing One/chain dispatch into success; the continuation still runs at most once. Downstream failures propagate outward without overwriting their diagnostics, and a fresh invocation of the same context is unaffected. | `luna.runtime-fragment-v1` |
| Runtime same-Slot continuation nesting | Each dispatch creates fresh single-shot activations, even with the same pinned context and selected chain. A propagated inner continuation escape bypasses post-resume code in the suspended outer chain. | `luna.runtime-fragment-v1` |
| Nested local override / None | An explicit inner context replaces or removes the selected Slot chain only for that context. It does not modify the suspended outer chain; None still propagates continuation escape. | `luna.runtime-fragment-v1` |
| Runtime payload storage | Actual Slot argument addresses must satisfy their declared alignment. Empty carriers use size 0, alignment 1, and null data. None and One reject malformed carriers before callbacks; borrowed and factory-returned environments must also be aligned. Rejected non-null factory output is destroyed once. | `luna.runtime-fragment-v1` |
| Runtime identity representation | C++ module, symbol, contract, Slot, and argument-layout IDs cannot contain embedded NUL. Generation staging rejects them before initialization; discovery, activation, None/One dispatch, and local override reject ambiguous keys rather than silently truncating them into a different C ABI identity. | `luna.moon-runtime`, `luna.runtime-fragment-v1` |
| Published handler ownership | Exported bodies receive independent ownership checking without local `apply`: local linear state must be consumed and conflicting loans or repeated frees are rejected. Implicit affine cleanup remains valid under repeated static application, continuation escape, and host-only runtime dispatch. | `luna.analysis-snapshot`, `luna.semantic-regressions`, `luna.moonir-canonical` |
| Published handler capability | Exported bodies are analyzed without local use. A generated Fragment runtime entry that directly or transitively requires an execution context is rejected using independently recomputed effects, even if its summary is forged. | `luna.semantic-regressions`, `luna.moonir-canonical` |
| Static/runtime cost boundary | Statically bound composition introduces no runtime dispatch or candidate discovery. Unbound exported Slots use explicit context-directed dispatch. | `luna.moon-cost-boundaries`, `luna.moonir-canonical` |

An escape is relative to the function owning the continuation or its explicit
callback control outcome. A normal return from an ordinary called function
does not automatically escape the caller's continuation or outer chain.

Static source evidence includes
[finite nesting](../tests/fixtures/fragment_nested_continuation.luna),
[nested discard and return](../tests/fixtures/fragment_nested_discard.luna),
[static handler publication](../tests/fixtures/exported_fragment_static_body.luna),
[published handler cleanup](../tests/fixtures/exported_fragment_ownership.luna),
and [private context inheritance](../tests/fixtures/fragment_static_dynamic_body.luna).
The independent CFG/effect checks live in
[canonical lowering tests](../tests/moonir_canonical_sealing_lowering_test.cpp);
the explicit context/chain/override checks live in
[runtime Fragment tests](../tests/runtime_fragment_test.cpp).

## Remaining decision and release boundary

`TBD-SF008` now has an explicit bounded implementation profile, not an unspecified
static recursion behavior. It remains open for stable-language acceptance and
any extension permitting published handlers to acquire an execution context and
dispatch dynamically from their own bodies. The current v1 execute wrapper
receives an environment plus opaque activation, not that capability. A future
extension must decide context propagation, re-entry semantics, and ABI
compatibility before implementation; removing the rejection alone is unsafe.

Host-authored native callbacks are not Luna source verified by this profile.
Storage alignment checks do not prove that arbitrary native pointers refer to
valid allocations of the claimed size or lifetime; the host and factory remain
responsible for those obligations.
The context must be live when entering the C ABI. In-flight retention does not
authorize another call through a released opaque pointer, or concurrent mutation
of the same C++ handle without synchronization.
Failure propagation does not roll back already-performed effects or sandbox
native handlers; it prevents a failed activation from being reported as a
successful dispatch when the handler returns.
Their ability to invoke the C dispatch ABI is not a stable guarantee of arbitrary
Luna handler-body recursion. No implicit TLS/current Runtime, automatic
same-Slot suppression, runtime recursion limit, or multi-shot continuation
policy is introduced.

Context-aware indirect calls and non-Copy exported contracts remain outside the
bounded first ABI. This profile does not enlarge the 2026-09-15 core freeze or
close the separate performance, stability, and release-authorization gates.
See the [runtime plan](slot_fragment_runtime_plan.md) and
[release register](ecosystem_release.md).

## Reproduce the bounded gates

Build the compiler and test targets first, then run:

```sh
ctest --test-dir build --output-on-failure -R 'luna\.(analysis-snapshot|semantic-regressions|moon-runtime|runtime-fragment-v1|moonir-canonical|moon-cost-boundaries|0\.3-design-contract)$'
```

The seven gates are behavioral/structural checks, not benchmark timing thresholds.
A stable-release claim still needs the full regression suite and corresponding
cross-platform evidence for the exact proposed release commit.
