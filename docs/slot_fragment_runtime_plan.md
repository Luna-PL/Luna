# Slot/Fragment Runtime Injection Plan

English | [简体中文](slot_fragment_runtime_plan.zh-CN.md)

> Status: Confirmed implementation plan, 2026-09-23
> Scope: type-safe, host-controlled runtime injection

## Model

A `slot` is a nominal, fixed injection point. Declaring a slot is the opt-in;
there is no separate `runtime slot` form and no transferable `RuntimeSlotRef`.
A `fragment` is a restricted handler for exactly one SlotId. It may receive the
slot continuation through `resume`, but it is not interchangeable with an
ordinary function even when their parameter shapes match.

The Slot/Fragment-specific source vocabulary is limited to four keywords:

```text
slot  fragment  apply  resume
```

`export` and `meta` remain general declaration facilities. `interceptor`,
`context`, `many`, `abort`, `default`, `runtime fragment`, and `runtime slot`
do not belong to the final Slot/Fragment surface. The first ABI is unit-result
and single-shot. `resume;` is a restricted statement, not a function call.

```luna
export meta plugin(name: string, priority: i32);
export slot pipeline(value: i32);

@plugin("trace", 100)
export fragment trace[prefix: string](value) for pipeline {
    print(prefix);
    resume;
}
```

Static apply remains a zero-Runtime-cost specialization. Binding or importing a
fragment creates a `RuntimeFragmentRef<S>` that owns or borrows its explicit
environment and pins its module generation. `apply` borrows that reference;
each slot invocation creates a fresh single-shot activation.

## Host-controlled discovery and injection

An exported Fragment targeting an exported Slot is a candidate by nominal
relationship, not because metadata grants it a capability. A verified artifact
publishes a data-only `FragmentOffer` containing at least its FragmentId,
target SlotId and ContractId, execution/factory contract, environment layout,
generation identity, and retained policy metadata.

Runtime exposes the semantics of a typed candidate catalog:

```text
candidates(SlotRequirement{SlotId, ContractId}) -> CandidateSnapshot<S>
```

The specification does not require an eager global index. A runtime may scan,
cache, lazily index, or merge per-generation catalogs as long as a snapshot is
complete, deterministic, immutable, and generation-pinned.

Discovery, policy, and execution are separate:

```text
verified FragmentOffer records
    -> CandidateSnapshot
    -> host policy selects None, One, or an OrderedChain
    -> RuntimeFragmentRef construction and one-time contract validation
    -> immutable BindingSet
    -> safe-point activation
```

Runtime never selects a winner, orders candidates by load order, or activates a
new candidate automatically. Loading a generation may notify the host that a
catalog snapshot changed; the host alone decides whether to construct and
activate a new BindingSet. Existing references retain their old ModuleLease.

Slot dispatch reads only the resolved BindingSet. Reflection, metadata
filtering, candidate enumeration, and full ABI validation never occur on the
ordinary slot hot path.

## Metadata and sysmeta

Candidate membership comes from the verified Fragment-to-Slot relationship.
Metadata is immutable, typed policy data used by the host for filtering and
ordering. Attachments whose schemas are public to the candidate interface are
stored directly with the FragmentOffer; they do not make another declaration
callable or executable.

Compiler-owned sysmeta records all safety and lowering facts: SlotId,
ContractId, target relationship, unit/single-shot control, continuation use,
environment layout, ownership, ABI, and generation-lifetime requirements.
User metadata can never forge these facts.

Runtime identity keys crossing the C++/C boundary reject embedded NUL, as well
as CR/LF/tab, so conversion to a null-terminated ABI name cannot silently change
the module, declaration, Slot contract, or argument-layout identity. Staging
rejects malformed generation keys before initialization; candidate discovery,
activation, dispatch (including None), and overrides reject ambiguous Slot keys.
These are C++ identity checks. A C ABI name ends at its first NUL; native callers
must not truncate an unvalidated C++ ID before invoking that ABI.

Runtime-retained metadata requires only a minimal owner identity anchor. It
must not promote its owner to a callable function, executable fragment, open
slot, or Runtime API. This boundary is now enforced by the frontend, MoonIR
verifier, and Runtime descriptor emitter: the metadata owner may keep
compile-time retention, with callable flag zero and a null entry.

## Validation boundary

