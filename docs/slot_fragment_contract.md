# Slot/Fragment bounded contract evidence

[English](slot_fragment_contract.md) | [简体中文](slot_fragment_contract.zh-CN.md)

> Status: implemented bounded behavior and regression evidence; v1 acceptance snapshot updated 2026-09-28.
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
| Concurrent host publication/dispatch | Four native readers suspend live continuations across explicit None/One/chain publications; old dispatches retain their selection on completion, escape, and failure. Reader-owned copies of one context keep dispatching after Runtime destruction, with independent activations and ordered final cleanup. The implementation and harness are instrumented by the Linux TSan gate. | `luna.runtime-fragment-concurrency` |
| In-flight snapshot lifetime | Dispatch owns its selected snapshot through complete handler unwind. A synchronous native callback may clear or replace the published C++ handle without releasing the active One/chain environments or generation; the replacement affects future calls only. Owned and borrowed environments are released before their final module lease, including escape and failure paths. | `luna.runtime-fragment-v1` |
| Factory generation lifetime | Owned construction pins the validated generation before invoking the factory. Synchronous clearing/replacement of the source binding cannot release that generation during construction or rejected-output cleanup, nor redirect the new reference to a different binding. Success transfers the original pin to the reference; failure destroys any non-null output once before releasing the generation pin. | `luna.runtime-fragment-v1` |
| Reference cleanup callbacks | `reset` detaches retired state before owned destroy or borrowed-lease release and retains its generation until those operations complete. A nested reset is empty; synchronous rebind of a live reference survives. Move assignment installs incoming state before old cleanup, so callback changes are not overwritten. Owned environments are destroyed once; the reference's borrowed lease is released before its module pin. | `luna.runtime-fragment-v1` |
| Single-shot failure propagation | A failed resume on a live activation remains failed. Ignoring a repeated-resume error cannot turn its enclosing One/chain dispatch into success; the continuation still runs at most once. Downstream failures propagate outward without overwriting their diagnostics, and a fresh invocation of the same context is unaffected. | `luna.runtime-fragment-v1` |
| Runtime same-Slot continuation nesting | Each dispatch creates fresh single-shot activations, even with the same pinned context and selected chain. A propagated inner continuation escape bypasses post-resume code in the suspended outer chain. | `luna.runtime-fragment-v1` |
| Verified container, local Slot publication | Decoding retains canonical Slot/contract and export rows, not frontend Slot objects. A local exact Slot export remains dispatchable through the verified generation adapter; missing/wrong-kind/wrong-contract exports and foreign import/re-export attempts are rejected. Generated Copy environment factories, None/One, resume/discard, captured writeback and continuation return escape remain executable after Runtime teardown. | `luna.moonir-canonical` |
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
The [concurrent host harness](../tests/runtime_fragment_concurrency_test.cpp)
uses immutable native environments and private per-call arguments. Hosts remain
responsible for callback/environment synchronization; this is not source-language
thread-safety admission for arbitrary Fragment environments or Arc payloads.

## v1 acceptance snapshot (2026-09-28)

This audit compares sources, host headers, verified-container tests and implementation
records. Core completion means **bounded v1 driven by native host APIs**, not the
entire SFR001 source-language plan or stable acceptance. C++ `RuntimeFragmentRef`
is not templated on `S`: it checks exact Slot/Contract identities at construction
and installation. It does not imply an available transferable Luna
`RuntimeFragmentRef<S>` type. Source type/ownership validation and runtime nominal
checking at the host boundary are different guarantees.

Keys/states below are audit labels, not new keywords, public APIs, decision IDs
or release approval.

<!-- SLOT_FRAGMENT_V1_ACCEPTANCE_BEGIN -->

