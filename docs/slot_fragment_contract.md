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
| `source-ref-apply` | `implementation-open` | Native singleton handle/check/Drop/transfer, source `RuntimeFragmentRef<S>` spelling/nominal checks, LLVM Ref Drop/local transfer and frontend exact-Slot Ref-apply recognition/lexical borrow checks are implemented. An internal canonical CFG record verifies the Ref owner, exact exported Slot and lexical Apply region; Slot sites feed the existing context-effect fixed point. A compiler-private native bridge derives and drops an owned local context from a borrowed Ref. A wire-neutral CFG plan verifies exact context entry and ordered exit obligations; a disposable LLVM module proves private derive/status/Drop transitions. A second disposable CodeGenerator lowers one top-level Ref-apply source body with an optional nested region borrowing the same or a second Ref for the same or a different exact Slot across normal and early-return exits. The private proof checks that derive, direct and outlined RuntimeSlot dispatches, and per-exit Drops follow the active context stack and that early-return Drop precedes outer Ref owner cleanup. A test-target-only JIT hook executes normal and early-exit source bodies, including sequential and outlined Slot sites, one- or two-level outlined returns, and two nested Apply regions, against real Refs, checks two-Slot dispatch preservation and restoration, and confirms the derived generation pin is released. Callback frames carry the full derived context owner stack; a callback-local inner Apply derives and releases its own owner through Jump edges, and the private proof checks its derive, dispatch, return and failure Drops. Disposable CodeGenerator modules also pair unit and direct affine-return bodies with private status-returning host carrier wrappers. Non-Jump transitions and recoverable runtime-failure cleanup proofs, source signature/import/parameter/return publication, a public host return ABI, complete compiler dropGlue, wire round-trip and end-to-end gates remain unimplemented; internal Refs cannot yet be published; see implementation slices. |
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

## ABI boundary ledger (2026-10-05)

This ledger distinguishes a versioned native interface from compiler-internal
proofs and an uncommitted source-language ABI. It does not expand the v1
acceptance snapshot or approve a stable release.

| Layer | Current boundary |
| --- | --- |
| Native Fragment ABI | Implemented v1 descriptor and dispatch C ABI (`LRF1`, version 1): exact Slot/Contract and argument/environment layouts, factory/destroy/execute callbacks, explicit dispatch context and single-shot continuation. Published execute callbacks have no context argument. |
| Native Ref bridge | Implemented v1 check/transfer/drop on validated owning handle cells. This is not arbitrary-pointer adoption or a Luna source import/return ABI. |
| Compiler-internal source Ref proofs | `RuntimeFragmentRef<S>` spelling and exact nominal Slot checking exist. Disposable LLVM modules exercise private unit and affine-return carrier wrappers; a test-only JIT executes a restricted one- or two-region Ref-apply source body using one or two borrowed Refs targeting the same or different exact Slots; it covers direct and outlined Slot sites, including callback-local inner Apply, outlined return escape and cleanup. None of these are public symbols, a published carrier layout or container support. |
| Cross-package source Ref ABI | Uncommitted and blocked: source signatures/imports/returns, complete drop glue, Moon Container round-trip and Ref-operand `apply` have no publishable contract yet. |