Artifact verification proves that every FragmentOffer agrees with sealed
MoonIR and the artifact proof. Binding construction validates exact SlotId and
ContractId, environment layout, declaration kind, ABI version, and required
entry flags once. A successfully constructed `RuntimeFragmentRef<S>` can only
be installed for `S`; ordinary function pointers and same-shaped fragments for
another slot are rejected.

The Fragment continuation is compiler-owned, non-forgeable, non-escaping, and
at most once per activation. A future ordered chain advances `resume` to the
next selected Fragment and finally to the base continuation. No implicit link
or load order defines that chain.

Resume failures are sticky within a live activation, including repeated use or
recursive use of that same activation. A void native execute thunk cannot hide
the failure merely by ignoring its negative resume result: the outer dispatch
reports execution failure after the thunk returns. Downstream chain diagnostics
remain intact, and every new invocation starts with fresh activation state.
This neither permits handler-body re-entry nor rolls back native side effects.

### Stage-3 runtime ABI foundation

The first stage-3 slice freezes a C-compatible Fragment descriptor containing
the FragmentId/ContractId, exact target SlotId/ContractId, factory contract,
environment layout, factory/destroy functions, and execution thunk. The thunk
receives a compiler-owned opaque activation rather than a host-constructible
continuation representation.

The C++ host layer constructs a move-only `RuntimeFragmentRef` from a verified
generation binding. Construction pins that binding's generation before validating
every identity and layout once and invoking the factory. A synchronous native
factory callback may clear or replace the source binding handle without changing
the generation selected for the new reference. Rejected-output cleanup retains
the same pin; success transfers it to the reference. Factory-owned environments are destroyed
before the generation lease is released; borrowed environments require their
own explicit lease.

Reference reset detaches retired state before calling owned destroy or releasing
the borrowed-environment lease, and retains the original generation through both.
Nested reset therefore sees an empty reference. A synchronous cleanup callback may
rebind a still-live reference without that value being erased by the outer reset.
Move assignment installs incoming state before old cleanup; it likewise preserves
callback changes. Cleanup must not throw, destroy an object still in use, or
resurrect a destroying object. These native host lifecycle rules do not broaden
Luna handler-body re-entry or change the v1 ABI.

Environment construction checks the actual address against its declared
alignment, not just the alignment field. A rejected non-null factory result is
destroyed once before publication; borrowed storage never enters the Fragment's
destroy callback. Slot activations and both C++/C dispatch reject malformed or unaligned
argument carriers before any Fragment or base continuation callback, including
host policy None. Empty carriers use size 0, alignment 1, and null data. These
checks do not prove allocation bounds or lifetime and do not change the v1 ABI.

The second stage-3 slice implements the source construction split:
`fragment name[environment](slot parameters) for slot` and
`apply name[arguments] { ... }`. Apply arguments are evaluated exactly once in
the Apply region, while every Slot activation borrows the resulting bindings.
Semantic analysis isolates fragment bodies from caller locals, so an omitted
environment can no longer become an accidental closure. The environment is a
frozen structural Record TypeId carried by MoonIR. Apply now owns Copy and
affine fields, activations borrow them, and all normal/outer-escape CFG edges
carry the corresponding cleanup. Linear fields remain rejected because this
reusable environment model cannot establish exactly-once consumption.

The third slice separates publication from retention in the runtime ABI.
Exported Slot/Fragment rows carry `PUBLIC_CONTROL` while keeping compile-time
retention; that flag alone never implies callable or Fragment-executable
entry points. An exported Fragment must target an exported Slot. The canonical
declaration/container model now preserves and verifies the exact target Slot
reference and frozen environment TypeId, so JIT loading cannot recover these
safety facts from user metadata or frontend-only recipes.

The host runtime also exposes immutable, SymbolId-ordered, generation-pinned
candidate snapshots. A query filters by exact SlotId/ContractId and includes
only bindings that are both public controls and executable Fragments. It does
not rank or activate them.

The fourth slice freezes a distinct structural argument Record TypeId for every
Slot and carries the exact record through Fragment rows and verified containers.
The Fragment descriptor now includes that argument layout. Runtime owns an
opaque activation that exposes arguments only after exact SlotId, ContractId,
layout, size, and alignment checks; its continuation is consumed at most once.
As a deliberately bounded first executable ABI, exported Slots currently
require Copy parameter contracts and exported Fragments require Copy
environments. Static composition retains affine environments. Supporting
move-only values across the host boundary requires an explicit transfer/drop
protocol and will not be approximated with an unsafe bitwise copy.