| Boundary | State | Evidence / actual remaining work |
| --- | --- | --- |
| `host-ref` | `implemented` | C++ move-only refs, owned/borrowed environments, factories/cleanup, generation pins and validated native owning-carrier transfer; `luna.runtime-fragment-v1`. |
| `source-ref-apply` | `implementation-open` | Native singleton handle/check/Drop/transfer, source `RuntimeFragmentRef<S>` spelling/nominal checks, LLVM Ref Drop/local transfer and frontend exact-Slot Ref-apply recognition/lexical borrow checks are implemented. Disposable CodeGenerator modules pair actual unit and direct affine-return bodies with private status-returning host carrier wrappers; the latter checks an empty output, transfers exact-target ownership twice and cleans failed return transfer. Source signature/import/parameter/return publication, a public host return ABI, complete compiler dropGlue, wire round-trip, executable Ref operand/context lowering and end-to-end gates remain unimplemented; internal Refs cannot yet be published; see implementation slices. |
| `candidate-snapshot` | `implemented` | `snapshotRuntimeFragmentCandidates(generation, slot, ...)` filters one explicitly supplied generation by exact Slot/Contract and pins an immutable snapshot; not an all-loaded-package global query. |
| `candidate-aggregation` | `host-managed` | Hosts know the packages they load and may compose per-generation candidates. Runtime has no built-in global candidate set/cross-generation aggregate query. A convenience API is a later scope choice, not a hot-path defect. |
| `candidate-notification` | `host-managed` | Load/activation results and generation identities let hosts observe changes; no built-in candidate event bus or automatic discovery, ordering, winner selection or injection. |
| `binding-dispatch` | `implemented` | Explicit None/One/ordered chains, safe points, pinned contexts and local overrides; in-flight handle release/replacement retains the original snapshot/environments. |
| `context-entry` | `implemented` | Explicit `runtime fn` host entries, direct-call effect fixed points, argument/continuation frames, cross-package verified containers and return/`?` escape; `luna.moonir-canonical`. |
| `handler-context-reentry` | `deferred` | v1 published execute wrappers lack context; direct/transitive dynamic dispatch from handler bodies is rejected. Native base-continuation nesting does not grant it; stable `TBD-SF008` acceptance stays open. |
| `context-indirect-call` | `deferred` | Context-dependent function values/indirect calls fail closed; no TLS or removal of rejection checks expands the ABI. |
| `noncopy-public-abi` | `deferred` | Exported Slot parameters/Fragment environments are Copy-only; static affine environments are not a cross-host move/drop protocol. |
| `multi-shot-nonunit` | `deferred` | v1 remains unit-result/single-shot, without escaping continuations, asynchronous activations or multiple resumes. |
| `runtime-cost-structure` | `implemented` | Static erasure, one dispatch per dynamic site and control-plane work outside dispatch; scoped activations/frozen identities have allocation/lifetime regressions, not allocation-free whole dispatch. |
| `performance-acceptance` | `acceptance-open` | Pinned-thread matched observations exist, not controlled alternating A/B, cross-platform budgets or formal approval. Tiny/short CI series prove behavior/protocol, not acceptance. |
| `durable-evidence` | `storage-open` | Local packages/14-day CI artifacts are not durable external storage. Choose destination, retention and identity anchors; longer CI retention alone is not permanent archival. |
| `stable-release` | `authorization-open` | Bounded behavior completion is separate from stable-language/release authorization; historical TBD keys, the 0.3 core freeze, tags/lock/release gates are unchanged. |

<!-- SLOT_FRAGMENT_V1_ACCEPTANCE_END -->

## ABI boundary ledger (2026-09-28)

This ledger distinguishes a versioned native interface from compiler-internal
proofs and an uncommitted source-language ABI. It does not expand the v1
acceptance snapshot or approve a stable release.

| Layer | Current boundary |
| --- | --- |
| Native Fragment ABI | Implemented v1 descriptor and dispatch C ABI (`LRF1`, version 1): exact Slot/Contract and argument/environment layouts, factory/destroy/execute callbacks, explicit dispatch context and single-shot continuation. Published execute callbacks have no context argument. |
| Native Ref bridge | Implemented v1 check/transfer/drop on validated owning handle cells. This is not arbitrary-pointer adoption or a Luna source import/return ABI. |
| Compiler-internal source Ref proofs | `RuntimeFragmentRef<S>` spelling and exact nominal Slot checking exist; disposable LLVM modules exercise private unit and affine-return carrier wrappers. Those wrappers are not public symbols or a published carrier layout. |
| Cross-package source Ref ABI | Uncommitted and blocked: source signatures/imports/returns, complete drop glue, Moon Container round-trip and Ref-operand `apply` have no publishable contract yet. |

`export` controls external visibility; `runtime` controls declaration retention or
a runtime metadata attachment. Neither is a Slot/Fragment-specific modifier:
`runtime slot` and `runtime fragment` are rejected. Exported public-control
descriptors do not imply runtime callable/executable capability, and metadata
retention does not confer that capability on its target. Context dependence is
inferred and verified, not granted by `export` or by metadata.