A real nested Ref-apply source fixture now verifies the CFG for `?`: its Result
Switch stays within both contexts, and its Err Return records inner-before-outer
context exits. A test-only JIT entry also executes local `Ok` and `Err` inputs
with `Result<i32, i32>` returns, checking dispatch, payload and pin release.
The private JIT also covers an outlined callback Result return and `?` after
a completed Slot dispatch. An apply-local source `Drop` on Err executes once
per call, with Drop and deallocation before context Drop.
An affine source error also converts through its exact frozen `From` method
to scalar `Err(47)` before context Drop in the private JIT; its own source
`Drop` runs once per call. This does not
establish a public return carrier, resource-bearing returned payload cleanup or
recoverable host-failure protocol. Current source Apply entry and normal exit
use Jump edges; non-Jump cross-region edges remain an explicit private-codegen
rejection.
Resource Err and Ok returns have separately passed sealed CFG verification
and test-only JIT execution. The wrapper observes a one- or two-field
resource's marker at its frozen offset, then runs frozen Drop glue and
deallocation once per call; scalar counterpart branches do not Drop. The
observer owner stays inside the JIT module. A separate test-only transfer entry
checks empty, distinct outputs before body execution, commits a resource owner
to a host cell, and pairs it with an explicit JIT Drop entry. The deep
three-struct fixture proves no Drop of the successfully transferred owner
before host Drop, then exact outer-to-inner
Drop and inner-to-outer deallocation on host Drop; a second Drop fails. A
resource Ok also transfers and Drops once per call; scalar Ok and Err
counterparts leave the owner cell empty. An injected failure after the
body returns a resource keeps both outputs untouched and cleans the owner in
the JIT before returning failure. On a successful transfer, the second owner
survives release of the borrowed Ref handle and its generation pin; an
independent LLJIT lease keeps the Drop thunk alive until explicit host Drop.
The raw owner pointer does not carry this lease. A publishable host carrier
must own both. This proof does not publish a return ABI, set production
failure statuses or cover JIT teardown with an outstanding owner.
Two- and three-struct return chains also run through the compiler's recursive
cleanup: private LLVM checks Drop from outer to inner and deallocation from
inner to outer, while JIT probes confirm each Drop once per call.
A bounded three-node fork with two independently owned fields also passes
the private `?` Err and post-body failure/host Drop proofs. The LLVM check
requires field-order Drop/deallocation, and ordinary plus ASAN canonical tests
pass on local Windows CLANG64 and WSL Arch Linux. A sealed four-node fork is
explicitly rejected before private JIT materialization. Larger graphs stay gated.
A separate test-only status entry composes a read-only parent-context check
and an exact borrowed-Ref ingress check with one sealed unit Apply body. A
live matching handle and parent enter the body; null parent, null handle and
live wrong-Slot handle fail without dispatch. The generic unit ingress helper
still rejects Apply regions, and this test entry is not a published symbol.
The compiler-private context check accepts only a live Runtime-created
context; it cannot validate arbitrary or stale pointers. This test entry now
maps success, invalid context, invalid handle, wrong target and unexpected
check results to distinct private `0/1/2/3/4` statuses. JIT calls exercise
the first four; LLVM checks the explicit fallback.
A future host entry needs a public status ABI, including recoverable body
failure rules, and must retain its code lease and both borrowed owners
through the synchronous call.
The private proof now checks the sealed function identity, callable TypeId,
linkage, borrow contract and CFG parameter against the exact first Apply
target before building this test entry; forged linkage or borrow facts fail.
The test hook also produces a pointer-free, versioned candidate row and
checks every identity and convention against those frozen facts. Malformed or
changed rows and generic Native v1 rows fail validation. The row remains
outside Native proof hashing, public descriptors and Runtime bindings.
It now explicitly binds the inferred context-effect bit; a false bit or a
forged source effect fails. The current verifier still blocks exporting a
context-dependent source function.
The test-only loaded entry view checks that the candidate row belongs to the
specific JIT module before exact symbol lookup. It retains that module through
each call: the fixture drops its original JIT reference, executes two calls,
then drops the view and observes the code lease expire. A matching source row
paired with an unbound JIT module is rejected. This does not authenticate a
container or expose a public Runtime binding.
The private fixture also duplicates the live Ref handle's immutable singleton
snapshot and copies the parent context before releasing the originals. A third
call succeeds through these retained owners, and releasing the duplicate Ref
pin ends its generation lease. This is a test-only borrowing proof; public
host call ownership and stale-pointer rules still need a versioned contract.
The bounded candidate is a separate typed entry record for a sealed exported
`shared borrow RuntimeFragmentRef<S> -> unit` function: it binds the function
and exact Slot identities, parameter/return ABI, status version, entry address
and owning generation. Generic Native/Runtime callable descriptors omit those
Ref ingress facts; the Fragment factory/execute descriptor has a different
calling contract. This record remains a design candidate, not a published ABI.
Native v1 hashes only generic export identities and its loader requires the
exact v1 row size. A publishable typed record needs its own versioned layout,
canonical proof fields and loader validation before it reaches a pinned
Runtime binding; see the [implementation plan](slot_fragment_runtime_plan.md).
An exported function reaching a Runtime Slot is currently rejected before
Native v1 artifact emission because it has no runtime-aware public entry ABI.
The 2026-10-06 semantic and `-t native` regressions preserve that gate and
check that the failed Native build leaves no library or trust record.

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
[apply parser](../src/parser/ParserStatements.cpp) and
[real cross-package container loading](../tests/moonir_canonical_runtime_slot_container_test.cpp).
The frozen-identity measurement anchor is `bc9d6fd`, with that stage's
77/77 non-hardware regressions and successful three-platform CI; it is also the
observation build. Report commit `7757b77` is tracked separately. Neither that
report nor this audit commit is the measurement build. Full observations/digests
are in the [implementation plan](slot_fragment_runtime_plan.md#matched-protocol-frozen-identity-observations-2026-09-28).

The 2026-09-28 audit stage's existing strict-warning build passes the eight bounded gates plus documentation
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

The 2026-10-03 local build is up to date. The complete non-hardware CTest run
passes 76/77 in parallel; `luna.repl-smoke` times out during process-tree
cleanup under that load and passes 1/1 when rerun alone. This is not one clean
full-suite pass or cross-platform CI. The release-readiness check still blocks
on the candidate ancestry mismatch despite the lock's `release-ready` label.

On 2026-10-04, after increasing the REPL process-tree cleanup test timeout to
30 seconds and adding the private two-region Ref-apply regressions, the CLANG64
non-hardware suite passed 77/77 with four workers. This is local test evidence;
the candidate ancestry mismatch still blocks release readiness.

On 2026-10-06 the rebuilt CLANG64 worktree passed the complete local
non-hardware suite 77/77 with four workers (71.18 seconds), including the
private Ref Apply, Native artifact and REPL gates. The read-only readiness
check still blocks on the candidate ancestry mismatch. These uncommitted
local results do not replace cross-platform CI or release-candidate evidence.
An isolated WSL Arch Linux Clang/LLVM 22.1.8 build of the same worktree
compiled all targets and passed the complete local Linux suite 76/76 with
four workers in 67.15 seconds. The initial 75/76 run failed only the frozen
ecosystem baseline check: WSL Git lacked the Windows system
`core.autocrlf=true` setting and misread the child worktrees' CRLF checkout as
dirty. Both were clean with that setting; a process-local Git configuration
made the single test and complete rerun pass. This is local Linux coverage,
not remote release-candidate CI.
Implementation commit `a0bf2b5` includes the subsequent Native v2 candidate,
private Ref Apply proofs and compile-only ABI layout tests. Its complete local
non-hardware suites passed 80/80 on Windows CLANG64 and 79/79 on WSL Arch
Linux. The commit is still local; these runs do not provide macOS or remote
release-candidate CI. Read-only readiness still rejects the child releases'
verified Luna source `41ce85e` because it is not an ancestor of `a0bf2b5`.

The 2026-10-06 [Native typed export boundary audit](slot_fragment_runtime_plan.md#native-typed-export-boundary-audit-2026-10-06)
confirms that Native v1 callable rows still hold raw body addresses without
Ref target, context effect or entry ABI profile. The private Ref/unit JIT row
does not change that published contract; a versioned proof, wrapper, loader
and pinned lookup path is the next implementation boundary.
The v1 descriptor emitter now checks requested rows against generated public
declarations and independently refuses context-dependent callables; this
preserves the existing gate while the typed path remains open.
A separate Native artifact fixture now rejects an exported source
`RuntimeFragmentRef<Slot>` parameter at the MoonIR verifier and requires no
library or trust record, independently of the context-dependent Slot fixture.
The v1 emitter now checks frozen source Function types against the generated
LLVM entry before publishing its address. The proposed typed path uses a
parallel v2 descriptor/query, preserving v1 reader behavior.

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