The fifth slice materializes every exported Fragment as an internal ordinary
function. Its body reuses the same verified static `apply`/Slot/`resume` CFG
composition path, while a container-preserved `runtimeEntry` reference keeps
the helper reachable. LLVM emits the stateful factory/destroy pair when needed,
an execute wrapper that validates and opens the opaque activation, and a
`FRAGMENT_EXECUTABLE` descriptor. The verified loader checks the descriptor
against the frozen Slot, argument, and environment layouts before publishing
the executable candidate. Both capture-free and stateful ABI behavior are
covered, including an end-to-end generation load and single-shot resume.

The sixth slice implements the stage-5 host activation boundary. A host may
consume prevalidated `RuntimeFragmentRef` values into an immutable BindingSet
with at most one winner for each exact SlotId/ContractId. `MoonRuntime`
publishes that set atomically only with a fresh same-runtime `SafePoint` and
returns pinned snapshots for dispatch. Replacing the active set does not
retarget or invalidate an older snapshot. Each dispatch retains its selected
snapshot until all handlers unwind, even if a synchronous native callback clears
or replaces the caller's published C++ BindingSet/context handle. This preserves
owned/borrowed environments and generation leases on completion, escape, and
failure paths; it does not permit reusing a released opaque pointer or concurrent
mutation of the same C++ handle. Dispatch performs no discovery,
metadata filtering, or descriptor revalidation: it makes one exact Slot lookup,
checks the invocation argument layout, and either executes the selected
Fragment or calls the base continuation directly for host policy `None`.

The seventh slice extends the same immutable representation with explicit
ordered chains and local overrides. The chain constructor preserves host input
order within each exact Slot even when the candidate catalog has a different
SymbolId order. Each `resume` synchronously enters the next Fragment, the last
one enters the base continuation, and control then unwinds through post-resume
code in reverse nesting order. A local override shares pinned Fragment
references while replacing or suppressing one exact Slot; it never mutates the
base snapshot or the globally active BindingSet.

The eighth slice freezes the compiled-code dispatch boundary. A host converts
one pinned BindingSet into an opaque `RuntimeFragmentExecutionContext` and
passes that capability explicitly at an execution entry. The stable C ABI
accepts that context, one exact Slot identity and argument layout, and a
compiler-owned synchronous continuation. It returns a distinct control result
when the continuation performs an enclosing `return` or `?`; generated
Fragment helpers propagate that result and skip post-`resume` code. The
context pins its BindingSet for the whole call and has no `MoonRuntime*`
back-pointer, candidate catalog access, process-global current Runtime, or TLS
lookup.

### Dynamic dispatch lowering decision

Three context-transport designs were considered:

- A process-global or TLS current Runtime keeps function ABIs unchanged, but
  makes nested hosts, concurrent runtimes, task migration, and local overrides
  depend on invisible mutable state. It is rejected.
- Adding a hidden context parameter to every Luna function is straightforward
  to verify, but charges code size and calling-convention cost to programs and
  call paths that cannot reach a runtime Slot. It is rejected as the default.
- Effect-directed propagation adds the hidden context only to functions whose
  transitive call graph can reach an exported, dynamically dispatchable Slot.
  This preserves the static zero-cost path at the price of a call-graph effect
  and context-aware indirect-call ABI. This is the selected design.

Compiler integration therefore proceeds in this order:

1. Preserve an unbound invocation of an exported Slot as a nominal runtime
   Slot terminator in sealed CFG, carrying its Slot declaration reference,
   frozen argument Record TypeId, arguments, continuation entry, and escape
   outcome. A private Slot with no static `apply` remains an erased identity.
2. Infer and verify a `requires_fragment_context` effect to a fixed point over
   direct calls. Function values and exported entries encode the same ABI fact;
   it is never inferred again from user metadata at load time.
3. Give only affected internal functions a hidden execution-context parameter.
   A runtime-aware exported descriptor provides the explicit host entry;
   ordinary callable exports are rejected if their body transitively requires
   the capability but their published ABI does not carry it.
4. Outline the Slot continuation and its live Copy frame, pack arguments using
   the frozen Slot record, and call `luna_runtime_fragment_dispatch_v1`.
   `CONTINUATION_ESCAPED` selects the already-verified enclosing return/error
   edge; a negative dispatch result enters the Runtime error boundary.