The source Ref design already requires exact nominal Slot and sealed Contract
identities, an affine owner versus a borrowed parameter, explicit generation
pinning, and host-selected binding. The public ABI still needs a versioned
entry/carrier and status contract, ownership commit and failure cleanup rules,
borrow lifetime, cross-package proof/wire/drop-glue rules, and Ref-operand
`apply` lowering. Internal Ref representation is not a wire format. These
open choices do not imply new keywords, a global catalog, or handler re-entry.

Source pointers: [host APIs](../src/runtime/RuntimeFragment.h),
[per-generation discovery](../src/runtime/RuntimeFragment.cpp),
[static apply parser](../src/parser/ParserStatements.cpp) and
[real cross-package container loading](../tests/moonir_canonical_runtime_slot_container_test.cpp).
The frozen-identity measurement anchor is `bc9d6fd`, with that stage's
77/77 non-hardware regressions and successful three-platform CI; it is also the
observation build. Report commit `7757b77` is tracked separately. Neither that
report nor this audit commit is the measurement build. Full observations/digests
are in the [implementation plan](slot_fragment_runtime_plan.md#matched-protocol-frozen-identity-observations-2026-09-28).

The audit stage's existing strict-warning build passes the eight bounded gates plus documentation
inventory, 9/9 (final run: 14.68 seconds); this is not a new full compiler build or a 77-test
rerun. The new status gate first fails without the acceptance snapshot and then
passes with it; in-memory negative fixtures also reject premature completion,
missing/duplicate boundaries and unregistered entries.
Read-only `verify_release_readiness.cmake` currently blocks publication because the
lock's verified Luna candidate `41ce85ec9d6d2c2f22b193b3ece60abb09d4c5c3` is not an
ancestor of HEAD. Exit 0 means the fail-closed policy works, not release-ready.
This is separate ecosystem evidence/promotion work, not a Slot execution defect;
do not automatically replace the candidate, edit the lock or move historical tags
to bypass it.
The local repository is not shallow, the candidate commit object exists, and a
direct `git merge-base --is-ancestor` returns 1; this is not merely missing shallow
history or an unavailable object.

Source Ref/runtime apply was selected on 2026-09-28, ahead of a third micro-optimization:

1. For native-host v1 acceptance, decide practical performance budgets, stable
   commitments and evidence-storage policy. No global catalog, candidate events
   or handler re-entry is required merely for this acceptance path.
2. `source-ref-apply` is actual missing work. The [source implementation slices](slot_fragment_runtime_plan.md#source-refapply-implementation-slices-2026-09-28) start with a frozen singleton and explicit parent for host ingress, nominal types and local apply. Source `.bind`, loading policy and handler re-entry are not part of this first increment.

`tests/luna_0_3_design_contract.cmake` protects both languages' complete classification
against conflating source gaps, optional host facilities, deferred capabilities
and approval. It is a status-consistency gate, not source-behavior evidence or
owner approval for these capabilities.

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
Cleanup callbacks must not throw, destroy an object still in use, or resurrect an
object whose destructor is running. Live-reference cleanup re-entry is a native
host lifecycle rule, not authorization of arbitrary Luna handler-body re-entry.
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
Cross-package verified containers use the owner's own root `Exports` as the
persistent Slot publication fact. After fully decoding that owner artifact,
the compiler issues immutable `SlotPublicationEvidence`; the host explicitly
supplies it when decoding or staging a consumer. Owner, direct dependency,
target/layout, exact SlotId/ContractId, structural type and argument layout must
agree. Missing evidence remains fail-closed, including load-once cache hits.
Consumers do not re-export dependency Slots. This introduces no new keyword,
container field, ContractId encoding, Runtime ABI or dispatch-time lookup.
Artifact verification is structural/integrity verification, not publisher
authentication or a native-code sandbox; artifact trust and Fragment selection
remain host responsibilities. The compiler adapter does not discover artifacts
on disk, recursively load dependencies, select candidates or activate bindings.
Formal `build -t moon` encoding self-verifies internally against the verified
source projection; it exposes no dependency evidence for consumer loading.
See the [runtime plan](slot_fragment_runtime_plan.md) and
[release register](ecosystem_release.md).

## Reproduce the bounded gates

Build the compiler and test targets first, then run:

```sh
ctest --test-dir build --output-on-failure -R 'luna\.(analysis-snapshot|semantic-regressions|moon-runtime|runtime-fragment-v1|runtime-fragment-concurrency|moonir-canonical|moon-cost-boundaries|0\.3-design-contract)$'
```

The eight gates are behavioral/structural checks, not benchmark timing thresholds.
A stable-release claim still needs the full regression suite and corresponding
cross-platform evidence for the exact proposed release commit.
