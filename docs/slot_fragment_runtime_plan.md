# Slot/Fragment Runtime Injection Plan

English | [简体中文](slot_fragment_runtime_plan.zh-CN.md)

> Status: Confirmed implementation plan, 2026-09-23
> Scope: type-safe, host-controlled runtime injection
> Current acceptance view (2026-09-28): see the [v1 acceptance snapshot](slot_fragment_contract.md#v1-acceptance-snapshot-2026-09-28).
> The native-host loop is implemented. A test-only private JIT now executes a restricted single-region source Ref/apply body, including sequential and outlined Slot sites. Public source ingress/return, container publication and the full cleanup contract remain unimplemented; the slices below distinguish these boundaries.

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

Static apply remains a zero-Runtime-cost specialization. In the final design, binding or importing a
fragment creates a `RuntimeFragmentRef<S>` that owns or borrows its explicit
environment and pins its module generation. `apply` borrows that reference;
each slot invocation creates a fresh single-shot activation.

## Source Ref/apply implementation slices (2026-09-28)

The selected direction lets Luna source act as a host: receive, select and locally
apply runtime references. Start with host-supplied verified Refs, without also
adding a source loader, global candidate index, handler re-entry or keywords.
The complete example remains **target syntax, not executable through the
public compiler/package path**. Source Ref spelling, nominal and borrow
checks, the canonical Apply region, and a restricted private JIT execution
path exist. Public Ref ingress/return and container publication do not.

```luna
export slot pipeline(value: i32);

// Unqualified move-only parameters are shared borrows, not ownership transfers.
runtime fn host_entry(selected: RuntimeFragmentRef<pipeline>) {
    apply selected {
        pipeline(42) { print("base"); }
    }
}

// Explicit affine parameters take ownership; owning returns carry no local context.
fn transfer(selected: affine RuntimeFragmentRef<pipeline>)
    -> affine RuntimeFragmentRef<pipeline> {
    return selected;
}
```

### Surface, identity and ownership

The dedicated keywords remain `slot fragment apply resume`. `RuntimeFragmentRef<S>`
is a builtin type name; `S` resolves to an exact Slot declaration. Initially keep
name operands: `apply trace[arguments] { ... }` composes a static Fragment;
`apply selected { ... }` locally overrides using a Ref value. Ref operands take
no environment arguments: construction fixes the environment, not each apply or
Slot invocation. Arbitrary expressions such as `apply choose() { ... }` are not
part of the first slice; there is no second apply keyword.

TypeId is parameterized by stable nominal Slot declaration identity, not parameter
shape, a user string or metadata. Sealed ContractId is separately matched by
artifact verification and host ingress. Do not feed the final ContractId back into
its dependent TypeId, creating an identity cycle. Same-shaped Slots remain distinct;
old Refs never silently upgrade contracts. Equal ShapeId/handle layout permits no
conversion. Candidate data is not a Ref; source has no null Ref or raw-pointer cast
constructor.

Owning local Refs are affine: movable, discardable and returnable, not implicitly
copyable. Existing unqualified move-only parameters are shared borrows; explicit
affine parameters take ownership. Apply evaluates once and shared-borrows throughout
the region. Sequential apply reuses the Ref; each Slot call creates a fresh
single-shot activation. The continuation, not the Ref lifetime, is single-shot.
Reject moving/freeing its owner during borrowing, escaping a borrow or returning
an owning Ref from a borrowed parameter.

### Host bridge and generation lifetime

Initially use the existing verified generation/factory path, then explicitly wrap
a native Ref in frozen state with exactly one Slot and one Fragment. Arbitrary
BindingSets cannot stand in for source Refs. Ingress checks nominal Slot, sealed
contract, carrier ABI and owning/borrowed convention before invoking source code.
Native Ref check/transfer/drop functions and private generated wrappers exist.
The **public source** import/return/drop bridge and carrier ABI remain
uncommitted; there is no C source-handle ingress, and the v1
descriptor/execute signatures are unchanged.

The source owner uniquely owns its wrapper; internal environment/generation pins
may be shared without exposing source Copy. Drop needs Runtime glue, never Luna
`free` for a C++ object or plugin environment. Final release destroys environments
before unloading modules; borrowed native environments still need an explicit
environment lease. Exceptions, return and `?` must clean up region contexts/owners.

Source `.bind` is not the initial constructor: execution contexts contain only a
BindingSet, with no MoonRuntime/generation/catalog back-pointer. Declaration
addresses, runtime metadata and TLS cannot prove an escaping Ref's module lifetime.
Future construction needs an explicit verified, generation-pinned offer/construction
capability; acquisition is a separate task, not an implemented reflection feature.

### Local apply and explicit context

Derive from a valid explicit parent, replacing only exact `S` with One(ref) while
preserving other Slots. Nested same-Slot regions shadow, never implicitly append;
exit uses the parent. No global BindingSet publication, safe point, candidate
enumeration or automatic winner selection occurs. Host source chooses the Ref
and its application site.

The direct-call context-effect fixed point must recognize runtime apply, with
independent MoonIR verifier recomputation. Do not synthesize an empty root; hosts
may explicitly supply an initialized None context. Handler bodies still cannot
acquire/pass context or directly/transitively dispatch dynamically. Context-aware
indirect calls, Ref-bearing exported Slot/Fragment payloads, non-Copy environments,
non-unit and multi-shot remain outside this slice. A normal runtime entry's Ref
handle bridge is not an expanded public Slot resource-payload ABI.

### Internal representation trade-offs

| Approach | Benefit | Cost / decision |
| --- | --- | --- |
| Consume native Ref per apply, reconstruct next time | Smallest implementation surface | Breaks borrowing/repeated apply, reruns factories; reject |
| Make source Ref Copy/reference-counted | Direct multi-installation | Changes affine semantics, expands aliasing/concurrency promises; reject |
| Affine wrapper around a frozen singleton BindingSet; local contexts share pins | Reuses validation/cleanup without rebuilding environments; source stays non-Copy | Copies Slot indexes/chain pins at region entry; selected initially, private representation can be optimized |

Current setup grows with base Slots/chains. This is not O(1), allocation-free
region setup or an arbitrary Ref/environment thread-safety promise. No per-Slot
invocation reflection/factory is added. Measure region frequency/context size
before optimizing; never remove validation or borrow callback-destroyable storage.

### Delivery order and completion gates

1. **Implemented native support**: `makeRuntimeFragmentBindingOverrideFromSnapshot`
   accepts initialized None or a frozen ordered chain for exactly one requested
   Slot; `makeRuntimeFragmentExecutionContextOverride` derives from an explicit
   parent. Inputs are not consumed, failure publishes no output, and the vector
   override shares the merge implementation. Native ordered chains do not make
   source Refs multi-Fragment values.
2. **Partially implemented types/artifacts**: internal nominal Ref/ownership/resource
   facts and in-memory freezing/materialization are complete as detailed below.
   Source fixtures parse exported Slots with typed arguments, and LLVM can emit
   local Ref Drop/transfer. Complete compiler dropGlue and Ref container
   round-trip remain unimplemented. Old wire ordinals are unchanged; current
   readers/writers explicitly reject the internal Ref.
3. **Partially implemented host bridge**: native strict singleton owning handles,
   exact-target checks, borrowed context derivation and Runtime drop are complete
   below. Disposable unit-ingress and direct affine-return wrappers exercise
   private carrier transitions. Public source import, owning/borrowed parameter
   and return ABI, compiler dropGlue and two-package carrier/contract validation
   remain unconnected.
4. **Partially implemented source apply**: exact-Slot local Ref recognition,
   environment-argument rejection, lexical shared-loan checks, the canonical
   Apply record, context-effect fixed point and independent CFG flow plan exist.
   A disposable CodeGenerator and test-only JIT execute one top-level region
   and, within it, one nested region using either the same borrowed Ref or a
   second borrowed Ref for the same or a different exact Slot. Jump transitions derive/drop
   each region's context from the active parent; normal and outlined returns
   release the stack inner-to-outer.
   Sequential and outlined Slot sites, repeated application and generation
   pin release are exercised. A two-Slot JIT fixture checks preservation of
   the outer Slot inside the inner region and restoration after its exit.
   An inner Apply inside one outlined callback now has normal-exit and nested
   return JIT coverage. Non-Jump context transitions and `?`/recoverable
   failure cleanup remain unproved.
   The production module
   verifier, container encoder and public CodeGenerator still reject Ref
   publication.
5. **End-to-end gate**: real two-package verified containers with Ref import/source
   application, parameter/return transfer and invalid candidates. Only then may
   `source-ref-apply` become implemented. Direction approval/native support closes
   neither source gaps, historical TBDs nor release authorization.

Native regressions cover repeated derivation, same-Slot replacement/None,
preserved other Slots, rejection of mismatched contracts/multiple Slots/
uninitialized inputs/aliased outputs, and no repeated factory calls. The lifetime
matrix now has 1536 cases: owned/borrowed environments; direct/snapshot-derived
BindingSets, C++ contexts and C ABI; release/replacement during execution;
destroyed caller records; repeated resume; completion/escape/invalid control/
throwing failure; and cleanup ordering. Two concurrent readers derive independent
contexts from a frozen parent/selected snapshot while two use the original path.
This tests immutable native environments, not source Send/Sync eligibility.

The strict build passes all 77 non-hardware gates (208.49 seconds). After the final
concurrency-fixture extension, rebuild and rerun of the two Runtime gates, design
status and inventory passes 4/4. Windows ASan/UBSan Runtime gates pass 2/2; direct
Runtime and concurrency C++17 ASan/UBSan compilation/execution passes on WSL Arch
Linux Clang 22.1.8. The status gate additionally rejects obsolete scope-open state,
without claiming source completion. No new matched performance sampling, evidence
anchor replacement, or release-gate change occurs in this stage.

### Internal Ref type preparation (2026-09-28)

This slice adds only internal `TypeKind::RuntimeFragmentRef` and
`Type::makeRuntimeFragmentRef(slotType)`. It registers no source predefined type
and changes no apply parser. The sole type edge names an exact nominal Slot:
TypeId encodes declaration identity, while ShapeId retains signature structure,
independent of runtime generation/final ContractId.

Formation requires a concrete unit/single-shot/Copy-only Slot. Reject functions,
anonymous same-shaped Slots, unresolved type parameters/inference/Unknown,
Ref-bearing target payloads and inconsistent contract facts. Ref resources are
owned/affine/Unique/Executable/Lexical with Drop, not Luna Deallocate. Ref is a
Plain value, not an owned single-shot continuation. Unqualified parameters borrow;
explicit affine parameters own. Array/record/Result/closure recursive cleanup
uses existing rules.

Ordinary shape equality permits no Ref conversion. Explicit conversion/ABI
compatibility additionally compares nominal Slot constraints at their exact graph
positions, including pointers, borrows, callable signatures, captures and nominal
aggregates. A changed inner Ref target cannot be erased even when the outer nominal
TypeId and normal shape stay equal. This does not replace future sealed ContractId
checks at host ingress.

MoonIR freezes the Slot graph through existing `innerTypeId`/`referencedTypeIds`.
TypeMaterializer independently restores identity/resources; mutating frontend
objects changes no frozen result. The 64-bit internal layout prepares one opaque
handle (8 bytes/alignment 8), not a published C carrier ABI. This type slice did
not connect Drop/dropGlue: freezing is not verified/executable publication.
The subsequent native bridge below still leaves source Drop gates closed.

Old enum ordinals Slot=27, Fragment=28 and Unknown=40 remain; internal Ref is appended
as 41. The wire decoder still rejects beyond its old limit, and the writer rejects
this kind too. Module verification explicitly rejects published Refs; LLVM helpers
cannot silently use the unknown-type i32 fallback. Forged Drop/sysmeta or a nonexistent
release symbol grants no bypass. Lifting the gate requires real host/release bridges
and cross-package verification together.

Regressions in `core_contracts_test.cpp` and `moonir_canonical_sealing_test.cpp`
cover distinct same-shaped Slots, stable identity versus contract shape, seven
wrappers, nominal aggregate payload changes, 17 forged Ref facts, invalid/unresolved
targets, recursive traversal, freezing/materialization isolated from frontend
mutation, writer/container/LLVM rejection and injecting ordinal 41 into an old type
section without decoder partial publication. `core-contracts-test` now directly
instruments its own implementation objects in sanitizer builds, leaving the installed
Runtime archive unchanged. Source signatures, region borrows, import/return/drop
and dynamic apply remain unimplemented; `source-ref-apply` stays implementation-open.

The final strict build passes all 77 non-hardware gates (237.00 seconds). Windows
ASan/UBSan builtin types, core contracts, canonical MoonIR and container model gates
pass 4/4 (14.29 seconds); core implementation objects are confirmed instrumented,
not merely linked against sanitizer runtimes. Direct C++17 core-type ASan/UBSan
compilation/execution also passes on WSL Arch Linux Clang 22.1.8. Design status,
documentation inventory and diff checks pass. No matched performance sampling,
release approval or historical TBD closure is added.

### Native Ref handle / Runtime Drop bridge (2026-09-28)

`RuntimeFragmentRefHandle` is a non-Copy, movable unique owner. Only
`makeRuntimeFragmentRefHandle` creates one by consuming a validated native Ref
for one exact SlotId/ContractId. Empty Refs, mismatched targets and initialized
outputs fail without consumption. All allocations precede the final move;
failure injection at every allocation proves rollback without lost ownership
or partial output. The private frozen BindingSet contains exactly one Slot and
one Fragment: no API imports None, multiple Slots or an ordered chain as a Ref.

`opaque()` borrows; `release()` transfers unique ownership to a raw carrier.
Additive C ABI `luna_runtime_fragment_ref_check_v1` checks the live handle's exact
target without heap allocation. `luna_runtime_fragment_ref_drop_v1(void**)`
clears the carrier before cleanup. Null carrier/address and reentrant drop
through the same cleared carrier are safe; duplicated owners, stale/foreign
pointers and concurrent mutation are not. Magic is a tag, not arbitrary-pointer
validation or a global handle registry.

`makeRuntimeFragmentExecutionContextOverrideFromRef` borrows the handle and reuses
frozen derivation: repeated use neither consumes it nor reruns its factory;
nested same-Slot use replaces rather than appends, preserving other Slots and
the parent. Each derived context independently pins environment/generation and
may outlive the handle/parent. Final pin release uses existing Ref cleanup,
environment before module release. C++ reset/move assignment also detach/install
before callbacks; cleanup rebinds of live owners survive callback return.

This is only an additive native bridge, not source/container carrier publication.
Descriptor v1, execute arguments, old wire ordinals and compiler guards remain
unchanged. Source signatures, import/parameter/return transfer, compiler dropGlue,
wire round-trip, region borrows and dynamic apply remain unfinished;
`source-ref-apply` stays implementation-open. No global candidate index, TLS or
reflection hot path is introduced. No new matched performance sampling or old
evidence/release-gate changes occur.

The strict full build passes all 77 non-hardware gates (202.19 seconds). After
the final module-cleanup pin assertion, rebuild/run of both native Runtime gates,
design status and inventory passes 4/4. Windows ASan/UBSan Runtime gates pass 2/2;
direct Runtime C++17 ASan/UBSan compilation/execution passes on WSL Arch Linux
Clang 22.1.8. The C ABI compile fixture checks C function-pointer signatures for
both new operations; diff checks pass. These are local checks, not remote CI or
stable-release approval.

### Source Ref type spelling and constraint boundary (2026-09-28)

The predefined type constructor `RuntimeFragmentRef<S>` is still an ordinary
Identifier, **not a new keyword**. Parsing reuses generic type syntax. Only in
this constructor's argument position does semantic analysis resolve a module
Slot declaration to its exact nominal Slot type; same-shaped functions/structs
and wrong arities fail. `S` must meet the prior concrete unit/single-shot/Copy-only
formation rules. No Ref construction from arbitrary declaration addresses is
introduced.

Constraint unification now preserves the nominal target and ABI shape. It cannot
silently equate distinct same-shaped Slots or a changed signature under the same
Slot identity. Inference traversal visits the Ref target; AST reconstruction keeps
the resolved nominal identity. Unqualified parameters remain shared borrows;
explicit affine parameters own. This does not grant source Ref copying or handler
re-entry.

Analysis-snapshot tests cover valid declarations, non-Slot targets and missing/
extra arguments. A real source-to-MoonIR regression demonstrates that the type
parses and lowers into internal preparation but the verifier still rejects
executable publication. Source import, owning parameter/return transfer, compiler
dropGlue, wire round-trip and dynamic `apply` remain unfinished;
`source-ref-apply` stays implementation-open and the old container decoder limit
is unchanged.

This slice passes a strict full build and all 77 non-hardware gates (257.87
seconds). Windows ASan/UBSan builtin types, analysis snapshot and canonical
MoonIR pass 3/3; direct C++17 builtin/semantic-constraint ASan/UBSan compilation
and execution pass on WSL Arch Linux Clang 22.1.8. Design status, inventory
and diff checks pass. These are local checks, not remote CI or stable-release
approval.

### Native owning-handle ingress/return transfer (2026-09-28)

Additive C ABI `luna_runtime_fragment_ref_transfer_v1(void** source, SlotId,
ContractId, void** destination)` prepares one primitive for both future owning
arguments and owning returns. It requires distinct live carrier cells, a
nonempty source and empty destination, then checks the exact SlotId/ContractId
using the existing Ref check. Success clears the source and installs that same
unique handle at the destination. Failure changes neither cell. No factory,
environment/generation-pin copy or heap allocation occurs. Default shared-borrow
parameters use `check` only: they must not gain Drop ownership through transfer.

Native regression covers null/aliased/occupied cells, wrong Slot/Contract,
consumed sources, owned/borrowed environments, host→argument→return→host
round-trips, repeated checking and final once-only Drop. The C header compile
fixture checks the new function-pointer signature; Windows and WSL Linux
instrumented execution pass. This primitive is not wired to generated function
ABI, exceptional/return/`?` paths or compiler dropGlue. It opens neither MoonIR
verification nor container publication, and adds no source keyword or plugin
selection policy.

The strict full build and all 77 non-hardware gates pass (223.99 seconds).
After making the carrier write order explicit (clear source, then install
destination), affected targets are rebuilt and canonical MoonIR, Runtime ABI,
Runtime Fragment/concurrency, design status and inventory pass 6/6. Windows
ASan/UBSan Runtime gates pass 2/2, as does direct Runtime C++17 ASan/UBSan
execution on WSL Arch Linux Clang 22.1.8. This is local verification, not
remote CI or stable-release approval; old performance evidence is unchanged.

### Compiler Ref Drop call preparation (2026-09-28)

LLVM represents an internal Ref as an opaque pointer, never the unknown-type
i32 fallback. Direct local and canonical value cleanup call
`luna_runtime_fragment_ref_drop_v1(void**)` with the original writable carrier
cell for a `Drop` action; a drop callback uses its supplied cell directly.
An extracted aggregate payload with only an SSA copy and no original field
cell is rejected explicitly, never duplicated or sent to `rt_dealloc`. The
JIT binds the same runtime symbol. A focused test checks the LLVM call target,
cell argument and module validity;
native ABI tests cover clear-before-callback and exactly-once release.

This is cleanup-call preparation, not complete compiler dropGlue. Aggregate
cleanup still needs original-field clearing and partially initialized/moved
cleanup. Source function ingress, owning parameter/return transfer, borrow
regions, dynamic `apply` and cross-package wire round-trip remain unwired.
MoonIR verifier and container gates stay closed; `source-ref-apply` remains
implementation-open.

The local strict build and all 77 non-hardware gates pass (220.43 seconds),
as does the Windows ASan/UBSan canonical MoonIR test (1/1). Design status,
file-guide inventory and diff checks pass. This is not remote CI or stable
release approval.

### Internal Ref local transfer preparation (2026-09-28)

In the internal codegen preparation path, `move ref` of a direct local lowers as load,
clear the original carrier cell, then hand off the value. A direct `return ref`
from an owned local/parameter uses the same operation so subsequent exit
cleanup callbacks cannot observe a second owning source. Projected field/index
Ref moves and returns fail closed until in-place projected cells and partial
initialization/move handling are proved. A focused LLVM test checks that the
clear immediately follows the load and the destination Drop still targets the
dedicated Runtime symbol.

This is not the host-to-source ABI. Generated functions still accept a raw
pointer argument without checking SlotId/sealed ContractId or consuming the
host carrier cell. Cross-boundary owned/borrowed parameter and return rules,
aggregate cleanup and wire round-trip remain unfinished. MoonIR/container
gates stay closed; `source-ref-apply` remains implementation-open.

This slice passes the strict build and all 77 non-hardware gates (204.15
seconds); Windows ASan/UBSan canonical MoonIR passes 1/1. Design status,
inventory and diff checks pass. This is local evidence, not remote CI or stable
release approval.

### Frozen Ref target resolution (2026-09-28)

`Module::resolveRuntimeFragmentRefTarget(TypeRef)` now resolves an exact
SymbolId/ContractId from a sealed `RuntimeFragmentRef<S>` inner type and its
nominal Slot declaration. It rederives Slot/Ref TypeIds and canonical types,
then checks the declaration type, symbol, canonical contract and sysmeta
identity. A same-shaped different Slot, mismatched declaration type, forged
contract or unsealed table is rejected. The regression uses two same-shaped
Slots from real source lowering and confirms that the Ref publication verifier
gate remains closed after restoring the table.

This supplies target facts for a future ingress; it does not validate a live
native handle or change generated functions' raw-pointer ABI. A borrowed host
parameter needs a read-only `check`; an owned one must validate and transfer
through distinct carrier cells, not copy a raw pointer and call it owning.
Ingress error reporting, cross-package evidence, owning return delivery and
wire round-trip remain unfinished; `source-ref-apply` and MoonIR/container
gates stay closed.

The local strict build and all 77 non-hardware gates pass (209.57 seconds),
as does Windows ASan/UBSan canonical MoonIR (1/1). Design status, file-guide
inventory and diff checks pass. This is not remote CI or stable-release
approval.

### Ref host-ingress LLVM call preparation and secondary gate (2026-09-28)

The compiler now has separate internal IR emitters: a shared borrow calls
`luna_runtime_fragment_ref_check_v1` on the live handle; an owned ingress calls
`luna_runtime_fragment_ref_transfer_v1` with distinct source/destination
carrier cells. Both accept a complete `DeclarationRef`; the focused regression
passes the SlotId/ContractId from frozen Ref target resolution and checks the
constants, call symbols and carrier argument positions. Their status remains
for a future ingress wrapper to handle; the JIT binds both Runtime symbols.

`CodeGenerator::generate` also fails closed on Ref types. Bypassing the MoonIR
verifier and calling the backend directly can no longer publish an unwrapped
raw-pointer function as a host entry. No ingress wrapper is generated or run
yet; status-to-source-return mapping, cross-package import and owning-return
delivery are unfinished. MoonIR/container gates and `source-ref-apply` remain
unchanged.

The strict build and all 77 non-hardware gates pass (195.47 seconds), as does
Windows ASan/UBSan canonical MoonIR (1/1). WSL Arch Linux Clang 22.1.8
syntax-compiles the changed backend files. Design status, file-guide inventory
and diff checks pass. These are local checks, not remote CI or stable-release
approval.

### Ref host-ingress status gate preparation (2026-09-28)

The compiler now emits separate borrow and owned ingress gates for an `i32`
status-returning host wrapper. A borrow checks the handle without acquiring
Drop rights; an owned ingress transfers from the caller's carrier into a
distinct, initially empty callee carrier. Only status zero branches to the
body; failure returns the unchanged Runtime status before user code. The IR
fixture checks both branch destinations, the returned failure value, exact
frozen Slot/Contract identities, the owned carrier position, and rejection of
a wrapper without a status return. The owned fixture drops its callee carrier
on the success path.

These are internal LLVM building blocks, not a generated or executed source
host wrapper. They do not define a general source return/error ABI, borrow
lifetime across callbacks, owning return delivery, aggregate dropGlue, or
cross-package import. Direct code generation, MoonIR verification and the
container codec still refuse source Ref publication; `source-ref-apply` stays
implementation-open.

The strict build and all 77 non-hardware gates pass (205.65 seconds), as does
Windows ASan/UBSan canonical MoonIR (1/1). WSL Arch Linux Clang 22.1.8
syntax-compiles the changed backend file. Design status, file-guide inventory
and diff checks pass. These are local checks, not remote CI or stable-release
approval.

### Private one-Ref host wrapper prototype (2026-09-28)

An internal LLVM emitter now constructs a complete status-returning wrapper
around a `void` body with one Ref parameter and, optionally, one explicit
leading Fragment execution context. Borrowed ingress checks the live handle;
owned ingress transfers the host carrier into an initially empty private cell,
then takes and clears that cell immediately before passing the sole handle to
the body. A failed check/transfer returns the exact Runtime status without
calling the body or changing the caller's owning carrier. The wrapper remains
internal-linkage and is not put into a runtime descriptor. IR regression
checks the body calls, context forwarding, owner-cell clearing, failure
branches, frozen target and rejection of invalid target/context/name shapes.

This prototype does not itself prove that the callee's CFG treats its Ref
parameter as borrowed or owned and runs the matching cleanup. That proof,
actual source declaration wiring, return ABI and cross-package execution are
required before publication. The MoonIR/backend/container gates remain closed
and `source-ref-apply` remains implementation-open.

The strict build and all 77 non-hardware gates pass (201.50 seconds), as does
Windows ASan/UBSan canonical MoonIR (1/1). WSL Arch Linux Clang 22.1.8
syntax-compiles the changed backend file. Design status, file-guide inventory
and diff checks pass. These are local checks, not remote CI or stable-release
approval.

### Frozen Ref ingress ownership pairing (2026-09-28)

The private one-Ref/unit wrapper no longer accepts a caller-supplied borrow
or owning mode, target, or context flag. Its construction derives these from
one actual module Function declaration. It requires a sealed nominal Ref
target and unit result, an intact frozen Function type and declaration
contract, a matching single canonical parameter, and a separately verified
sealed CFG. A shared borrow must have no parameter cleanup; an affine owner
must have exactly one direct root-scope Drop cleanup. The fragment-context
effect is independently recomputed from the module call graph before its ABI
shape is accepted. The LLVM body symbol and pointer arity must match the
source declaration. Adversarial regression mutates relation, cleanup,
canonical callable type, Ref resource facts and context effect, and checks
that no wrapper is created; a mismatched LLVM body name is also rejected.

This establishes an internal ownership-selection proof, not proof that an
arbitrary supplied LLVM body implements that CFG. The private generated-body
pairing described below addresses the latter gap for two unit-entry shapes;
it does not publish a descriptor. MoonIR/backend/container Ref gates remain
closed. Source import, return, aggregate cleanup and dynamic `apply` remain
open.

The strict build and all 77 non-hardware gates pass on the final code (193.67
seconds), as does Windows ASan/UBSan canonical MoonIR (1/1). WSL Arch Linux
Clang 22.1.8 syntax-compiles the changed backend file. Design status,
file-guide inventory and diff checks pass. These are local checks, not remote
CI or stable-release approval.

### Private generated-body Ref ingress proof (2026-09-28)

On the direct-codegen bypass path, a one-Ref/unit source entry is now lowered
through the real canonical-CFG CodeGenerator into a separate, short-lived LLVM
module. Before lowering, its sealed CFG is independently verified. The frozen
source signature and CFG select the private wrapper mode and nominal target;
the wrapper then calls that exact generated body. The proof requires internal
linkage, one wrapper-to-body call, a generated Ref Drop for an affine owned
parameter and no Ref Drop for a shared borrow, and valid LLVM module IR.
Canonical regression confirms both source entry shapes pass this proof and
that a forged ownership relation fails it.

The temporary module and wrapper are discarded even when the proof succeeds.
The publishing CodeGenerator still returns a blocking diagnostic and cannot
emit this body via JIT or AOT. This is intentionally limited to a single Ref
parameter with a unit result; a function needing other bodies or additional
lowering support may fail the private proof without changing the closed gate.
It is not a source host ABI, descriptor, import, return transport, wire
round-trip, dynamic `apply`, or cross-package execution. `source-ref-apply`
remains implementation-open.

The strict Windows build and all 77 local tests pass, as does the Windows
ASan/UBSan canonical test. WSL Arch Linux Clang 22.1.8 syntax-compiles the
changed backend file. Design-status, file-guide and diff checks pass; these
are not remote CI or release approval.

### Private affine Ref return-body proof (2026-09-28)

A real source `fn transfer(selected: affine RuntimeFragmentRef<S>) -> affine
RuntimeFragmentRef<S> { return selected; }` now reaches a second disposable
CodeGenerator proof. It requires the exact same frozen nominal Ref type on
both sides, owned/affine parameter and result contracts, no hidden runtime
context, an independently verified sealed CFG, and a direct return of its
unique parameter local. The generated internal LLVM body must return a handle
loaded from the parameter carrier, clear that carrier before returning, and
pass LLVM module verification. A forged result ownership usage fails the
proof. The earlier unit-result body/wrapper proof remains separate.

This proves a narrow callee-side move, not a host return ABI. The next private
slice adds a status-bearing output carrier and failure cleanup; cross-package
import, descriptor and callable publication remain absent. Both proof modules
are destroyed and direct codegen still rejects all source Ref publication.
`source-ref-apply` remains implementation-open.

### Private affine Ref return carrier wrapper (2026-09-28)

The direct affine parameter-to-return proof now pairs its actual generated
body with an internal `(source cell, empty output cell) -> status` wrapper.
Before consuming the source, the wrapper rejects null or aliased carrier
addresses and a nonempty output. It then uses the native exact Slot/Contract
transfer into a private cell, takes that owner into the body, and transfers
the returned owner into the host output using the same frozen target. The
return-transfer failure branch drops the owner still held in the private
return cell. The source CFG is restricted to callback-free direct parameter
returns, so it cannot mutate the caller's output between the precheck and
the second transfer; concurrent host carrier mutation remains forbidden.

Pre-entry failures leave source and output unchanged. Once ingress succeeds,
the source is consumed: an unexpected post-body transfer failure cleans up
the returned owner rather than promising rollback of the source. The private
proof requires one generated-body call, two native transfers, one failure
Drop, internal linkage and valid LLVM IR. Structural regression checks the
output guard, both frozen identities, body-to-output carrier flow and invalid
body/contract rejection. This is not a published return ABI or a claim that
an arbitrary supplied LLVM body implements the source CFG. The module is
discarded; MoonIR/backend/container gates still reject source Ref publication.

### Source Ref apply recognition and borrow gate (2026-09-28)

The dated slices through the placement plan below record their state when
landed. The current private execution boundary and remaining publication work
are summarized in the delivery order above and the 2026-09-29 body proof below.

The existing `apply name { ... }` spelling now distinguishes a local
`RuntimeFragmentRef<S>` from a static Fragment. It resolves the exact exported
Slot, rejects environment arguments, and borrows the owner for the lexical
body; repeated apply is allowed while moving the owner inside that body is
rejected. A dynamic binding masks an outer static binding for the same Slot
in frontend analysis. At this checkpoint there was **no executable source
apply**: the canonical region and effect proof did not yet lower a body, and
module publication reported a context-override diagnostic. Later private JIT
proofs execute a restricted body; public operand/context execution,
return/error cleanup and cross-package tests remain completion gates.

The strict full build passes. The parallel non-hardware run passed 76/77;
`luna.repl-smoke` timed out in process-tree cleanup under parallel load and
passed when rerun alone. The final focused analysis, canonical, REPL,
documentation and inventory gates pass 5/5. This is not a source Ref
end-to-end acceptance or a new performance observation.

### Ref-carrying context effect and nested-loan proof (2026-09-28)

The existing sealed-CFG effect fixed point marks both a Ref-parameter function
that directly invokes an exported Slot and a Ref-parameter caller reaching it.
Private unit ingress now checks that independently recomputed effect before
generating any LLVM body; a forged transitive effect is rejected. Frontend
regressions also prove nested shared `apply` loans, release before an owning
return, and rejection of an owner escaping from inside an active loan. At this
checkpoint this validated the machinery surrounding Ref apply, before the
private region override and executable lowering. No public ABI or wire format
changed.
The strict full build and all 77 non-hardware gates pass (156.55 seconds).

### Internal canonical Ref-apply region (2026-09-28)

Source Ref apply now lowers to a structured MoonIR apply and then an in-memory
canonical `Apply` region. A separate `RuntimeRefApplyRecord` associates that
region with its local Ref owner and exact exported Slot. The CFG verifier checks
the region kind and uniqueness, lexical scope, local Ref type, and nominal
Slot/Contract match. Dynamic binding masks an outer static Fragment for that
Slot, so Slot sites in the body become runtime dispatch and participate in the
existing context-effect fixed point. At this checkpoint the record was only a
proof boundary. The sealed module verifier and container encoder still reject
publication, as does the public code generator; the later disposable private
CodeGenerator accepts a restricted region. The frozen Moon Container 0.3
layout and public ABI have not changed. Complete exit/error cleanup,
wire/public ABI design and end-to-end acceptance remain open.

### Compiler-private Ref context bridge (2026-09-28)

The native runtime now offers a compiler-private, unpublished bridge from a
borrowed live Ref handle and explicit parent context to an independently owned
derived context pointer. It validates the exact Slot, preserves the parent's
other bindings, uses the existing immutable snapshot override, and publishes
the output only after all allocations succeed. An owning-cell Drop clears the
cell before releasing the snapshot. Nested derivation and survival after Ref
owner Drop are regression-tested. At this checkpoint LLVM still rejected
Ref-apply CFGs; later private proofs emit restricted source-body entry/exit
code. The public generator still rejects Ref publication, and complete
return/error cleanup, wire format and public ABI remain open. The bridge is
not part of the stable runtime Fragment ABI.

### Canonical Ref-apply context placement plan (2026-09-28)

A wire-neutral compiler analysis now walks each CFG block's Ref-apply region
ancestry. For every successor edge it records contexts to release in
innermost-first order and contexts to construct in outermost-first order;
`return` and `unreachable` terminals are recorded separately. Entry into a
Ref-apply region is accepted only through that region's exact entry block.
The independent CFG verifier runs this analysis, so a forged edge cannot
bypass context construction. Regression covers normal entry/exit, a bypassed
entry, and nested early return with inner-before-outer release obligations.
At this checkpoint this was a placement **plan**, not emitted cleanup. Later
private proofs emit one top-level region and verify outlined Slot context
inheritance and return cleanup. A later private proof checks dispatch failure
Drop before its trap; recoverable failure cleanup remains open. No
container or public ABI change.

### Private LLVM Ref-apply context transition proof (2026-09-29)

Reusable LLVM helpers now emit the compiler-private Ref-to-context derivation
and owning-cell Drop calls with exact Slot/Contract constants. A disposable
internal LLVM function uses a real nested Ref-apply CFG flow plan to prove the
outer-before-inner entry sequence, status checks before context use, cleanup
of the outer context when inner derivation fails, and inner-before-outer Drop
on early return. Invalid helper operands are rejected and the generated module
passes LLVM verification. This function models only context transitions: it
does **not** generate the source body or its outlined RuntimeSlot continuation.
The source module, container encoder and public CodeGenerator gates remain
closed; no stable ABI or wire format is changed. The private bridge symbols are
registered in the explicit JIT runtime symbol map, but no publishable source
module references them yet.

### Private one- or two-region source Ref-apply body (updated 2026-10-04)

A disposable CodeGenerator can now lower the **actual canonical source body**
for one top-level Ref apply and optionally one nested Apply that borrows the
same local Ref or a second borrowed Ref for the same or a different exact Slot.
Each region has one owned context cell. The private proof checks each derivation against
its own parameter carrier and exact Slot/Contract constants. Jump
entry derives from the current parent; Jump exits and returns run cleanup and
Drop each active cell in inner-to-outer order, before the borrowed Ref owner
is released. The private proof maps every CFG entry/exit obligation to the
corresponding LLVM derive/Drop, checks Slot dispatch against the innermost
active cell, and follows every generated continuation callback. Each frame
captures the full owner stack before dispatch; outlined returns release that
stack before escaping. No generated dispatch may remain unverified.
Sequential and nested outlined Slot sites are supported. The proof body
requires at least one RuntimeSlot site. An inner Apply may enter and leave
inside an outlined Slot callback through Jump edges; the callback captures its
Ref carrier, derives a callback-local owner cell and passes the current owner
stack to nested Slot callbacks. The private proof checks the callback copy of
each derive, dispatch, Jump exit and Return, including failure Drops. Non-Jump
context transitions and executable unreachable terminals remain private-codegen
errors. The sole allowed unreachable terminal is the default arm of an
exhaustive two-tag Result Switch with no context-changing incoming edge.
Production ingress proof remains unit-return only; the test-only hook also
accepts `Result<i32, i32>` for a restricted Ref-apply body.
Derivation failure leaves its output cell empty, releases active parent cells,
then traps. A negative Slot dispatch status drops all active cells in reverse
order before trapping, in the source body and in outlined callbacks. The
private proof connects each failure branch to its derive/dispatch and checks
the owner stack and order. This includes a repeated Drop after a continuation
return has already cleared a cell. Recoverable derivation/dispatch failures
still have no source-level return protocol.
The module verifier, container encoder and public CodeGenerator still reject
Ref-bearing publication, so this is not end-to-end executable source support
or a public ABI change.

The canonical regression target additionally has a compile-time-only private
JIT hook. It materializes the already verified source body behind an internal
test wrapper, then executes normal exit, return after Slot dispatch, and return
before dispatch, including two sequential Slot sites and one- and two-level
outlined continuation nesting and returns inside those continuations, plus
two nested Apply regions with normal exit and inner outlined return, against
a real borrowed Ref and an empty parent context. A two-Ref fixture uses
separate descriptors for the same Slot and checks outer-inner-outer dispatch
origin and arguments. A two-Slot fixture checks outer `checkpoint`, inner
`shadow`, and the retained outer `checkpoint` both inside and after the inner
region, including exact Slot/Contract/layout and argument payloads. A second
two-Slot fixture returns from the inner outlined continuation and checks the
reverse owner cleanup path. Callback-local Apply fixtures check ordered Slot
arguments on normal exit and that an inner outlined return escapes after the
active owner stack is dropped. A generation
lease remains pinned during the call and expires after the host releases its
Ref handle or handles, demonstrating that the derived context leaves no
retained pin on those paths. Each body is invoked twice with the same borrowed
Ref handle or handles, checking repeated application without consuming them. This
hook also reads each activation's packed argument to verify sequential sites
dispatch `1`, then `2`, on both invocations. It is absent from the production
compiler target; no public descriptor or container code is emitted.

### Source `?` inside Ref apply: private execution (2026-10-05)

A sealed source fixture now places `?` inside two nested Ref Apply regions.
The generated Result Switch keeps both regions active on its Ok and Err edges;
the Err arm is a Return terminal whose flow plan releases the inner context
before the outer one. The ordinary CFG verifier accepts this path. A private
JIT hook now lowers the actual Result-returning body and exposes its tag and
`i32` payload through a test-only scalar observation value. Two source
functions use local `Ok(9)` and `Err(7)` inputs: Ok dispatches `checkpoint(9)`
and returns `Ok(0)`; Err returns `Err(7)` before dispatch. Both run twice
against a real borrowed Ref. The proof accounts for each derived context and
Drop, and the generation pin expires after the host handle is released.
Additional private JIT fixtures return `Err(7)` from an outlined Slot
continuation and propagate `?` after an earlier Slot dispatch has completed.
The former verifies Result storage and escape through the callback frame; the
latter verifies inner-before-outer cleanup when Err follows a completed
dispatch. Both use a real borrowed Ref and check dispatch count and payload.
One more Err fixture allocates an apply-local struct with a source `Drop`
method before `?`. The test-only JIT imports that exact frozen Drop glue and
an external probe. Each of two calls invokes Drop once with the expected
payload. The private LLVM proof matches the local storage and checks Drop,
`rt_dealloc`, then context Drop in that order. The ASAN canonical target passes.
An additional Err fixture propagates a `Result<i32, SourceError>` through
`From<SourceError> for i32` while the function still returns
`Result<i32, i32>`. The test-only JIT imports only the exact frozen `From`
method referenced by the canonical Err Return. Its private LLVM proof requires
one conversion call before either context Drop. Two executions return
`Err(47)` without Slot dispatch and release the borrowed Ref pin after the
host handle is dropped. The source error is affine, has a source `Drop` method,
and is consumed by the conversion method. The test-only JIT follows its
frozen cleanup table to import that exact Drop glue; a probe observes one
Drop per call. The returned payload remains scalar.
An additional `From<SourceNestedError> for i32` fixture consumes an affine
outer error with one owned `SourceError` field inside the same nested Apply and
`?` path. The conversion has two scalar return branches. The positive branch
moves the whole affine error into a branch-local owner before returning its
marker; the negative branch returns from the original parameter. Each branch
runs twice and returns `Err(43)` without Slot dispatch: the positive branch
observes outer Drop `43` before inner Drop `47`, and the negative branch
observes outer Drop `-43` before inner Drop `47`. The focused canonical ASAN
test passes on Windows Clang64/LLVM 20 and WSL Arch Linux/LLVM 22. This proves
one bounded conditional whole-owner local move and both exits of the two-node
conversion; partial-field or host ownership transfer and wider conversion
bodies remain outside the proof.
A direct two-owned-field source probe now fails at source ownership checking
on `move pair.first`, with the field-move location and a whole-struct move
suggestion. Before this explicit rejection, the source checker tracked the
moved field while return cleanup still named the root owner; MoonIR sealing
then rejected the missing projected cleanup rows. The ordinary struct still
has one root cleanup. A regression fixture pins the early rejection. Before
extending `From` to field transfer, derive disjoint field
cleanup rows plus allocation release, carry the precise remaining obligations
through each terminal branch, and verify their order and single execution in
the sealed CFG and JIT. A whole-owner branch move does not discharge this work.
Another source fixture returns `Result<i32, ReturnedResource>` from the same
nested Ref Apply and `?` shape. The sealed CFG verifies an owned payload move
and inner-before-outer context exits. A narrower test-only JIT wrapper now
observes its resource Err after the source body returns, invokes the exact
frozen Drop method, then calls `rt_dealloc`. Two calls return marker `59`
without Slot dispatch, and the Drop probe fires once per call. The wrapper
now also covers a resource Ok returned after a completed Slot dispatch
(`marker = 67`) and a two-field resource Err whose `marker` is the second
field (`marker = 61`); it reads that field through the frozen product offset.
Each owner is dropped once per call. Scalar Ok and Err branches of those same
Result shapes return `14` and `13` without invoking resource Drop. Each source
Drop method clears `marker` after probing it, so the returned marker also
checks that the wrapper observes the payload before finalization. The
initial test-only wrapper accepts at most two top-level fields: an `i32` marker and
either an `i32` or the bounded nested resource described below. It consumes the owner inside
the JIT module, and hands no resource pointer or ownership carrier to the
host. The canonical ASAN target passes with these fixtures, subject to the
separate intermittent COFF loader failure below.
Further Err fixtures return two- and three-struct ownership chains. After
reading the outer marker, the wrapper uses the compiler's existing recursive
owned-payload cleanup. Its narrow shape gate admits at most three structs,
each with a frozen source Drop and an `i32` marker. The private LLVM check
matches Drop calls from outer to inner, then deallocations from inner to
outer. Two JIT calls observe `71, 73, 71, 73` for the two-struct chain and
`79, 81, 83, 79, 81, 83` for the three-struct chain. The ASAN canonical
target passes. At that checkpoint, longer or branching ownership graphs
remained outside this proof.
The next private fixture admits exactly three owned struct nodes in a bounded
fork: an outer marker and two independently owned fields, each with its own
marker and frozen Drop. The shape gate counts nodes across the whole tree and
rejects a fourth before JIT materialization; a sealed four-node fork is the
negative fixture. The `?` Err return observes `89, 91, 93` in field order on
each of two JIT calls. The private LLVM check also requires outer Drop,
left Drop/deallocation, right Drop/deallocation, then outer deallocation.
The host-transfer experiment repeats this order for both injected post-body
failure cleanup and successful exactly-once host Drop. The focused canonical
test passes on Windows CLANG64 and WSL Arch Linux in ordinary and ASAN builds.
On Windows Clang64 ASAN with LLVM 20.1.8, one of five canonical test
invocations in the previous validation ended during LLVM COFF JIT loading with
`IMAGE_REL_AMD64_ADDR32NB relocation requires an ordered section layout`;
the immediate rerun and three subsequent repeated invocations passed. A
further run after the resource-return CFG fixture also passed. The failure
occurred in the earlier sealing-test stage, before the registered Ref fixtures.
It was a JIT loader failure, not an ASAN memory diagnostic. The default
Windows LLJIT object layer uses RuntimeDyld; the exact triggering JIT call
has not been identified. The canonical test now marks each sealing subtest and
the first three JIT calls so the next failure can be placed more precisely;
12 consecutive ASAN reruns after that instrumentation passed without another
relocation failure. On 2026-10-08, 20 further canonical ASAN runs before the
new nested-conversion fixture also passed without reproducing it. Keep the
ASAN result qualified until the COFF section layout failure is isolated.
An isolated test-target opt-in trial replaced the Windows object layer with
LLVM 20.1.8 JITLink. One ASAN canonical run failed earlier while linking a
compiled-host fixture: `.pdata` to `.text` exceeded a `Pointer32` fixup range.
The trial was reverted; the default RuntimeDyld build then passed the same
canonical test. Directly switching to this JITLink configuration is therefore
not a validated fix for the intermittent RuntimeDyld failure.
The private JIT now also has a separate host-transfer experiment for admitted
resource Result shapes. Its status entry checks nonnull, nonoverlapping tag and
owner output cells and requires an empty owner cell before calling the source
body. The scalar branch writes only the tag; the resource branch writes the tag
and then the owner pointer as its final ownership commit. A separate JIT Drop
entry clears that cell before running the exact frozen recursive Drop and
deallocation sequence. The private LLVM proof requires one body call, permits
cleanup calls only on the injected failure branch, and checks both Drop call
sequences. A three-struct Err
fixture proves that a successfully transferred owner is not Dropped before
host Drop, then observes
`79, 81, 83` exactly once per transferred owner; duplicate Drop fails. A
resource Ok fixture also transfers and Drops its owner once per call. Scalar
Ok and Err counterpart fixtures return a null owner and no resource Drop.
Invalid occupied, aliased
and null output storage fails before body dispatch without changing outputs.
An injected failure after the source body returns a nonnull resource leaves
both outputs untouched and runs the same exact cleanup inside the JIT before
returning status `3`. On the second successful transfer, the host defers Drop,
releases the borrowed Ref handle and observes its generation pin expire with
no extra resource Drop. It then releases the original JIT handle while a
separate shared LLJIT lease keeps the Drop entry executable; host Drop runs
exactly once, after which that lease expires. The raw owner pointer does not
retain JIT code itself. A public carrier must pair owner and code lease so
the Drop thunk remains live through cleanup. This is a test-only protocol,
with no published carrier or symbol contract. It does not establish
production failure statuses or safe JIT teardown with an outstanding owner.
The observation value is not a public return carrier or stable ABI.
More than three owned structs, wider resource graphs, conversion bodies with
conditional partial-field or host ownership transfer or wider ownership
graphs, `?` inside outlined Slot bodies, and recoverable derive/dispatch failures are outside this
execution proof; the source frontend
continues to reject `?` across the outlined Slot boundary.

The current source builder enters and normally leaves each Ref Apply through
Jump edges. It does not produce a source-level non-Jump context transition in
these structured paths. The flow planner computes such obligations for all
edge kinds, while private codegen rejects any non-Jump transition. Retain that
gate until a future source lowering makes such an edge reachable and supplies
an executable cleanup proof.

A separate test-only status entry now composes a read-only parent-context
preflight and the exact native borrowed-Ref check with one sealed
unit-returning source Apply body. It derives the
SlotId/ContractId from that body's frozen parameter target. Two calls using a
real host-created Ref handle dispatch once each; a null handle and a live Ref
for a different exact Slot return distinct private invalid-handle and
invalid-target statuses without entering the body. A null parent context also
fails before the Ref check and body. The private LLVM check requires one
context check, one Ref check and one body call, and checks the exact status
switch cases, destinations and return constants. The compiler-private context check accepts a
live Runtime-created parent, rejects null, and allocates nothing. Both handles
retain their generation until
released, and the JIT lease covers the synchronous calls. This does not use
`emitRuntimeFragmentRefUnitIngressWrapper`: that general helper deliberately
rejects Apply regions. The composition is confined to the test build, and
the production CodeGenerator and module verifier still reject publication.
The native check reads a handle created by the runtime; it cannot validate an
arbitrary or stale pointer; the same applies to the context preflight. The
private status profile maps success/context/handle/target/unexpected check to
`0/1/2/3/4`. Only exact known native results receive their named status;
other check results use the unexpected branch and cannot enter the body.
These numbers are not a public entry status contract, and the body has no
recoverable execution-failure status. Publication still needs a public status
ABI, symbol and descriptor mapping, and a code lease held for the full call.
Before this test entry is generated, the private proof cross-checks the sealed
Function declaration and callable TypeId/linkage/ownership contracts against
its source signature, CFG root parameter and first exact Apply target.
Mutating the frozen linkage or parameter borrow contract makes JIT
materialization fail. The test hook now also emits a pointer-free candidate
row: independent magic, version, total size, zero reserved word, fixed field
counts, bounded length-prefixed module/package/function/Ref/Slot/entry
identities, and explicit calling, borrow, result and private status-profile
codes. It now also carries an explicit context-effect word derived from the
fixed-point analysis of the exact function; both a false record word and a
forged source effect fail. Its validator rechecks the sealed CFG and frozen
facts. Tests reject
truncation, extra bytes, altered identity or convention fields, changed source
facts and a generic Native v1 export row. This row is test-only; no public
descriptor, export digest or loader consumes it.
The test-only loaded entry view revalidates this row against the frozen source
and against the JIT module that emitted it before looking up the fixed ingress
symbol. An otherwise valid row paired with a JIT module materialized without
that row is rejected. The view holds the code lease and copies it for each
synchronous call: the fixture releases its original JIT reference, calls the
entry twice, then releases the view and observes the JIT lease expire. Both
Runtime-created borrowed owners remain live during those calls. A third call
uses a private duplicate handle shell sharing the original immutable Ref
snapshot and a copied parent context. The fixture releases the original Ref
and parent first, then observes the snapshot pin keep the generation alive
through that call and release it afterward. Duplication starts from a live
typed handle and rejects an occupied output; it does not accept arbitrary or
stale opaque pointers. This is an in-process provenance and lifetime proof,
not a verified artifact loader or public Runtime lookup.
On 2026-10-05 the focused canonical test passed in the strict CLANG64 and
Windows ASan/UBSan builds for the loaded-entry view; the production `luna`
target built, design-status and file-guide inventory gates passed, and
`git diff --check` found no errors. The later borrowed-owner pin extension
has its own validation below.
The borrowed-owner pin extension passes the strict CLANG64 and Windows
ASan/UBSan canonical tests; the production `luna` target and both documentation
gates build/pass with the test-only helper excluded from the production ABI.
This local slice does not constitute a full-suite or cross-platform run.

#### Candidate borrowed Ref/unit host entry contract (design only)

The smallest publishable profile would use one synchronous entry with the
logical shape `status(parent_context, borrowed_ref) -> i32`. Its parameter is
`shared borrow RuntimeFragmentRef<S>` and its source result is `unit`. The
host must keep both Runtime-created owners live throughout the call; no Ref
ownership moves to the body. A pinned callable returned by lookup must keep
the descriptor and machine code live until the call completes. The entry
must check parent and exact Ref target before body execution. The private
`0/1/2/3/4` profile is evidence that preflight failures can be separated,
not the public status ABI.

| Candidate contract fact | Required check before publication |
| --- | --- |
| Independent magic, ABI version, struct size, zeroed reserved fields | Reject unknown layout or version before reading any entry fields. |
| Function SymbolId, ContractId, TypeId, linkage name and owning module identity from its verified registry | Match one sealed exported function and the verified artifact; do not infer the ABI from a raw symbol address. |
| Exact target SlotId and Slot ContractId | Match the frozen `RuntimeFragmentRef<S>` target and the source parameter type. |
| Calling convention, two pointer arguments in parent/Ref order, shared-borrow mode, unit result and versioned status domain | Match the generated wrapper and independently distinguish context, handle, target and execution failure. |
| Loaded entry view: entry pointer and generation identity | Obtain only through a verified, pinned module lookup; retain the code lease across the synchronous call. |

The existing generic Native export and Runtime declaration descriptors do
not encode the Ref target or this ingress ABI. The Fragment descriptor
describes factory/environment/activation execution, so it cannot stand in
for a source function entry. Decide how the new typed record is attached to
verified Moon/Native exports before changing production publication. The
Moon Container 0.3 encoder still rejects Apply regions, and the module
verifier and CodeGenerator still reject source Ref publication.

The present Native verification path fixes the integration boundary:
`buildNativeLibrary` forms each `NativeExportSpec` from the sealed declaration
table, `canonicalNativeExport` serializes only kind, flags, SymbolId, ContractId
and linkage for the proof export digest, and `emitNativeLibraryDescriptor`
emits the corresponding v1 rows. On load, `validateNativeDescriptor` requires
the exact v1 row size and recomputes that digest before the exports enter a
generation. Therefore a Ref target or ingress convention attached only to a
runtime binding would be unproved. Do not extend a v1 row in place. A new
versioned typed row must derive from the verified frozen function signature
and CFG, enter the canonical export digest and generated descriptor together,
and be checked for field validity and identity ambiguity by the loader.
The existing artifact digest and trust record then bind the rebuilt image and
its export digest. `stageVerifiedNativeGeneration` currently copies only
symbol/contract/entry/kind/flags into `GenerationBinding`; typed facts must
also reach the pinned lookup result before a host can call the entry. Its
generation already retains the verified library lease, but a caller must keep
the `PinnedBinding` alive for the complete synchronous call.

On 2026-10-06 an actual `-t native` build of an exported Runtime Slot caller
failed at the MoonIR verifier with the missing runtime-aware public entry ABI
diagnostic. Semantic and Native artifact regressions now pin that failure;
the Native test also requires no library or trust record to be produced. The
existing proof-v1 callable test covers a no-argument `i32` function and cannot
validate Ref/unit ingress. This guard stays in place while the versioned typed
export path is designed.
The current CLANG64 worktree then rebuilt and passed all 77 local non-hardware
CTest cases with four workers in 71.18 seconds on 2026-10-06; this is broader
regression evidence, not a verified Native Ref/unit export or release approval.
An isolated WSL Arch Linux build with Clang/LLVM 22.1.8 compiled all targets
and passed the complete local Linux CTest suite 76/76 with four workers in
67.15 seconds. The initial 75/76 run failed only the frozen ecosystem
baseline: WSL Git lacked the Windows system `core.autocrlf=true` setting and
misread the child worktrees' CRLF checkout as dirty. Both were clean under
that setting; a process-local Git override made the failed test and complete
rerun pass. This is local Linux evidence, not remote platform CI.

#### Native typed export boundary audit (2026-10-06)

The current Native v1 export row contains declaration kind, callable flag,
SymbolId, ContractId, linkage name and a raw entry address. Its canonical
export digest covers the pointer-free fields; the artifact digest binds the
binary bytes, and the loader validates row structure and identity. However,
neither path carries a callable signature, a Ref target or a Fragment-context
effect. `stageVerifiedNativeGeneration` passes the row on as an untyped
`GenerationBinding`. The code generator currently places the generated function
body, rather than a Ref host wrapper, at a callable row's entry address. The
structured verifier correctly rejects a context-dependent exported function,
and the Native fixture requires this rejection before any library or trust
record is emitted.

The private two-argument `i32(parent_context, borrowed_ref)` wrapper and its
test-only status profile cannot be exposed through that v1 row. Its pointer-free
record checks frozen signature/Slot identities and the context effect, while
the private loader requires identical record bytes in the retained JIT module.
The record does not include a CFG or code digest; full body identity across
separately built artifacts is not established by that record alone.

The next independently reviewable slice is a versioned typed export contract
with a distinct entry ABI profile. Define the exact two-argument calling
convention, status values, parent-context and borrowed-handle lifetimes, Ref
target identity and effect fields before emitting a callable descriptor.
Adding a profile bit to a v1 callable row alone is insufficient: current host
callers check `CALLABLE` and cast the raw address as `i32()`; a Ref entry must
be reachable only through a lookup that enforces its exact profile.
Bind all of those fields to the canonical export proof and verified wrapper;
make loader validation and a pinned typed lookup reject missing, mixed-version
or mismatched fields. Keep Native v1 generic entries and the source Ref export
gate closed until that complete path and the Container wire rules pass.

The Native v1 descriptor emitter now independently checks each requested row
against the generated module's public export table and declaration identities:
exact SymbolId/ContractId, declaration
kind, linkage and callable flag before creating its registry. It also rejects
a callable whose source declaration requires a Fragment context, even if an
upstream caller bypasses the structured verifier. A canonical regression
supplies a generated but unpublished `main`, then forges an export and context
effect after code generation; both attempts are rejected. Ordinary Native
artifact generation still passes. Focused canonical and Native artifact tests
pass on Windows CLANG64 and WSL Arch Linux; the complete Linux CTest rerun
passes 76/76 with four workers in 75.62 seconds. This is a v1 fail-closed
guard, not a typed Ref entry or release-candidate CI.

The Native artifact gate now also builds a package with an exported
`RuntimeFragmentRef<Slot>` parameter. It requires the MoonIR Ref wire/ingress
diagnostic and verifies that no shared library or trust record appears. This
tests source Ref publication separately from the context-dependent Slot entry
case; the verifier currently rejects it before the secondary codegen guard.
The focused Native artifact CTest passes on Windows CLANG64 and WSL Arch Linux.

Before a Native v1 callable address is emitted, the descriptor generator now
also compares its frozen Function parameter/result TypeIds with the source
declaration and the actual defined LLVM function type and C calling convention.
A canonical regression changes both the source parameter and frozen function
type after code generation; the unchanged LLVM body cannot be published under
the forged signature. Canonical and Native artifact CTests pass on Windows
CLANG64 and WSL Arch Linux. The complete Linux CTest rerun passes 76/76 with
four workers in 74.36 seconds. This checks compiler-internal consistency; it
does not tell an external v1 caller which signature to use.

The first **parallel Native v2 descriptor/query** slice is implemented for
verified no-argument `i32` exports. V1 rows, query and proof export digest
remain unchanged for existing readers. The producer adds a
`C_I32_NOARGS_V1` row only after checking the sealed source signature and the
defined LLVM `i32()` C-callable function. The loader validates the v2 header,
identity, row profile, canonical row digest, and exact correspondence with a
verified v1 row. The existing artifact proof digest binds all v2 metadata.
`VerifiedNativeLibrary::callI32NoArgs` invokes the typed entry while its
owning library remains loaded; no new raw pointer escapes. The independent
consumer checks both queries and digests, and a resealed v2-digest mutation
is rejected by the loader on Windows CLANG64 and WSL Arch Linux. Native
generation resolution now copies only validated v2 profiles into the separate
`GenerationBinding.entryAbi` field. A profiled requirement finds the exact
entry; `PinnedBinding::callI32NoArgs` calls while its generation retains the
verified library lease. Load-once, activation and switchable requirements
check profile stability. Four-field legacy requirements still match by
identity/kind/flags, but an explicit unprofiled requirement cannot match a
profiled export. An earlier synthetic test image with its v2 query symbol
renamed failed on macOS (consumer exit 24) and was removed; it depended on
rewriting bytes in a linked library. A separate C shared library built from
v1-only source, with its own sealed proof/trust record, also passes the
independent proof oracle, v1 loader/call and unprofiled generation checks;
its typed lookup is rejected because it has no v2 query. This independent
fixture now covers the optional query path on all three 64-bit CI platforms.
This is an implemented ABI experiment, not a frozen public ABI or a Ref
wrapper export.
The complete local CLANG64 and WSL Arch Linux CTest suites passed 77/77 and
76/76 respectively after this integration, before the three compile-only
layout tests were registered.

The candidate below records the current profile layout, digest and version
rules. The existing release packages and CI matrix target 64-bit Linux,
Windows and the macOS runner architecture; no 32-bit release target is listed.
The [release state](ecosystem_release.md) records successful Linux, Windows,
and macOS CI on corrected implementation commit `8aac3a0`, including the
Native artifact test. Before a public ABI promise, review the next-query rule
and host pointer-lifetime obligations with host users.
Keep source Ref publication gated until its context effect, exact Slot/Contract
target, carrier status and ownership semantics are sealed and verified end to
end.

#### Native v2 local layout and version candidate (2026-10-06)

The current 64-bit target matrix pins natural C layout: v1 export/library
records are 48/64 bytes; v2 export/library records are 56/96 bytes. In a v2
row, `entry_abi`, `symbol_id` and `entry` start at offsets 16, 24 and 48;
in a v2 library, `export_count`, `exports` and the 32-byte descriptor digest
start at 48, 56 and 64. The C header asserts these values; the independent
consumer checks them. A freestanding compile-only probe of i386 GNU/Linux,
i686 Windows GNU and i386 Darwin shows 32-bit v1 export size 32 and v2 export
size 40; v2 row `entry_abi`, `symbol_id` and `entry` offsets are 16, 24 and
36. The v2 library `export_count`, `exports` and digest offsets are 32, 40
and 44. Library tail alignment differs: v1/v2 sizes are 44/76 on the probed
GNU/Linux and Darwin targets, 48/80 on Windows. The repeatable probe is
`tests/native_abi_layout_probe.c`; Clang CTest compiles it for all three
triples on each configured host without a 32-bit linker or runtime. The
three layout CTests and file-guide inventory pass on local Windows CLANG64
and WSL Arch Linux. After registering them, the complete non-hardware CTest
suites pass 80/80 on Windows CLANG64 and 79/79 on WSL Arch Linux.
These observations do not establish a 32-bit artifact producer, loader,
execution path or release promise. Adding a 32-bit target requires separate
end-to-end artifact and runtime CI plus an explicit ABI/version decision.

V1 proof and query remain required and unchanged. The v2 query is optional:
absence yields unprofiled v1 bindings; presence requires exact ABI 2,
structure sizes, zero reserved fields, matching proof identity and a v2 row
subset matching verified v1 identities, flags, linkage and entry addresses.
Same-version tail extensions are rejected; a larger record needs a new query
and schema version.
Schema 2 recognizes only profile `C_I32_NOARGS_V1 = 1`, meaning a defined C
calling-convention function `int32_t(void)`. Any unknown profile or malformed
v2 row rejects the whole loaded image; it never falls back to v1.

The v2 descriptor digest is SHA-256 over a little-endian `u32` row count,
followed by sorted, unique rows as `u32` byte length plus exact bytes. A row
is `LUNA_NATIVE_EXPORT_V2\n` followed by decimal kind, flags and entry ABI,
then SymbolId, ContractId and linkage, separated by `\n` with no trailing
separator. Pointers are not canonical row bytes; the v1 whole-artifact proof
digest binds the linked image containing them. A changed row encoding,
digest algorithm or new profile needs a new parallel query/schema version,
because existing v2 loaders reject unknown fields and profiles.
The loader requires bounded, nonempty UTF-8 descriptor strings without CR,
LF or tab in both v1 and v2. A separately sealed v1 fixture with an invalid
UTF-8 symbol verifies offline but is rejected by the host loader.
The independent C fixture also produces a v2 query. Its sealed valid form
supports a typed host call; an independently sealed oversized v2 row passes
offline proof verification but fails host loading. This exercises the query
and exact-size rule without relying on Luna's LLVM emitter.
The same v2 fixture calls through a pinned typed binding after `MoonRuntime`
has been destroyed, checking that the generation's module lease keeps code
alive independently of the runtime object.

Within MoonRuntime, binding profile 0 means unprofiled and profile 1 enables
the pinned `i32()` call. `GenerationEntryAbiAny` is a requirement-only wildcard
that keeps existing four-field callers source-compatible; explicit 0 or 1
matches exactly. A switchable binding captures the active concrete profile
and refuses a later generation that changes it. These rules describe a local
candidate and do not publish a stable external ABI.

The private typed-row encoder and validator above close only the local
source-to-record check. Moon Container's Function code record already carries
`requiresFragmentContext`, and the module verifier recomputes it from the
CFG, but the Container encoder still rejects Apply graphs. The frozen
`DeclarationRecord`/canonical contract and Native v1 export row omit the
effect; the structured verifier also rejects an exported function that
requires a Fragment context. The next boundary is therefore a public status
mapping and a coordinated verifier/export rule for a context-dependent
exported source function. Its effect and Ref target must enter sealed export
metadata. Then extend the v2 profile, canonical hashing, loader validation
and pinned Runtime lookup together, while keeping generic v1 rows untyped.

### Next source Ref/apply gates (updated 2026-10-08)

This is the proposed implementation order for the remaining source feature,
not a public ABI decision or stable-release approval:

1. **Close control-flow and cleanup proofs.** The private two-region Jump
   path now has owner-stack and JIT evidence for one or two borrowed Refs,
   with same or different exact Slots, including an inner Apply in an outlined
   callback. The source builder currently makes Apply entry and normal exit
   Jump transitions; retain the explicit rejection of non-Jump cross-region
   edges until source lowering produces one. The narrow `Result<i32, i32>`
   `?` path now has an inner-before-outer CFG proof and private Ok/Err JIT
   evidence, including one affine source error converted to a scalar by its
   frozen `From` method and cleaned by source `Drop`. A two-node owned
   conversion parameter now also proves outer-before-inner Drop on both scalar
   return branches in Windows and Linux ASAN JIT runs, including a whole-owner
   move to a branch-local binding on the positive path. Partial-field or host
   ownership transfer and wider conversions still need proof. The next
   partial-field step requires projected canonical cleanup rows and
   field-sensitive return obligations before a positive JIT fixture.
   One- and two-field resource Err and one-field
   resource Ok now have a private wrapper that observes and destroys the
   returned owner after ordered context exits; scalar counterpart branches
   do not Drop. Up to three structs in one ownership chain now have recursive
   cleanup and ordered Drop/deallocation proof. A bounded three-node fork with
   two independently owned fields now has ordered `?` Err and injected-failure
   cleanup evidence on both local platforms, including focused ASAN runs.
   Larger or wider owned-field graphs remain outside the proof. A test-only host-transfer entry now proves
   the empty owner-cell preflight, tag/owner commit, a separately retained
   JIT lease after the borrowed Ref pin expires, and explicit exactly-once
   Drop for the three-struct chain. A public host
   ownership carrier and production post-body failure statuses still need a
   contract and executable proof; the private injected failure now proves
   cleanup of an uncommitted returned owner.
   Specify recoverable derivation/dispatch
   failure cleanup before publishing a path that can return those failures.
   Every admitted exit must release contexts in
   inner-to-outer order exactly once, after apply-local cleanup and before a
   borrowed Ref owner is dropped. Keep unsupported paths rejected meanwhile.
   Independently capture the exact JIT call for the intermittent Windows LLVM
   20.1.8 RuntimeDyld COFF relocation failure. The direct JITLink trial hit a
   different `.pdata` `Pointer32` range failure; next test an ordered-section
   allocator or newer LLVM in an isolated build, then repeat ASAN runs before
   treating sanitizer results as stable evidence.
2. **Freeze a publishable host boundary.** Specify a versioned Ref ingress
   carrier, return carrier/status, exact Slot/Contract validation, borrowed
   lifetime, ownership commit point and failure cleanup. Private wrappers and
   the current opaque internal type are evidence, not that contract. The
   smallest candidate is one synchronous shared-borrow Ref parameter and a
   unit result, with a status-returning wrapper that validates the exact Slot
   and live parent context before entering the body. The test-only composition
   now executes both checks against live correct/wrong-target handles and a
   null parent;
   the general ingress helper still excludes Apply regions. Publication must
   freeze the candidate typed record and status version, attach it to a
   verified export, and carry the call-duration code lease through the public
   lookup path, then update
   the structured verifier,
   export symbol metadata, CodeGenerator and dropGlue together; the existing
   check/transfer/drop tests cover native carrier behavior but do not prove a
   published source entry.
   The separate test-only tag/owner-cell and JIT Drop entries now exercise the
   candidate commit and lease rule. Before publishing a resource return
   carrier, make the owner and JIT lease one lifetime unit; define status and
   layout versioning, teardown behavior, and the
   real failure conditions that use this precommit cleanup. The injected
   post-body failure has an executable cleanup proof, but this experiment
   does not publish an ABI.
3. **Connect the verified artifact path.** Implement complete compiler
   dropGlue and production codegen for the admitted flow subset, then define
   and validate Ref type/Apply-region wire rules in the module verifier and
   Moon Container encoder/decoder. Invalid or older artifacts must fail
   without publishing a partial generation.
4. **Pass the two-package execution gate.** Import a host-created, pinned Ref
   from a verified generation; run source apply against an explicit parent
   context; exercise borrowed ingress first, then owning parameter/return
   transfer. Include nominal/contract mismatch, failed ingress, normal and
   early exits, repeated apply and cleanup/pin-release regressions. Only after
   this gate and the remaining control-flow cases pass may
   `source-ref-apply` move out of `implementation-open`.

Performance acceptance, durable evidence storage and stable-release approval
remain separate gates in the v1 acceptance snapshot.

## Host-controlled discovery and injection

An exported Fragment targeting an exported Slot is a candidate by nominal
relationship, not because metadata grants it a capability. A verified artifact
publishes a data-only `FragmentOffer` containing at least its FragmentId,
target SlotId and ContractId, execution/factory contract, environment layout,
generation identity, and retained policy metadata.

The implemented v1 query is scoped to one explicitly supplied, verified
generation and an exact Slot/Contract requirement:

```text
snapshotRuntimeFragmentCandidates(generation, slot, ...) -> pinned snapshot
```

Its result is complete and deterministic for that generation, immutable and
generation-pinned. The host tracks loaded generations and may merge their
snapshots under its own policy. A cross-generation query or global index is
not part of v1; a future convenience API could be considered separately.

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
Verified container coverage now separately checks local public Slots: the codec
retains declaration/contract/export rows but does not reconstruct frontend
`SlotDecl` objects. The verifier therefore accepts an exact local Slot export
without that transient object, while retaining executable-declaration checks
when one exists. Missing, wrong-kind or wrong-contract publication rows and
foreign import/re-export attempts remain rejected. A full encode/decode/load
test exercises generated Copy factories, None/One, resume/discard, capture
writeback and return escape after Runtime teardown.
The source-to-JIT cross-package test above is distinct from artifact loading.
The verified-container gap is now covered by a separate small host/plugin
workspace: both packages encode into independent containers and load through
the verified generation adapter, not direct JIT registration. Persistent Slot
publication remains in the owner's root `Exports`. Fully verified owner decode
issues an immutable `localSlotPublication` evidence snapshot; consumer decode,
stage and load-once accept explicit `SlotPublicationDependencies`. Evidence checks
owner/direct import, target/layout, exact identity, structural type and argument
layout. Source composites use transient compiler-owned evidence while checking
the concrete projection; this is not serialized into consumer exports. Decode
without independently verified owner evidence still fails closed. Null/duplicate,
private-owner, changed-contract, unrelated-owner and other-target evidence are
rejected without partial output or initialization; load-once cache hits cannot
bypass verification. Byte-identical consumer re-encoding demonstrates that this
path changes neither wire format, ContractId encoding nor Runtime ABI. An owner
evidence snapshot never aliases mutable decoded exports, and consumers cannot
attest their imported Slots as local publications.
Formal `luna build <package> -t moon` packaging is also covered. The complete
target-bound roundtrip now runs inside `encodeContainer`, while the verified
source projection's dependency facts are available; those transient facts are
not returned to callers. The CLI does not perform a second context-free decode.
The CLI gate emits each host/plugin package twice into real files and compares
their hashes. Artifact consumers still need independently decoded owner evidence.

```sh
luna build tests/fixtures/runtime_fragment_container/host -t moon -o build/fragment-host.moon
luna build tests/fixtures/runtime_fragment_container/plugin -t moon -o build/fragment-plugin.moon
```

The test makes 64 real host/consumer None/One calls after Runtime teardown.
This is functional artifact evidence, not a latency benchmark or release gate
approval. The compiled-plugin comparison probe below uses these explicit owner
artifacts, with setup/discovery outside dispatch timers. Artifact
trust/authentication, dependency retrieval and candidate-selection policy remain
host responsibilities; no implicit loading, activation or hot-path catalog is added.
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

This is the historical fixed-order protocol. Do not compare its absolute times
directly with the interleaved protocol below: fixture organization and compiled
probe code differ, even though Runtime implementation semantics are unchanged.

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

#### Interleaved observation protocol

The separate `--interleaved [iterations] [rounds]` mode defaults to 10000 measured
iterations and 30 rounds. It prebuilds independent 4-/64-/256-row fixtures in one
process, including all five execution contexts, before starting any sample timer.
Each round measures all 30 catalog/case pairs; round `r` (zero-based) maps temporal
position `p` to `(p + 7*r) % 30`. Every pair occupies each position exactly once
over 30 rounds. Multiples of 30 are position-balanced; shorter runs explicitly
report `position_balanced=no`, rather than claiming equivalent evidence.

```sh
cmake --build build-perf --config Release --target runtime-fragment-benchmark
cmake -DLUNA_FRAGMENT_BENCHMARK_EXECUTABLE="$PWD/build-perf/runtime-fragment-benchmark" \
  -DLUNA_FRAGMENT_BENCHMARK_ITERATIONS=10000 \
  -DLUNA_FRAGMENT_BENCHMARK_RECORD="$PWD/build-perf/fragment-cost-interleaved.csv" \
  -P tests/runtime_fragment_benchmark.cmake
```

Adjust the executable path for Windows or multi-config builds. The script defaults
to only three iterations for a fast protocol check when the iterations argument
is omitted. It independently verifies the 900-row schedule, source SHA-256,
metadata, exact counters (including warmup), partial-round labeling, and strict
CLI rejection. A record is written only after all checks pass. CSV comments contain
the build HEAD, probe source digest, build type, compiler, C++ dialect, and sampling
configuration; HEAD alone is not a clean-worktree or complete Runtime provenance
claim. Samples include round and position so consumers can inspect temporal effects.
Warmup remains `min(iterations, 1000)` per sample; fixture setup, CSV output, and
aggregate checks are outside timers, while native handler/per-call checks remain
inside them. CPU affinity, power policy, and background activity are uncontrolled
and explicitly labeled. Interleaving reduces ordering confounds, not all confounds.

Linux C++17/C++23, macOS, and Windows CI use 10000 iterations and 30 rounds. They
retain validated CSV artifacts as `fragment-cost-*` for 14 days; no ns/op value is
used for pass/fail. These are observations from shared runners, not performance
acceptance. A stable evidence archive, controlled affinity/power experiments,
replicated independent runs, and compiled-plugin workloads remain release work.

Local interleaved snapshot: the same Windows/Clang 20.1.8 machine described above,
Runtime implementation `1663a0b05b16506702ea467cd1cca0c8b8a26e25`, probe source SHA-256
`2510865763f7e27424d677fcc35c0d8ca429a8db741acbfa4c2dd76bbb6e71f6`.
One process, 30 samples per cell, 10000 measured + 1000 warmup calls per sample;
all 900 samples passed schedule and counter checks. Median ns/op (min–max):

| Case | 4 rows | 64 rows | 256 rows |
| --- | --- | --- | --- |
| candidate_snapshot | 643.55 (609.9–737.5) | 6834.65 (6595.7–7366.3) | 26379.90 (25241.8–29402.8) |
| dispatch_none | 216.15 (202.7–673.7) | 214.15 (201.8–462.2) | 216.10 (202.8–254.8) |
| dispatch_one | 505.95 (472.6–948.5) | 499.45 (477.4–716.3) | 511.80 (472.5–900.9) |
| dispatch_chain_2 | 730.05 (675.2–1014.7) | 718.35 (664.5–1084.7) | 727.95 (684.4–781.7) |
| dispatch_chain_4 | 1223.90 (1137.9–1342.2) | 1200.10 (1077.7–1388.3) | 1206.10 (1085.2–1406.3) |
| dispatch_override_none | 217.15 (202.9–292.6) | 218.10 (201.8–246.6) | 215.70 (202.1–307.4) |

This run did not reproduce the historical catalog-associated median growth in
chain/override dispatch. It still has outliers and does not isolate their cause.
Because the fixture organization, iteration count, and compiled probe also changed,
do not attribute the difference solely to ordering or claim a Runtime speedup.

#### Measurement-thread affinity

The native probe additionally exposes `--affinity-info` (read-only) and
`--pinned-thread CPU [iterations] [rounds]`. CPU is a canonical unsigned decimal
logical index, chosen explicitly from the reported allowed set, not a physical
core identifier or automatic topology policy. The defaults remain 10000/30.
The pinned mode now uses `luna.fragment-cost.pinned-thread.v2`, binding the shared
`benchmarks/fragment_thread_affinity.h` controller with `affinity_control_sha256`
as well as the probe's own source digest. Earlier pinned v1 records are retained
as historical observations without retroactively claiming controller identity.
Default interleaved v1 and uncontrolled compiled v2/bundle/evidence formats are unchanged.

```sh
./build-perf/runtime-fragment-benchmark --affinity-info
# Replace 0 with a CPU in the reported allowed set; use a fresh output path.
cmake -Werror=dev -DLUNA_FRAGMENT_BENCHMARK_EXECUTABLE="$PWD/build-perf/runtime-fragment-benchmark" \
  -DLUNA_FRAGMENT_BENCHMARK_LOGICAL_CPU=0 \
  -DLUNA_FRAGMENT_BENCHMARK_ITERATIONS=10000 \
  -DLUNA_FRAGMENT_BENCHMARK_RECORD="$PWD/build-perf/fragment-cost-pinned-thread.csv" \
  -P tests/runtime_fragment_benchmark.cmake
```

Only the new probe's measurement thread is constrained. Windows uses
[thread affinity](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setthreadaffinitymask)
and accepts only a single processor group (group 0); Linux uses
[calling-thread affinity](https://man7.org/linux/man-pages/man2/sched_setaffinity.2.html)
with a fixed `CPU_SETSIZE` mask. Larger kernel masks, disallowed CPUs, syscall
failures and unsupported platforms fail closed, never falling back to an
uncontrolled run. macOS and multi-group Windows explicitly report unsupported.
Neither the caller process nor global power, priority or scheduler policy is changed.
The mask and current CPU are checked after binding and before/after every sample,
outside the timer. Metadata says `affinity=measurement_thread` and
`verified=sample_boundaries`, with the requested CPU/group and power policy still
`uncontrolled`; this is not proof against an external change inside a sample.

Platform CI runs `tests/runtime_fragment_affinity.cmake` as a separate correctness
smoke: read-only capability query, the first allowed CPU with three iterations,
900 schedule/counter checks, and disallowed/unsupported CPU rejection. This
choice is only a smoke policy, not a performance-acceptance CPU selection. No pinned
native CSV is added to the artifact; the separate compiled series below is retained
for 14 days. Independent-process measurements,
frequency/power/background controls, topology documentation and durable archival
still need their own evidence. Boundary checks can affect cache/scheduling between
samples; pinned and uncontrolled records must not be treated as identical harnesses
or evidence of a Runtime speedup. No production Runtime or source-language API changes.

#### Compiled dispatch-thread affinity

The compiled probe reuses that controller through read-only
`--compiled-fragment-affinity-info` and opt-in
`--compiled-fragment-cost-pinned-thread CPU [iterations] [rounds] [O0|O2|O3]`.
Defaults are 10000/9/O0. Invalid/unsupported/disallowed CPU requests fail before
fixture compilation; after compiling, verified loading, binding and 320 correctness
checks, the probe rechecks the allowed set and pins the dispatch measurement thread.
Existing ORC workers are not pinned by the probe. The main thread's mask and
current CPU are verified before/after each sample, outside the timer.

```sh
./build-perf/moonir-canonical-test --compiled-fragment-affinity-info
# Choose an allowed logical CPU and a fresh output path.
cmake -Werror=dev -DLUNA_COMPILED_PROBE_EXECUTABLE="$PWD/build-perf/moonir-canonical-test" \
  -DLUNA_COMPILED_PROBE_LOGICAL_CPU=0 -DLUNA_COMPILED_PROBE_PROFILE=O2 \
  -DLUNA_COMPILED_PROBE_ITERATIONS=10000 \
  -DLUNA_COMPILED_PROBE_RECORD="$PWD/build-perf/compiled-pinned-O2.csv" \
  -P tests/compiled_fragment_benchmark.cmake
```

The separate `luna.compiled-fragment-cost.pinned-thread.v1` raw-record protocol
adds controller SHA-256, CPU/group/boundary-verification metadata, and
`setup_affinity=uncontrolled,measurement_scope=dispatch_samples`. Setup timings
are **not pinned observations**. The new pure `compiled_fragment_pinned_protocol.cmake`
requires these exact identities before reusing the unchanged 81-sample schedule,
calls/checksum and build/workload checks. Derived validation strings never replace
or relabel observed bytes. Pinned and default readers reject the other mode.
The shared affinity smoke additionally runs three-iteration O0/O2/O3 dispatch
checks (243 samples), a second allowed CPU when available, and explicit rejection
on unsupported platforms. CMake 4.4 child invocations use the author-warning spelling.

The existing v1 series, default bundle entry point, evidence exporter and default
summary mode accept only uncontrolled compiled v2. Pinned raw records are not added to
their inventories. The separate pinned series below is not a portable evidence
export or an input to the default summary mode. Do not place these CSVs inside an
existing closed bundle. Default CTest adds only
synthetic record checks, never thread pinning or a timing threshold. Frequency,
power, background load, durable storage and performance/release approval remain unclaimed.

#### Pinned series and offline acceptance

`tests/compiled_fragment_pinned_series.cmake` requires an explicit
`LUNA_COMPILED_PROBE_LOGICAL_CPU` and fresh `LUNA_COMPILED_PINNED_SERIES_OUTPUT_DIR`.
It reuses the six profile permutations, cycle rotation and within-block position/
directed-neighbor checks. Each cycle has 18 fresh measured processes, six per
profile, with nine position-balanced rounds each: 1458 samples. No capability,
default or invalid-CLI smoke subprocesses interleave these records. Windows/Linux
are supported; unsupported platforms, disallowed CPUs and process failures do
not fall back to uncontrolled observations. The directory is created only after
all processes validate, and the manifest is written last.

```sh
cmake -Werror=dev -DLUNA_COMPILED_PROBE_EXECUTABLE="$PWD/build-perf/moonir-canonical-test" \
  -DLUNA_COMPILED_PROBE_LOGICAL_CPU=0 -DLUNA_COMPILED_PROBE_ITERATIONS=10000 \
  -DLUNA_COMPILED_SERIES_CYCLES=1 \
  -DLUNA_COMPILED_PINNED_SERIES_OUTPUT_DIR="$PWD/build-perf/compiled-pinned-series" \
  -P tests/compiled_fragment_pinned_series.cmake
cmake -Werror=dev -DLUNA_COMPILED_PINNED_BUNDLE_DIR="$PWD/build-perf/compiled-pinned-series" \
  -DLUNA_COMPILED_PINNED_BUNDLE_EXPECTED_COMMIT=<full-build-commit> \
  -DLUNA_COMPILED_PINNED_BUNDLE_EXPECTED_MANIFEST_SHA256=<manifest-byte-sha256> \
  -P tests/compiled_fragment_pinned_bundle.cmake
```

Choose a reported allowed CPU, adjust Windows/multi-config paths, and use a fresh
output directory. The standalone sampler defaults to three iterations when omitted;
the explicit CI observation policy uses 10000, one cycle, and the first allowed CPU
selected **outside** the sampler. This is not a topology or performance-acceptance
policy. Unsupported hosts emit no pinned directory and no uncontrolled substitute.

The closed `luna.compiled-fragment-pinned-series.v1` directory contains only
`manifest.csv`, `samples.csv` and its `18 * cycles` raw records (20/38/182 files
for 1/2/10 cycles). It binds the unchanged shared validator, pinned validator,
sampler and affinity-controller source digests; declares CPU/group and unpinned
setup scope; and preserves exact raw-to-combined row mappings, per-profile
materialization keys, common build/workload identities and file byte hashes.
The explicit pinned reader and default reader share a private validation core
but select their modes through trusted entry points, never the manifest or an
ambient flag. Both reject the other's protocol. Readers are read-only and check
all raw/combined bytes again before returning; neither starts a probe, executes
archive code, infers a winner or approves latency/release. Optional commit and
manifest anchors detect mismatches, not authenticity by themselves.

CI exports the pinned series to `compiled-fragment-pinned-evidence/` below,
**alongside**, not inside, `compiled-fragment-evidence/`,
keeping the existing artifact names and 14-day retention. macOS or unsupported
Windows groups omit it explicitly. The default synthetic gate covers LF/CRLF,
multiple cycles, unchanged input bytes, missing/unlisted/symlink/unsafe-path inputs,
metadata/source/order corruption, rehashed invalid records, mode separation and
failure without publication. Pinned/protocol tests share synthetic fixture builders.
This directory still needs matching trusted checkout bytes: it carries digests,
not a portable controller/validator source snapshot; the separate evidence package
below supplies those bytes. Controlled power/background experiments and durable
archival remain separate work; setup and between-block balance remain unclaimed.

#### Compiled-plugin comparison protocol

`moonir-canonical-test --compiled-fragment-cost [iterations] [rounds] [O0|O2|O3]` reuses the
compiler harness's frontend/backend linkage, not native stand-in handlers.
`benchmarks/compiled_fragment/` contains independent library host/plugin packages.
The probe compiles both with MoonIR O2, encodes and self-verifies real Moon
Containers, obtains owner publication evidence from verified decoding, and loads
both through `loadVerifiedMoonGenerationOnce`. It does not bypass verification
through direct JIT registration. The adapter defaults to LLVM IR O0 and accepts
explicit O2/O3 profiles. Protocol v2 records the exact IR level and
`orc_codegen=default`: ORC machine-code generation is unchanged. MoonIR O2 alone
does not select LLVM IR O2. Production defaults and Runtime C ABI are unchanged.

Each input is `call_index % 1024`; let `B = input * 3`:

| Timed cases | Expected result | Entry ABI |
| --- | --- | --- |
| plain, private_erased, static_resume | B + 17 | i32 → i32, no context |
| static_discard | B | i32 → i32, no context |
| dynamic_none, dynamic_one, dynamic_chain_2, dynamic_chain_4, dynamic_override_none | B + 17 | explicit context + i32 → i32 |

The host explicitly chooses `resume_a/b/c/d` in that order. Generated Copy
factories receive `mask=0`, so every timed handler resumes; chain lengths are
checked as 1/2/4. Local None overrides chain-4 without mutating its parent.
Each profile executes 320 non-timed output checks: 32 inputs across nine
cases, plus 32 factory `mask=1` checks distinguishing resume from discard.
Default CTest checks all three profiles (960 results) and configuration/cache
rejection, without calling timing mode. Runtime teardown precedes all result checks and samples; pinned entries and contexts
retain generated code and environments. This does not add Luna threading or
native-code isolation guarantees.

Timing is opt-in. Defaults are 10000 iterations and nine rounds; accepted ranges
are 1..10000000 iterations and 1..90 rounds. Each round measures all nine cases,
with `(position + 4*round) % 9` (zero-based). Multiples of nine are position-balanced;
partial runs report `no`. Every sample warms `min(iterations,1000)` calls, resets
the input sequence for measurement, and verifies a closed-form checksum after
the timer. CSV `calls` and `checksum` include warmup; `ns_per_op` covers measured
calls only. The checksum verifies final outputs, **not** per-handler event counts.
Native harness case branching, input variation, indirect JIT calls and checksum
accumulation are inside the timer. Baselines use a different context-free ABI;
differences are end-to-end observations, not isolated Slot instructions.

Compile/encode/owner-decode, verified load/JIT, discovery/factory/binding setup,
result validation, and output are outside dispatch timers. Three setup durations
are separate single observations, not statistically sampled setup benchmarks.
Metadata includes build HEAD, probe SHA-256, the path/hash aggregate of all six
workload files, actual host/plugin container digests, build type, native compiler,
C++ dialect, LLVM version, target/layout, optimization profile, materialization key and uncontrolled
affinity/power. HEAD alone does not prove a clean worktree or complete build provenance;
artifact digests may differ by target and source locations.

```sh
cmake --build build --target moonir-canonical-test --parallel
cmake -Werror=dev -DLUNA_COMPILED_PROBE_EXECUTABLE="$PWD/build/moonir-canonical-test" \
  -DLUNA_COMPILED_PROBE_ITERATIONS=10000 -DLUNA_COMPILED_PROBE_PROFILE=O2 \
  -DLUNA_COMPILED_PROBE_RECORD="$PWD/build/compiled-fragment-cost-O2.csv" \
  -P tests/compiled_fragment_benchmark.cmake
```

Adjust the executable path for Windows/multi-config builds. The script defaults
to three iterations for a protocol check and independently verifies 81 samples,
case/position balance, exact calls/checksums, source/workload hashes, metadata,
partial labeling and invalid CLI rejection. It writes CSV only after validation.
The script's profile defaults to O0; select O2/O3 explicitly. Platform CI runs
three-iteration CLI preflight, then the multi-process series below. Neither
ns/op nor setup duration is a pass/fail threshold. These shared-runner samples
do not close SF008: controlled machine state, wider replication and stable
evidence archival remain acceptance work.

The compiler adapter sets `CodeGenerator`'s LLVM IR level before verified
lowering. The optional generation `materializationKey` keeps artifact digest
and generated-code configuration separate; it includes LLVM version, IR profile
and the default ORC policy. Both the early adapter cache and Runtime's locked
load-once path reject different keys, leaving active history and output handles
unchanged. Same-profile reuse still validates target/layout, integrity and owner
Slot evidence first. Explicit activation/rollback may switch profiles while old
pins retain their code. This is an additive C++ source control-plane extension,
not a container/sysmeta schema or Runtime C ABI change. The key is trusted loader
attestation, not cryptographic proof or a native-code sandbox.

#### Repeated-process comparison protocol

`tests/compiled_fragment_series.cmake` starts one new executable process per
measured record, without interleaved default/invalid-CLI smoke processes. One
cycle contains all six O0/O2/O3 permutations: six blocks of three launches,
18 processes, six independent processes per profile and 1458 dispatch samples.
Each profile appears twice in each within-block position; each directed distinct
profile neighbor appears twice **within blocks**. Between-block transitions are
not claimed balanced. Additional cycles rotate permutation-block order; accepted
cycles are 1..10, default 1. Iterations default to 3 for a smoke run, accepted
canonical decimal integers 1..10000000 (no leading zero); CI uses 10000.

Each process reuses the v2 validator for 81 samples, counters, checksums, build
and artifact facts. All processes must agree on host/plugin container hashes,
target/layout, build/source identities, sampling configuration and ORC policy;
materialization keys must agree separately per profile. Required metadata keys
occur exactly once, and SHA-256 fields reject suffixes. This separates actual
replication from repeatedly invoking a cached generation in one process.

All results validate in memory before any bundle is written. The requested
output directory must be fresh; existing paths are never overwritten or removed.
The bundle contains `process-N-PROFILE.csv`, `samples.csv` prefixed with process,
cycle, block, within-block position and profile, and a last-written `manifest.csv`.
The manifest records common provenance, runner/validator source hashes checked
unchanged during sampling, per-profile keys, counts/order claims,
actual-byte SHA-256 for every raw file and the combined CSV. Writes are read back
and checked after LF/CRLF normalization before the manifest is published. An
interrupted write may leave a partial directory without a complete manifest;
readers must verify the listed hashes, not treat directory existence as success.
Hashes establish integrity, not authenticity, clean-tree proof or reproducibility.

```sh
cmake -Werror=dev -DLUNA_COMPILED_PROBE_EXECUTABLE="$PWD/build/moonir-canonical-test" \
  -DLUNA_COMPILED_PROBE_ITERATIONS=10000 -DLUNA_COMPILED_SERIES_CYCLES=1 \
  -DLUNA_COMPILED_SERIES_OUTPUT_DIR="$PWD/build/compiled-fragment-series" \
  -P tests/compiled_fragment_series.cmake
```

Linux C++17/C++23, macOS and Windows CI archive the entire bundle alongside the
native probe for 14 days. The default synthetic CTest checks LF/CRLF fixtures,
cycles 1/2/6/10 and rejection of duplicate metadata, damaged counters/checksums,
headers/digests, changed provenance, repeated/truncated schedule and occupied
output paths, without starting a timing probe. This adds no language, container
or Runtime API. Process and block scheduling, affinity/power and shared-runner
load remain uncontrolled; this protocol does not close performance acceptance.

#### Offline bundle acceptance

`tests/compiled_fragment_bundle.cmake` accepts an already unpacked v1 bundle
without executing a probe, extracting an archive, writing files, or applying
timing thresholds. It requires exactly the manifest, combined CSV and listed raw
records; missing, unlisted and symlinked entries are rejected. Record filenames
must be the canonical `process-N-PROFILE.csv` basenames, with no path traversal.
Manifest/raw size is bounded at 64 KiB each, combined CSV at 4 MiB, and cycles
remain bounded at ten. Required manifest keys are unique, unknown keys/text are
rejected, and declared counts/order/balance must match the v1 schedule.

The reader checks actual-byte SHA-256, every raw v2 record, common identities and
per-profile keys, then reconstructs the combined CSV from the raw records and
compares every mapped row. A wrong checksum, changed provenance or incorrect
combined mapping is rejected even when the affected files have been rehashed.
Runner/validator hashes must match the source checkout's exact bytes, and raw
probe/workload hashes are checked by the shared validator. Keep the matching
source checkout with an archive; differing checkout line endings can change
these byte hashes. This is not an arbitrary historical-version reader.

```sh
cmake -Werror=dev -DLUNA_COMPILED_BUNDLE_DIR="/path/to/compiled-fragment-series" \
  -DLUNA_COMPILED_BUNDLE_EXPECTED_COMMIT="<full-lowercase-40-hex-commit>" \
  -DLUNA_COMPILED_BUNDLE_EXPECTED_MANIFEST_SHA256="<lowercase-64-hex-manifest-digest>" \
  -P tests/compiled_fragment_bundle.cmake
```

Both expected anchors are optional. If supplied, compare them with independently
trusted archive/CI facts; calculating them from the same untrusted bundle does
not establish authenticity. Commit metadata alone is self-reported, not proof
of a clean tree, complete build provenance or reproducibility. Inputs must remain
immutable while validating: before/after byte-hash checks detect changes but do
not provide a filesystem snapshot or a sandbox against concurrent hostile edits.
Without anchors this is self-consistency acceptance against source bytes, not
authenticated provenance, performance acceptance or release authorization.

All four platform/dialect observation jobs run the reader with the workflow
commit before upload. Default CTest adds synthetic file-fixture acceptance and
rejection (including rehashed corruption), read-only byte checks, LF/CRLF and
cycles 1/2/10; Unix also tests a symlinked record. Fixtures stay in uniquely named
build-tree directories, never overwrite observations, and do not run timing code.
The 14-day CI retention policy is unchanged; durable evidence storage and
controlled-machine performance measurements remain separate unfinished work.

#### Portable evidence export

`tools/package_compiled_fragment_evidence.cmake` exports an accepted bundle to a
fresh directory with an existing non-symlink parent. Both the expected observation
commit and actual bundle-manifest SHA-256 are required. Existing output paths and
output nested inside the input bundle are rejected. It never overwrites/deletes
observations, creates a Release, uploads to storage, or changes retention policy.

The package contains the original `bundle/`, twelve exact-byte files under
`source/` (the reader, protocol, runner, packaging/byte-checking tools, probe C++
and six workload inputs), and a last-written `evidence.csv`. This v1 index has a
closed, sorted path/hash inventory, observation commit/manifest anchors, cycle
count, `source_snapshot=validation-inputs-only` and `approval=none`. Its
`bundle_git_commit` refers only to the observation; archived validation tools are
identified by their byte hashes, not attributed to that historical commit.
Copies preserve bytes, including source/record line endings. Input/copy hashes
are checked before index publication, then the completed package is byte-checked.
Partial directories may remain after interruption; directory/index existence
alone is not evidence of acceptance. Never reuse such a directory as output.

```sh
cmake -Werror=dev -DLUNA_COMPILED_EVIDENCE_BUNDLE_DIR="/path/to/compiled-fragment-series" \
  -DLUNA_COMPILED_EVIDENCE_OUTPUT_DIR="/existing/parent/new-evidence-directory" \
  -DLUNA_COMPILED_EVIDENCE_EXPECTED_COMMIT="<observation-commit>" \
  -DLUNA_COMPILED_EVIDENCE_EXPECTED_MANIFEST_SHA256="<manifest-byte-digest>" \
  -P tools/package_compiled_fragment_evidence.cmake
```

Export prints the actual-byte SHA-256 of `evidence.csv`. Preserve this index
anchor independently of the package, together with trusted CI/source facts.
`tools/compiled_fragment_evidence.cmake`, run from a trusted checkout, accepts the
unpacked package read-only, checking its entire closed tree and each byte hash:

```sh
cmake -Werror=dev -DLUNA_COMPILED_EVIDENCE_DIR="/path/to/copied-evidence-directory" \
  -DLUNA_COMPILED_EVIDENCE_EXPECTED_COMMIT="<observation-commit>" \
  -DLUNA_COMPILED_EVIDENCE_EXPECTED_INDEX_SHA256="<independently-trusted-index-digest>" \
  -P tools/compiled_fragment_evidence.cmake
```

Byte verification never executes/includes archived scripts. It does **not**
repeat bundle protocol acceptance: a rehashed script is just data, and replacing
both files and a self-reported index does not establish authenticity. The optional
expected index/commit anchors must come from independently trusted facts. Only
after establishing trust in the validation source bytes may a receiver manually
run `source/tests/compiled_fragment_bundle.cmake` against `bundle/`, using the
original expected commit/manifest anchors. Relocation then requires no original
source path or Git checkout. Do not run archived code based solely on an untrusted
index or on an unanchored successful byte check.

The source files are sufficient for the bundled reader's validation inputs, not
a complete build/reproducibility snapshot, compiler/JIT binary, native probe
record, attestation or signature. Keep the sibling native CSV separately if
needed. Inputs must stay immutable during checks/export; this is not a filesystem
snapshot or a sandbox against hostile concurrent edits. The package has no timing
threshold or performance/release approval, and does not alter SF008 scope.

Linux C++17/C++23, macOS and Windows CI export this package after bundle acceptance
and upload `compiled-fragment-evidence/` alongside the native CSV under the same
artifact names for 14 days. Default synthetic CTest checks LF/CRLF, 1/2/10 cycles,
relocation, unchanged observations, inert archived scripts, occupied/nested
outputs and corrupted/incomplete/unsafe inventories (Unix also symlink records),
without running a timing binary. The package enables migration to separately
chosen storage; a permanent backend, retention/access policy and controlled
performance acceptance are still open. Release-evidence/attestation workflows
and ecosystem locks are unchanged.

#### Portable pinned evidence

`tools/package_compiled_fragment_pinned_evidence.cmake` explicitly exports an
accepted pinned bundle; `tools/compiled_fragment_pinned_evidence.cmake` is its
read-only byte-check entry point. The protocols are
`luna.compiled-fragment-pinned-evidence.v1` and
`luna.compiled-fragment-pinned-series.v1`. Default and pinned entry points reject
the other mode and never select it from metadata. They share private export/check
cores; old evidence v1 retains its index format and 12-source inventory.

The pinned package contains original `bundle/` bytes, 17 validation-input files
under `source/`, and a last-written `evidence.csv`. Its inventory replaces the
default sampler with the pinned sampler, adding the pinned bundle reader,
pinned protocol validator, `fragment_thread_affinity.h`, pinned evidence reader
and pinned exporter. Shared reader/validator, export/check cores, probe C++ and
six workload inputs remain. The sorted path/hash index preserves observation
commit/manifest anchors, cycles, `source_snapshot=validation-inputs-only` and
`approval=none`; CPU/group/scope/controller facts remain in original manifest/raw
records. Cycles 1/2/10 have 38/56/200 files including the index.

```sh
cmake -Werror=dev -DLUNA_COMPILED_PINNED_EVIDENCE_BUNDLE_DIR="/path/to/pinned-series" \
  -DLUNA_COMPILED_PINNED_EVIDENCE_OUTPUT_DIR="/existing/parent/new-pinned-evidence" \
  -DLUNA_COMPILED_PINNED_EVIDENCE_EXPECTED_COMMIT="<observation commit>" \
  -DLUNA_COMPILED_PINNED_EVIDENCE_EXPECTED_MANIFEST_SHA256="<manifest byte digest>" \
  -P tools/package_compiled_fragment_pinned_evidence.cmake
cmake -Werror=dev -DLUNA_COMPILED_PINNED_EVIDENCE_DIR="/path/to/copied-pinned-evidence" \
  -DLUNA_COMPILED_PINNED_EVIDENCE_EXPECTED_COMMIT="<observation commit>" \
  -DLUNA_COMPILED_PINNED_EVIDENCE_EXPECTED_INDEX_SHA256="<independently trusted index digest>" \
  -P tools/compiled_fragment_pinned_evidence.cmake
```

Export requires commit/manifest anchors, a fresh output and an existing
non-symlink parent; observations are not overwritten and nested output is rejected.
Preflight failures create no output. Bytes and the full protocol are rechecked
around copying; interrupted writes can still leave partial directories. Both byte
readers recheck inventory-file hashes before returning, execute no archived code
and do not replay full bundle acceptance. Rehashed archived scripts remain data.
Only after confirming source bytes against independently trusted facts may one
manually run `source/tests/compiled_fragment_pinned_bundle.cmake`, specifying
`LUNA_COMPILED_PINNED_BUNDLE_DIR` and original commit/manifest anchors. The relocated
matched-source reader needs no original checkout. Python summaries still use the
current trusted checkout and never automatically execute package source.

CI uploads the package beside `compiled-fragment-evidence/`; original pinned
records are inside it, not also uploaded as a duplicate loose bundle. Unsupported
hosts produce no pinned package. Artifact names and 14-day retention are unchanged.
Shared synthetic gates cover fixed source counts, relocation, read-only bytes,
mode separation, missing/corrupt controller and failed preflight without publication;
they run no timing or affinity code. This snapshot is not a complete build,
probe binary, signature or attestation. Digests do not establish authenticity or
reproducibility and are not a hostile-concurrency filesystem snapshot. Performance
approval, controlled power/background conditions and durable storage remain open;
Runtime ABI, SF008 and formal release-evidence workflows are unchanged.

#### Offline descriptive summaries

`tools/summarize_compiled_fragment_bundle.py` (Python 3.8+, standard library only)
accepts an unpacked bundle through the **trusted current checkout's** CMake reader
before producing `luna.compiled-fragment-summary.v1` JSON on stdout. It never
executes a probe or archived scripts, extracts an archive, or writes input/output
files. `--cmake` selects a trusted executable; its default is `cmake` on PATH.
The source bytes must match the bundle's protocol/probe/workload/runner identities,
as for ordinary offline acceptance. This is not an arbitrary historical reader.

```sh
python3 tools/summarize_compiled_fragment_bundle.py \
  --bundle "/path/to/compiled-fragment-series" \
  --expected-commit "<full lowercase observation commit>" \
  --expected-manifest-sha256 "<independently trusted manifest byte digest>"
```

The default, also spelled `--mode uncontrolled`, retains that protocol and reader.
Only explicit `--mode pinned` selects the pinned reader and produces the separate
`luna.compiled-fragment-pinned-summary.v1` report. Neither mode detects or falls
back to the other from metadata; mismatched bundles are rejected with no report.
Both reuse the same decimal/process aggregation. Pinned reports preserve all
manifest metadata, including CPU/group, controller and pinned-validator digests,
per-profile materialization keys and unpinned setup scope:

```sh
python3 tools/summarize_compiled_fragment_bundle.py --mode pinned \
  --bundle "/path/to/compiled-fragment-pinned-series" \
  --expected-commit "<full lowercase observation commit>" \
  --expected-manifest-sha256 "<independently trusted manifest byte digest>"
```

Pinning is limited to the dispatch measurement thread and verified sample
boundaries, not setup, power policy, shared machine load or between-process
scheduling. Boundary-check/harness costs prevent treating pinned and uncontrolled
records as interchangeable. The tool does not merge bundles or compare modes.

Optional anchors have the same trust boundary as the reader. Without an external
manifest anchor, the tool pins the manifest bytes seen at the start for internal
consistency, not authentication. It rechecks raw-file, combined-file and manifest
bytes plus the closed inventory before emitting a complete report. Validation,
missing-tool or parsing failure produces an error on stderr, a nonzero status
and no report on stdout. Inputs must remain unchanged; this is not a filesystem
snapshot against malicious concurrent changes. Archived scripts remain data.

Each process/case reports count, minimum, median and maximum of its nine rounds.
Each profile/case then summarizes the **independent-process medians**, not all
pooled rounds: six processes per profile per cycle. Even-count medians average
the two central values; exact decimal strings avoid floating-point rounding.
Per-process rows retain cycle, block, profile position and record name, and the
report retains manifest identity and the reporting tool's actual byte digest.
Setup stages separately summarize their single observation per process; they
remain descriptive observations, not statistically sampled setup benchmarks.

The report explicitly records `approval=none`, uncontrolled machine conditions,
different entry ABIs and timed harness costs. It supplies no confidence intervals,
ratios/winner selection, timing gates, isolated Slot instruction-cost claim or
performance/release approval. It is derived data, not part of the closed v1
bundle/evidence inventory; keep any saved report **outside** those directories.
Existing package formats, 14-day CI retention, Runtime semantics and SF008 scope
are unchanged. Permanent storage and controlled performance acceptance stay open.

The default `luna.compiled-fragment-summary` and `luna.compiled-fragment-pinned-summary`
CTest gates share the same fixture/statistics test and reuse the Python 3.8+
interpreter already required by `BUILD_TESTING`; compiler-only builds gain no dependency.
Synthetic LF/CRLF fixtures cover zero values, exact decimal/even-count medians,
1/2-cycle aggregation, a skewed distribution that distinguishes process medians
from pooled rounds, unchanged input bytes, wrong anchors, corrupted records and
missing CMake. The tests execute only the trusted reader, never a timing probe.
They also check explicit/default uncontrolled equivalence, mutual mode rejection,
invalid mode spelling, fixed identity retention and limited-affinity wording,
CPU/group/setup mismatches, and controller/checksum corruption with recomputed
raw hashes. All failures leave stdout without a report and inputs unchanged.

#### Local compiled-plugin observations (2026-09-27)

This round evaluates existing code without modifying Runtime, ABI, probes,
timing protocols or system power/priority policy. The observation build is
`303c6bee7bfd9b0232be61fc2625c66dca4ccace`; the tree was clean before sampling.
Environment: Windows 11 build 26200, i7-12700 (12 cores/20 logical processors),
MSYS2 CLANG64 Clang/LLVM 20.1.8, C++17, Ninja RelWithDebInfo (native
`-O2 -g -DNDEBUG`, strict warnings). Only dispatch measurement threads were
pinned to logical CPU 0/group 0 and checked at sample boundaries; no physical
core class is claimed. Read-only power queries before and after reported
Windows Balanced, but frequency, temperature, background load, setup affinity
and process scheduling were uncontrolled/not continuously monitored. No build,
regression suite or second measurement cohort ran concurrently.

Separate sequential cohorts used 10000 and 100000 iterations, each with three
complete series cycles: 54 independent processes, 18 per profile, nine samples
per process/case, 4374 samples. Total: 108 processes/8748 samples. Warmup was
1000, MoonIR O2, LLVM IR O0/O2/O3, default ORC. Every schedule, identity,
call/checksum, raw/combined mapping and byte check passed. Common manifest
identities matched apart from iteration configuration and combined-file hash.
Each cohort was summarized separately, without pooling earlier one-cycle or
uncontrolled records.

Each sample is average ns/op across its batch, not individual-call latency.
The 100000-iteration table reports **median of process medians (minimum–maximum
process median)**, not P50/P99 or confidence intervals:

| Case | LLVM O0 | LLVM O2 | LLVM O3 |
| --- | --- | --- | --- |
| plain | 4.50 (4.40–4.50) | 2.00 (1.90–2.00) | 2.00 (2.00–2.10) |
| private_erased | 4.50 (4.40–4.50) | 2.00 (2.00–2.00) | 2.00 (2.00–2.00) |
| static_resume | 2.20 (2.20–2.30) | 2.00 (2.00–2.10) | 2.00 (2.00–2.00) |
| static_discard | 2.00 (2.00–2.00) | 2.00 (2.00–2.00) | 2.00 (2.00–2.00) |
| dynamic_none | 216.30 (209.10–225.40) | 217.05 (208.40–230.20) | 215.30 (211.30–229.50) |
| dynamic_one | 561.35 (544.40–609.80) | 555.15 (542.30–594.00) | 548.90 (531.70–575.70) |
| dynamic_chain_2 | 799.10 (771.60–824.50) | 790.60 (770.30–842.00) | 793.15 (772.30–831.60) |
| dynamic_chain_4 | 1422.75 (1384.20–1535.10) | 1356.15 (1317.60–1401.90) | 1342.85 (1290.50–1440.40) |
| dynamic_override_none | 218.10 (211.20–226.90) | 217.45 (213.00–233.10) | 215.55 (212.30–229.50) |

O2 cohort summaries below are separate sequential observations. Their changes
cannot be attributed to iteration count, used to select a winning profile, or
claimed as a speedup:

| Case | 10000 iterations | 100000 iterations |
| --- | --- | --- |
| dynamic_none | 209.35 (203.70–221.20) | 217.05 (208.40–230.20) |
| dynamic_one | 546.10 (523.20–581.80) | 555.15 (542.30–594.00) |
| dynamic_chain_2 | 777.75 (751.80–824.40) | 790.60 (770.30–842.00) |
| dynamic_chain_4 | 1339.35 (1285.40–1388.40) | 1356.15 (1317.60–1401.90) |
| dynamic_override_none | 212.50 (206.60–246.10) | 217.45 (213.00–233.10) |

Long-cohort O2 per-cycle medians were 544.45/568.25/562.90 ns/op for One and
1344.70/1369.40/1353.25 for chain-4, not constant across cycles. Individual
batch-average samples for O2 None ranged from 201.50 to 614.70 ns/op; the
process-median table is not a tail-latency bound. Causes are not isolated.
Long-cohort O2 setup medians were 11.518 ms compile/encode/decode, 24.984 ms
verified load/JIT and 0.0315 ms lookup/four-candidate discovery/host ordering/
factories/bindings/context creation. These single setup observations are
unpinned, not a general reflection benchmark or large-catalog evidence.

Source inspection is distinct from timing attribution: `findBindingEntry`
binary-searches the pinned BindingSet, not the candidate catalog.
`luna_runtime_fragment_dispatch_v1` constructs owning Slot/Contract/argument-layout
strings per invocation. `dispatchRuntimeFragmentChain` uses the public activation
constructor per handler, which calls
`std::make_unique<RuntimeFragmentActivationState>` and copies identity/carrier
data. These are code allocation paths, not measured allocation counts or
attributable time. Validation, snapshot pinning, callbacks/capture writeback and
different entry ABIs/harness costs contribute to the full path. Subtracting
plain does not isolate a Slot instruction; static-discard also returns a different
result.

The next recommended experiment is scoped activation storage **inside the
synchronous chain**, retaining the public owning activation API, nominal/layout
validation, single-shot, failure propagation, escape/cleanup, nesting and
generation pinning, and checking deep-chain stack use. First verify correctness
and allocation paths without timing, then remeasure with matched protocols.
Owning entry-string optimization is separate: establish C ABI byte lifetimes
before proposing borrowed views. No new candidate-set mechanism or context/
reentry/non-Copy ABI scope is needed. None of these optimizations was implemented
in this round; no threshold was set, `approval=none`, controlled acceptance stays open.

Local evidence is under
`build-audit-clang64/fragment-evaluation-303c6be-i10000-c3-evidence/` and
`fragment-evaluation-303c6be-i100000-c3-evidence/`, each with 74 files and original
records in `bundle/`. These ignored build-tree directories are not durable
external storage or automatically uploaded CI artifacts. Manifest SHA-256:

```text
10000:  e7bee2874fac69d3f38e73312304d8d7b6229fa70170b5244ffee6084a624f19
100000: 9c92401ecebfedc5d34c15d77254a5f0e26a1d418327981d111d1ee80e4ed1f9
```

Evidence-index SHA-256:

```text
10000:  d5e18925b4b4958630af61ed7695477afa2a2379704a566557ebd5b281a9d437
100000: 0c40d250ad89548d79604af47bfe924e68b718a092732a931d6695fa63823226
```

Summary tool SHA-256:
`e35ce22f899950fc79915de09af05d9f4f2eb952a0d082211bc83ff0ca4eeb59`.
Actual probe binary SHA-256:
`c2a14bb67460a16f5aeff9afd57e4e9c63972ca02a50f7c6344deee0a21ad682`.
RuntimeFragment.cpp last changed in `0c92301928b51018847102687c7b354f499bb7b9`;
its current SHA-256 is
`fc7bdaa9bba31506418a0a25f01d09c01f00355c6e7edbc5f6f1ba07248604e1`.
These identities/anchors support observation checks, not complete build
attestation or reproducibility claims.

To rerun, select an allowed CPU and fresh output paths, pass iterations
10000/100000 and three cycles separately to the pinned series, then use pinned
evidence export and `--mode pinned` summaries with each commit/digest. Values
need not reproduce exactly; later report commits are not these observation builds.

#### Scoped synchronous-chain activation storage (2026-09-27)

The first proposed optimization is implemented: internal chain dispatch creates
an activation on each synchronous handler's call frame, borrowing Slot/Contract
and carrier records owned by the enclosing dispatch. It no longer invokes the
public constructor for per-handler heap state and identity-string copies.
Public owning `RuntimeFragmentActivation` construction, moves and opaque API
are unchanged, as are C ABI functions/descriptor layouts and the v1 version.
The same private state type retains owning fields, left empty in internal
frames, avoiding a second opaque representation.

Both paths share complete activation contract validation. Single-shot, sticky
failure, downstream diagnostics, continuation escape, exception propagation
and the complete BindingSet/generation pin remain intact. Handler tokens live
only during synchronous execute (including nested resume), not after return;
the old implementation also released activations on return. Payload bounds and
lifetime remain host duties. Entry-string construction and per-dispatch owning
record snapshots are unchanged; this is not allocation-free whole dispatch.

The existing `luna.runtime-fragment-v1` test now compares ordinary C++ allocation
counts for 1/4/64-handler chains with long identities to avoid SSO hiding copies.
The old implementation fails because counts grow with chain length; the new
counts match. Fixture construction is outside the counting window, handlers/base
do not allocate, and aligned/system allocations are not covered. This is not a
timing threshold. Tests also cover distinct addresses for 64+64 simultaneously
live activations, matching arguments before/after resume and wrong-identity
rejection, nested completion/escape, recovery after an inner handler throws, and
public owning records surviving caller mutation/destruction and two moves.

ASan/UBSan builds directly compile the relevant Runtime implementation into this
test, instrumenting more than its fixture without contaminating the installed
AOT runtime archive. Strict-warning standard builds and both Runtime tests pass,
as do the same two tests under ASan/UBSan. Deep chains remain recursive; tested
depths are not a bound for arbitrary stack use. No chain-length policy or async
semantics are added. The full non-hardware CTest suite passes 77/77 (268.39 s),
including the evidence and summary gates. Matched-protocol timing and controlled acceptance remain
open; reduced allocation paths are not a measured speedup claim.

#### Linux sanitizer allocation-counter fixture repair (2026-09-27)

Before remeasurement, CI for `6da36eb` showed macOS/Windows passing and Linux
ordinary C++17/C++23 and TSan passing, but ASan/UBSan failed
`luna.runtime-fragment-v1`. The log reported
`alloc-dealloc-mismatch (operator new vs free)` while libstdc++ released a
`stable_sort` temporary buffer, rather than an activation lifetime failure.

The test counter replaced ordinary throwing new/delete but omitted nothrow
entry points. The buffer used the sanitizer's default nothrow new, followed by
the test's free-based sized delete. Scalar/array nothrow allocation and cleanup
delete now complete the ordinary allocation family using the same allocator.
An explicit regression checks four nothrow allocations, their counts and both
ordinary/cleanup release paths. Aligned allocation is not replaced; mismatch
checking and sanitizers are not disabled. Production Runtime, ABI and timing
protocols are unchanged.

Existing local Arch Linux WSL (Clang 22.1.8/libstdc++ 16), directly compiling
the fixture and relevant Runtime with C++17, O0,
`-fsized-deallocation -fsanitize=address,undefined`, reproduced the same stack
before the fix and passes afterward, as does C++23. This is neither an Ubuntu
CI replica nor full Linux compiler-suite acceptance. Both Runtime CTest gates
pass in strict-warning normal and ASan/UBSan Windows builds. Planned matched
timing samples have not started; remote regression confirmation comes first.

#### Matched-protocol scoped-activation observations (2026-09-27)

The observation build is `108193719213ca0344541b736acbc835b54adfe6`.
Production Runtime last changed in `6da36eb`; the later repair changed only the
test counter. The probe was rebuilt at the new commit and passed untimed result
checks. The tree was clean before sampling and sources/protocols were unchanged
throughout. No second optimization was implemented.
Before committing this report, Linux/macOS/Windows CI for this build all
succeeded, including the repaired Linux ASan/UBSan gate: runs
`36324347602`/`36324347627`/`36324347594` respectively.

Environment remains Windows 11 build 26200, i7-12700 (12 cores/20 logical CPUs),
Clang/LLVM 20.1.8, C++17, strict-warning RelWithDebInfo, native
`-O2 -g -DNDEBUG`. Only dispatch measurement threads were pinned to logical
CPU 0/group 0; allowed CPUs were 0–19 and no physical core class is assumed.
Read-only start/end queries reported Balanced. Frequency, temperature,
background load, setup affinity and process scheduling remain uncontrolled.
Earlier Linux fixture diagnosis used WSL; background conditions were not shut
down or normalized. This agent ran no concurrent local build, regression,
WSL compile or second measurement cohort. Remote CI was tracked separately.

Separate sequential 10000/100000-iteration cohorts each used three cycles,
1000 warmups and nine batch samples per case. MoonIR O2, LLVM IR O0/O2/O3 and
default ORC were unchanged. Each has 54 fresh processes, 18 per profile and
4374 samples: total 108/8748. All schedule/identity/call/checksum/raw-to-combined
mapping and byte checks passed. Common manifest metadata against matching
`303c6be` cohorts differed only in commit and combined-file hash. Workload,
probe/runner/validator/affinity-controller source hashes, container hashes,
configuration, materialization keys and timed harness matched. Between the two
new manifests only iterations and combined hash differ. Actual probe binaries
and Runtime source differ; matching metadata is not a claim of identical builds.

Values remain **median of process medians (minimum–maximum process median)**,
in ns/op. Each underlying sample averages an entire batch, not a single call.
Complete new 100000-iteration results:

| Case | LLVM O0 | LLVM O2 | LLVM O3 |
| --- | --- | --- | --- |
| plain | 4.40 (4.30–4.60) | 2.00 (2.00–2.00) | 2.00 (2.00–2.00) |
| private_erased | 4.40 (4.40–4.50) | 2.00 (2.00–2.00) | 2.00 (1.90–2.00) |
| static_resume | 2.20 (2.20–2.20) | 2.00 (1.90–2.00) | 2.00 (1.90–2.00) |
| static_discard | 2.00 (1.90–2.00) | 2.00 (1.90–2.00) | 2.00 (1.90–2.00) |
| dynamic_none | 211.85 (210.10–225.50) | 213.35 (209.40–233.20) | 213.25 (207.00–218.80) |
| dynamic_one | 459.35 (450.00–486.80) | 461.95 (452.70–500.70) | 460.60 (453.50–471.60) |
| dynamic_chain_2 | 609.70 (598.60–649.60) | 608.20 (589.60–658.00) | 605.65 (592.10–622.50) |
| dynamic_chain_4 | 977.90 (962.00–1022.80) | 904.80 (888.10–977.70) | 906.35 (895.30–920.20) |
| dynamic_override_none | 214.25 (208.40–225.90) | 213.20 (208.90–222.60) | 213.60 (209.70–219.50) |

Matching-profile O2 old/new cohorts are summarized separately, not pooled or
paired across processes at different times:

| Case | Old 10000 | New 10000 | Old 100000 | New 100000 |
| --- | --- | --- | --- | --- |
| dynamic_none | 209.35 (203.70–221.20) | 211.35 (205.50–218.20) | 217.05 (208.40–230.20) | 213.35 (209.40–233.20) |
| dynamic_one | 546.10 (523.20–581.80) | 460.25 (447.70–479.40) | 555.15 (542.30–594.00) | 461.95 (452.70–500.70) |
| dynamic_chain_2 | 777.75 (751.80–824.40) | 608.10 (588.90–637.00) | 790.60 (770.30–842.00) | 608.20 (589.60–658.00) |
| dynamic_chain_4 | 1339.35 (1285.40–1388.40) | 909.65 (888.20–940.40) | 1356.15 (1317.60–1401.90) | 904.80 (888.10–977.70) |
| dynamic_override_none | 212.50 (206.60–246.10) | 210.10 (206.10–227.50) | 217.45 (213.00–233.10) | 213.20 (208.90–222.60) |

Bound-path observations are lower, while None/local None remain in roughly the
same range, consistent in direction with reduced per-handler allocation.
However, these four sequential cohorts are not randomized alternating controlled
A/B trials or repetitions of one machine state. No allocator-time attribution,
confidence intervals, attributable speedup ratios, profile winners or
performance/release approval are supplied. Matching-profile O2 static paths
remain near 2 ns, but differing ABIs/harness costs and static-discard results
still prevent subtraction to isolate a Slot instruction. `approval=none` and
no new timing threshold.

New long-cohort O2 per-cycle medians are 461.85/464.15/457.20 for One and
900.10/906.90/905.30 for chain-4, not constant. Individual batch-average ranges
are 442.50–544.20 for One, 875.70–1019.30 for chain-4 and 203.50–311.80 for
None, not individual-call tail bounds. Single setup-observation medians are
11.349 ms compile/encode/decode, 24.417 ms verified load/JIT and 0.032 ms
lookup/four-candidate discovery/host ordering/factories/bindings/context.
Setup is unpinned; discovery in this fixed four-candidate workload is not a
general-reflection or large-catalog performance result.

Next, prioritize testing **reuse of Slot/Contract identities already frozen
in BindingSet**, removing extra owning identity copies per bound dispatch.
The existing snapshot pin must retain these records until every handler returns.
Do not borrow host C ABI strings, remove identity/layout validation, alter the
public owning activation or change payload lifetime duties. First test caller
identity mutation, context release, nesting/failure and allocation behavior,
then use matched timing protocols. This is a candidate, not implemented here;
no new candidate-set mechanism or keyword is needed.

New evidence remains in ignored local build directories, not automatically
uploaded CI artifacts or durable external storage:
`build-audit-clang64/fragment-evaluation-1081937-i10000-c3-evidence/` and
`fragment-evaluation-1081937-i100000-c3-evidence/`, each with 74 files.
Manifest SHA-256:

```text
10000:  2a1f7ae3f0c85ff4a85d98fd7d3218603172d306ce271e934e487f8360f0fcd8
100000: 1a1d331fcd42077303aab88242aaeecabdf7bb452615ee8b1a6e2fae5745d8fa
```

Evidence-index SHA-256:

```text
10000:  e764e8193ab5d3a7a013d416309eb74e93c67dbf9e953aef927ff040312ccd2b
100000: 92070c286c0548b16e7e93da986c873167c39ed5422b83622baf16e82300e174
```

Actual binary SHA-256 before/after sampling:
`bf2b812e51f2ce7264526b41029c27181125cf2d85e44f9cf4563924ed3a8690`.
RuntimeFragment.cpp SHA-256 before/after:
`e4772b379dc4e9f59877a8239c46aeb1f5d960b5b545a1851d6901629e4fd554`.
Summary tool remains
`e35ce22f899950fc79915de09af05d9f4f2eb952a0d082211bc83ff0ca4eeb59`.
Anchors are not complete build/binary archives, signatures or reproducibility
proofs. Neither old nor new evidence was overwritten. Reruns still require
fresh paths, explicit CPU 0, separate iterations 10000/100000, three cycles and
each commit/digest. Later report commits are not this observation build, and
values need not reproduce exactly.

#### Reuse frozen Slot identity during bound dispatch (2026-09-27)

The next candidate above is implemented. A private BindingSet Entry stores the
same two nominal keys in an owning `RuntimeSlotRequirement`; exact Slot/Contract
lookup, stable grouping and local-override ordering are unchanged. Synchronous
chain dispatch no longer copies those strings; activations reference the selected
entry's record. The existing local shared snapshot pin retains the entry, chain,
environments and generation until all nested resumes/handlers return, regardless
of release or replacement of the published handle.

The argument carrier still owns its by-value layout/size/alignment/data record;
payload storage lifetime remains a host duty. C ABI entry still copies host C
strings, and public activations still own their identity/carrier records. No public
API/ABI/syntax, validation, single-shot or failure-propagation protocol changed.
Host selection remains explicit None/One/ordered chain, with no automatic candidate
set, hot-update mechanism or ordering policy.

First adding a long-identity allocation comparison to the old implementation
produced `bound dispatch copied frozen Slot/Contract identities`; the new code
passes. Ordinary C++ allocation counts for 1/4/64 bound handlers must now equal
None dispatch with the same carrier, not just stay constant as the chain grows.
Copying the long layout in the by-value carrier can still allocate. This counter
does not cover all aligned/system allocation, prove allocation-free dispatch or
provide a timing improvement.

The existing lifetime fixture now covers 768 combinations: One/two-handler chain,
owned/borrowed environments, BindingSet/C++ context/C ABI, caller identity/carrier
mutation or destruction in the first handler or base, published-handle release or
replacement, repeated resume, and completion/escape/invalid-result/exception.
All live activations must retain original identities/arguments; environments are
destroyed before generation release, subsequent repeated resume cannot overwrite
downstream diagnostics, and replacement None affects only the next invocation.
Nested same-Slot 1/4/64 chains destroy the caller records shared by outer/inner
dispatch at the deepest base while up to 128 activations remain suspended.
Public owning moves, recovery after handler exceptions and existing local-override
regressions remain in place.

Both Runtime CTests pass in Windows strict-warning ordinary and ASan/UBSan builds;
sanitizer instruments the relevant implementation directly. Existing Arch Linux
WSL Clang 22.1.8/libstdc++ 16 also passes direct Runtime test builds under C++17
and C++23, O0, sized deallocation and ASan/UBSan. This is not a full Ubuntu CI
replica. The Windows strict-warning full build succeeds; all 77 non-hardware
regressions pass (285.74 seconds), as do documentation inventory and
`git diff --check`.

No new matched performance cohort was sampled in this implementation stage. Next confirm cross-platform
CI for a clean new build commit, then use the existing CPU/iteration/three-cycle
protocol with fresh evidence paths. The preceding observations are not performance
results for identity reuse. Performance/release acceptance and durable external
evidence storage remain open.

#### Matched-protocol frozen-identity observations (2026-09-28)

The observation build is `bc9d6fd2c9e760ab3bb10a77d862bffda0e032dc`. Its Linux,
macOS and Windows CI all succeeded: runs `36326337409`, `36326337446` and
`36326337412`. The probe was rebuilt for this commit and non-timed correctness
checks pass. The tree was clean before/after sampling; sources and timing protocols
did not change during sampling. This stage only adds an observation report, not a
third Runtime optimization. The implementation stage's 77/77 non-hardware and
Windows/WSL sanitizer results remain recorded; this report commit is not the
observation build.

The host remains Windows 11 build 26200, i7-12700 (12 cores/20 logical CPUs),
Clang/LLVM 20.1.8, C++17, strict-warning RelWithDebInfo native `-O2 -g -DNDEBUG`.
The original order runs iterations 10000 then 100000, each with three cycles,
nine batch samples per case and 1000 warmup calls. MoonIR O2, LLVM IR O0/O2/O3,
default ORC and the timed harness are unchanged. Each cohort contains 54 fresh
processes, 18 per profile and 4374 samples, totaling 108/8748. Exact mappings,
orders, nominal identities, call counts and checksums all pass. Only the dispatch
measurement thread is pinned to group 0/logical CPU 0 and checked at sample
boundaries. Allowed CPUs are 0–19; physical core class is not inferred and setup
is unpinned.

Read-only power queries before/after sampling both report Balanced. No power
policy, priority or background environment was changed; frequency, temperature,
background load and scheduling remain uncontrolled. This agent ran no concurrent
local build, regression, WSL compilation or second observation cohort while
sampling. CPU pinning is not complete machine-state control; cross-day sequential
observations are especially not controlled alternating A/B trials.

Matching-iteration common manifest metadata compared with `1081937` differs only
in commit and combined-file digest. Probe/workload/runner/validator/affinity-controller
source digests, both Container digests, configurations, harness and materialization
keys match. The two new manifests differ only in iterations and combined digest.
Runtime implementation and actual probe binaries differ; matching metadata is not
complete build identity or proof of reproducibility.

Results are **medians of process medians (minimum–maximum process median)** in
ns/op. Each underlying sample is a whole-batch call average, not individual-call
latency. The complete new 100000-iteration cohort:

| Case | LLVM O0 | LLVM O2 | LLVM O3 |
| --- | --- | --- | --- |
| plain | 4.40 (4.30–4.40) | 2.00 (1.90–2.00) | 2.00 (1.90–2.00) |
| private_erased | 4.40 (4.30–4.50) | 2.00 (1.90–2.00) | 2.00 (1.90–2.00) |
| static_resume | 2.20 (2.10–2.20) | 2.00 (1.90–2.00) | 2.00 (1.90–2.00) |
| static_discard | 1.95 (1.90–2.00) | 2.00 (1.90–2.10) | 2.00 (1.90–2.00) |
| dynamic_none | 208.80 (203.90–217.40) | 207.85 (202.90–213.40) | 209.55 (201.50–221.50) |
| dynamic_one | 404.30 (401.00–408.70) | 404.10 (396.30–422.80) | 404.55 (394.30–425.50) |
| dynamic_chain_2 | 553.80 (542.90–564.80) | 547.80 (542.40–576.60) | 552.35 (540.60–591.30) |
| dynamic_chain_4 | 914.50 (901.40–937.20) | 857.60 (842.30–892.10) | 860.20 (846.80–884.40) |
| dynamic_override_none | 208.60 (203.10–215.90) | 208.40 (204.90–219.90) | 210.55 (202.50–220.00) |

O2 old/new cohorts are summarized separately, not pooled or paired by process:

| Case | Old 10000 | New 10000 | Old 100000 | New 100000 |
| --- | --- | --- | --- | --- |
| dynamic_none | 211.35 (205.50–218.20) | 204.80 (198.80–219.10) | 213.35 (209.40–233.20) | 207.85 (202.90–213.40) |
| dynamic_one | 460.25 (447.70–479.40) | 396.65 (386.10–405.50) | 461.95 (452.70–500.70) | 404.10 (396.30–422.80) |
| dynamic_chain_2 | 608.10 (588.90–637.00) | 546.45 (536.00–566.00) | 608.20 (589.60–658.00) | 547.80 (542.40–576.60) |
| dynamic_chain_4 | 909.65 (888.20–940.40) | 850.90 (832.20–880.90) | 904.80 (888.10–977.70) | 857.60 (842.30–892.10) |
| dynamic_override_none | 210.10 (206.10–227.50) | 203.65 (198.40–228.80) | 213.20 (208.90–222.60) | 208.40 (204.90–219.90) |

Bound-path observations are lower, consistent in direction with removing owning
Slot/Contract copies per dispatch, but None/local None also shift slightly.
There is no allocator-time attribution or controlled machine state, so not all
differences can be attributed to this change. No attributable speedup ratios,
confidence intervals or profile winner are claimed. Static paths remain near
2 ns, but ABI/harness differences and static-discard results still prevent
subtraction to isolate a Slot instruction. `approval=none`; no new timing threshold
or performance/release approval.

Long-cohort O2 per-cycle medians are 399.40/404.25/406.80 for One and
858.30/856.25/862.70 for chain-4, not constant costs. Individual batch-average
ranges are 386.70–443.90 and 824.10–940.40 respectively, and 195.40–261.90 for
None, not individual-call tail bounds. Setup single-observation medians are
10.9025 ms compile/encode/decode, 24.057 ms verified load/JIT and 0.029 ms
lookup/four-candidate discovery/host ordering/factories/bindings/context. Setup
is unpinned; this synthetic four-candidate workload does not establish general
reflection or large-catalog performance.

New evidence is still only in ignored local directories, not CI uploads or
arranged durable external storage:
`build-audit-clang64/fragment-evaluation-bc9d6fd-i10000-c3-evidence/` and
`fragment-evaluation-bc9d6fd-i100000-c3-evidence/`. Each package has 74 files;
the 73 indexed file byte hashes pass the current trusted checkout's checker,
without executing archived scripts or probes. Manifest SHA-256:

```text
10000:  786997de18ad85338efd9191dec544de82e43c1ca3ec2ac28615e3e68cb4d873
100000: 2eec94a483ae2765e2a4bdc49ab9b960e7ca30767f0276e906e82db3736f7ee1
```

Evidence-index SHA-256:

```text
10000:  50eecb78d127a8ab2e30e713d74261c5962411e7d08e5f9e20379a3ffa8c561e
100000: 463baafa81aa660aa8c2908e21f6147bdde775491b5e944c0652f76f83dab632
```

Actual probe binary SHA-256 before/after sampling:
`eb8a27e28d66507268c05fb360aa5e46e9cb43c94cd45fc034e2de6799322227`.
RuntimeFragment.cpp SHA-256 before/after:
`0812cbdc4f7c931393304f2b383e31bea665e2ae72f558921b1273868a5e0242`.
Summary tool remains
`e35ce22f899950fc79915de09af05d9f4f2eb952a0d082211bc83ff0ca4eeb59`.
These are not signatures, full binary archives or complete build attestations.
No old/new evidence was overwritten; reruns require fresh paths and each
commit/digest, not identical numeric outcomes.

Report checks independently recompute all 47 table cells in each document,
per-cycle medians, batch-average ranges and setup medians from raw CSV.
Evidence inventory/digest anchors, documentation inventory and `git diff --check`
pass. This documentation stage does not claim a new complete compiler regression
or performance approval.

Next consolidate the remaining v1 acceptance checklist: completed core
correctness/allocation regressions, undecided performance/release criteria and
explicitly deferred capabilities, without immediately adding a third optimization.
Further optimization should first audit ownership/borrowing and validation at
Runtime entry and argument storage, identify a testable allocation/cost hypothesis,
then add regressions and matched observations. Do not remove validation, borrow
host storage that callbacks can destroy or expand keywords/automatic candidate
set mechanisms.