5. Keep static `apply` composition ahead of this lowering. When a lexical
   binding is known, the existing inlined CFG remains authoritative and emits
   no runtime dispatch, BindingSet lookup, or execution-context dependency.

## Implementation stages

1. Introduce the unified `slot`, `fragment ... for`, and `resume;` frontend
   surface; route it through the existing single-shot static pipeline while the
   legacy internal FragmentKind split is removed.
2. Remove `interceptor`, `context`, `abort`, `many`, declaration defaults, and
   Slot/Fragment uses of `runtime`; derive one canonical control model in
   sysmeta and MoonIR.
3. Add explicit fragment environments, Fragment factory/execution descriptors,
   `RuntimeFragmentRef<S>`, one-time validation, cleanup, and ModuleLease.
4. Emit verified FragmentOffer records and expose immutable, generation-pinned
   CandidateSnapshot semantics to the host.
5. Add host-created immutable BindingSet and safe-point activation; keep
   candidate discovery and metadata policy off the dispatch path.
6. Add deterministic ordered chains and local `apply` overrides on the same
   binding representation, followed by performance and artifact-cost gates.
7. Add explicit execution contexts, preserve dynamic Slot terminators, and
   propagate the context capability only across dynamically affected calls.

As of 2026-09-25, stages 1 through 6 are complete. The AST, type system, Sema,
sysmeta, MoonIR, lowering, verifier, JIT, and AOT paths now expose one canonical
single-shot Fragment model. The 0.3 container keeps two reserved wire positions
at their existing offsets, writes only the canonical values, and rejects legacy
values when decoding; those positions no longer appear as semantic fields.
The runtime ABI/reference foundation and compiler-side explicit Copy/affine
environment ownership, borrowing, transfer checks, and cleanup lowering are
implemented. Public-control descriptor publication,
container-preserved target/environment facts, and candidate snapshot semantics
are also implemented. Slot argument layout preservation and the Runtime-owned
opaque single-shot activation are complete. Compiler-generated hidden entries,
factory/destroy/execute thunks, executable descriptor publication, and verified
generation loading are also complete. Host-owned immutable BindingSet
construction, safe-point activation, pinned dispatch snapshots, None/One
dispatch, deterministic ordered chains, and immutable local Slot overrides are
complete. The artifact-cost gate now proves that exported executable Fragments
materialize the descriptor/factory/execute ABI while private static composition
does not retain it. The explicit execution-context C ABI and continuation
escape propagation are also complete. Stage 7 now preserves every unbound
exported Slot invocation as a verified `RuntimeSlot` terminator carrying the
exact Slot declaration reference, frozen argument Record TypeId, packed
arguments, continuation entry, and completion edge. Statically bound `apply`
still composes first, and an unbound private Slot still erases to its
continuation. The container codec and projection retain the new terminator
facts. The compiler also derives `requires_fragment_context` to a
least fixed point over exact direct calls, serializes the result in function
code rows, and has the verifier independently recompute it to reject forged or
stale summaries. The effect is now materialized as one leading opaque context
parameter on affected internal LLVM functions, and exact direct calls forward
the caller's capability. Unaffected functions keep their original ABI.
Ordinary exports, `main`, externs, kernels, and function-value materialization
cannot silently cross this boundary; a context-requiring `runtime fn` is the
supported host entry, while context-aware indirect calls remain unsupported.
Nested closure bodies remain
conservatively included in the effect. A context-requiring `runtime fn` now
serves as that explicit host entry: its existing Function descriptor and
generation binding carry `FRAGMENT_CONTEXT`, which states that the entry takes
a leading opaque execution-context pointer. `export` remains ordinary external
visibility and does not acquire this ABI implicitly. Cross-package invocation
and exported Fragment targeting use the dependency's exact Slot declaration
identity and require that dependency to expose the Slot as a public control.
The verifier checks that a root export belongs to its publishing package and,
when executable code is present, agrees with the declaration's public flag;
a forged export row cannot turn a private or foreign Slot into a runtime target.
Foreign Slot package ownership must also agree with the stable declaration ID,
not merely a mutable package label or dependency row.
An end-to-end host test now separately compiles the Slot owner's full package
and the plugin package, loads them as distinct JIT generations, selects the
plugin candidate for the host Slot, and distinguishes selected dispatch from
the unbound continuation. The execution context keeps the plugin generation
alive after the runtime tears down and releases it when the context is dropped.
The host package's unused exported compiler-domain selector is classified at
declaration time and erased from MoonIR even without a local `select` call.
The current lowering packs the
frozen Slot argument record and calls `luna_runtime_fragment_dispatch_v1` with
a synchronous stack frame containing the execution context, return storage,
and pointers to live captures. The callback mirrors those values into
typed locals, executes its outlined blocks, writes mutations back, and reports
normal completion or an enclosing `return`/`?` escape. Canonical cleanup edges
for continuation-local and enclosing resources, plus Result switch case bindings, are emitted
inside the callback before it reports escape. The dispatcher then selects the
completion edge or returns the escaped value. A continuation can call a second
function that reaches a dynamic Slot, with the same context forwarded. Lexically
nested Slots recursively outline their callbacks, share the context and return
storage, and write back transitive captures. Ownership analysis also treats a
runtime Fragment that omits `resume` as a continuing path, so both that path
and an escaping continuation retain their exact resource cleanups. A non-timing
structural gate now requires one generated
dispatch call per dynamic Slot site and no Runtime selection machinery in a
static-only composition.

### Static cycle safety and continuation nesting

As of 2026-09-26, Sema and the CFG builder independently reject direct or
mutual static Fragment expansion cycles instead of recursing indefinitely.
Regression tests cover both source diagnostics and a recursive MoonIR body
injected after semantic analysis. Finite same-Slot nesting in a base
continuation remains valid, including lexical binding overrides.
The CFG verifier accepts entry into a directly nested Fragment only through its
recorded entry block; an adversarial jump to a nested non-entry block remains
rejected. Runtime C ABI tests reuse one pinned context and selected chain for
nested same-Slot dispatch,
checking independent single-shot activation state and propagation of an inner
continuation escape through both chains. Arbitrary handler-body runtime
recursion is still outside the frozen `TBD-SF008` guarantee; these checks do not
introduce a runtime depth limit or automatic suppression policy.

### Published handler execution boundary

The published-handler boundary is checked independently of local use: exported
Fragment bodies receive semantic and independent ownership analysis even when
no local `apply` exists. Local linear state must be consumed on all completing
paths, conflicting loans and repeated frees fail in the frontend, and implicit
affine cleanup is retained for host-only candidates. Ownership analysis uses an
opaque, normally completing continuation with no caller resources; actual
static applications additionally check their real continuation paths.
After sealing, the verifier uses its recomputed direct-call fixed point to
reject any Fragment runtime entry requiring an execution context, because the
v1 public execute wrapper cannot pass that capability. Direct and transitive
handler dependencies are rejected by `luna check` before codegen; a forged
context-free helper summary cannot bypass the check. Statically bound exported
Slots, erased private Slots, and private static Fragment composition remain
valid. Erased private Slot bodies use lexical regions, not suspended
continuation regions, so they can execute inside a handler and access its
locals without weakening the real Fragment continuation boundary. Runtime
handler context propagation and its re-entry policy still require an explicit
ABI/design decision; no new implicit context source is added.

The [SF008 bounded contract evidence](slot_fragment_contract.md) consolidates
these boundaries and the nested override/None and static discard/return gates.
It records implemented behavior, not stable-release approval.

### Concurrent host evidence

`luna.runtime-fragment-concurrency` now verifies host-owned atomic publication
against four readers whose continuations remain live across 96 None/One/chain
transitions. It covers normal completion, escape, failure isolation, and 1024
further dispatches through reader-owned copies of one pinned context after the
Runtime is destroyed. The independent target instruments both Runtime sources
under ASan/UBSan or the separate Linux TSan job, without instrumenting the AOT
archive or LLVM/ORC. Its immutable native environments and per-call argument
storage do not confer arbitrary plugin thread safety or introduce a Luna
cross-thread API. See the [testing guide](testing.md) for reproduction.

### Reproducible runtime cost probe

`runtime-fragment-benchmark` is an explicitly built microbenchmark, not part of
the default build or a timing-based CI gate:

```sh
cmake --build build-perf --config Release --target runtime-fragment-benchmark
./build-perf/runtime-fragment-benchmark 100000 64
```

On Windows the executable has an `.exe` suffix; multi-config builds place it
under `Release`. The first argument is iterations per case. The second is the
number of Fragment rows in one generation (4–4096), of which four target the
queried Slot. Run with 4, 64, and 256 rows and record the platform, compiler,
build type, and ns/op instead of comparing absolute times across machines.
The cases separate candidate snapshot discovery, ref plus BindingSet creation,
four-ref chain construction, local None override construction, safe-point
activation plus pinning, and explicit C ABI dispatch for None, One, two-/four-member
chains, and local None over a four-member base chain. Context construction is
outside the dispatch timers. The native capture-free handlers open the checked
argument carrier and increment a private call counter; timings include these
fixture checks and the base callback, not just dispatcher overhead.
Each case warms up for `min(iterations, 1000)` calls, then measures the requested
iterations. Per-dispatch and aggregate counters reject missing handlers, duplicate
continuations, or an override that still executes its base chain. The `10 4` CI
smoke must report `checksum=210, continuation_calls=100, fragment_calls=140`;
timing never determines success. Discovery is control-plane work and may scale with
catalog size; hot-path dispatch must not. Non-timing structural tests remain
the hard regression gate. This probe supplies performance evidence without a
machine-dependent CI time threshold.
Platform CI only builds the probe and runs a tiny correctness smoke; it never
compares the reported times. The `11 64` and `1001 256` cases also cover odd
activation counts and the warmup cap; their expected final counters are
`232/110/154` and `21011/10005/14007` (checksum/continuation/Fragment).

#### Local measurement snapshot (2026-09-26)

Environment: Windows 11 build 26200, Intel Core i7-12700 (12 cores, 20 logical
processors), MSYS2 CLANG64 Clang 20.1.8, C++17, Ninja RelWithDebInfo with strict
warnings. Runtime implementation: `2c9cf754f17678978dd9f1fa2eb4c50689f001cb`;
probe Git blob: `d6c94daea117324b30b01b9c8d90b4cabe124200`.
Five separate processes per row count, in order 4, 64, 256, each with 100000
iterations per case and 1000 warmup calls. All 15 processes verified
`checksum=1060500, continuation_calls=505000, fragment_calls=707000`.
CPU affinity, power policy, and background activity were not controlled.

Values are median ns/op, with observed minimum–maximum in parentheses:

| Case | 4 rows | 64 rows | 256 rows |
| --- | --- | --- | --- |
| candidate_snapshot | 651.6 (637.2–721.5) | 6671.2 (6534.8–6821.2) | 25699.0 (25501.5–26090.7) |
| ref_plus_binding_set | 861.1 (830.7–904.8) | 850.0 (846.1–885.6) | 847.2 (826.1–866.8) |
| refs_plus_chain_4 | 1948.0 (1927.2–1980.6) | 1981.0 (1894.5–2054.9) | 1925.0 (1899.1–2149.2) |
| local_override_none | 279.4 (274.5–295.3) | 281.6 (270.5–295.7) | 273.3 (271.2–275.6) |
| safe_point_activate_and_pin | 56.4 (55.5–56.9) | 54.7 (53.9–56.4) | 54.5 (52.9–55.3) |
| dispatch_none | 217.2 (212.6–233.6) | 213.1 (205.1–222.1) | 210.5 (208.1–225.3) |
| dispatch_one | 503.6 (495.4–523.5) | 720.1 (678.5–748.5) | 493.3 (483.8–930.0) |
| dispatch_chain_2 | 730.7 (707.3–759.8) | 911.3 (876.6–918.4) | 1398.2 (1062.8–1540.1) |
| dispatch_chain_4 | 1186.1 (1145.8–1249.9) | 1362.6 (1347.0–1480.1) | 2147.7 (2051.7–2372.3) |
| dispatch_override_none | 222.9 (212.0–229.6) | 213.6 (210.2–218.6) | 423.2 (416.5–564.5) |

Discovery shows the expected scanning cost. Dispatch has no catalog access by
construction, but these samples do **not** establish constant measured latency:
chain and override timings vary significantly, and the causes are unisolated.
Do not attribute the variation to catalog scanning or dismiss it as scheduler
noise without further evidence. Controlled, interleaved runs and independent
Linux/macOS measurements remain necessary before a performance acceptance claim.
This is native-fixture evidence, not compiled plugin workload or release approval.
