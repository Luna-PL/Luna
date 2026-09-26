# Luna 0.3 host evolution API

English | [简体中文](evolution.zh-CN.md)

Luna 0.3 exposes its minimum stateless generation loop to an embedding host as
a C++17 source API. Include `<luna/runtime/Evolution.h>` and link the installed
`runtime` archive. `luna::runtime::EvolutionApiVersion` is `1` for this surface.
This control plane is intentionally separate from the C-compatible Runtime ABI
v1: it does not promise a C ABI or C++ binary compatibility across toolchains.

There is no Luna source keyword for generation evolution and no `luna activate`
command in 0.3. Runtime state belongs to the embedding process, so a one-shot
compiler command could not safely identify that process's safe points, retained
generations, or live references. A process manager may wrap this API, but that
protocol is application policy rather than a language or compiler contract.

## Fixed object model

- `MoonRuntime` owns module histories and each module's atomically published
  active generation.
- `GenerationStagingRequest` carries a stable module ID, content digest, and a
  non-null shared module lease. The lease must keep code and descriptor storage
  alive.
  An optional trailing `materializationKey` separates a loader's generated-code
  configuration from the artifact digest. Existing three-field requests keep
  an empty key. The key is immutable in staged/pinned snapshots; load-once reuse
  requires equal content **and** equal keys. Changing a key is an explicit
  `activate` transition, not an implicit replacement. Nonempty keys reject CR,
  LF, tab and embedded NUL before callbacks. This additive C++ source API field
  does not change Runtime C ABI v1 or any container record.
- `GenerationVerifier`, `GenerationResolver`, and optional
  `GenerationInitializer` form the trusted staging boundary. Staging runs them
  in that order and publishes nothing.
- `StagedGeneration` is a move-only candidate. `loadOnce` performs initial
  publication; `activate` performs an evolution transition.
- `PinnedGeneration` and `PinnedBinding` retain the exact generation they
  observed. They never retarget.
- `SwitchableBinding` is created explicitly with a
  `GenerationBindingRequirement`. Each `pin()` takes one atomic snapshot and
  returns a `PinnedBinding`; callers invoke only through that snapshot.
- `SafePoint` is a move-only, single-use token created by `safePoint()`. It is
  a host attestation, not runtime thread suspension. `activate` and `rollback`
  require a fresh token from the same `MoonRuntime`.
- `RuntimeFragmentBindingSet` is the immutable result of host Slot policy. It
  owns prevalidated Fragment references and pins their generations. The strict
  constructor permits at most one binding per exact Slot; the explicit chain
  constructor preserves host order. Publication through
  `activateFragmentBindings` requires the same safe-point protocol;
  `pinFragmentBindings` returns a stable dispatch snapshot. A local override
  may replace or suppress one Slot without mutating either snapshot.
- `RuntimeFragmentExecutionContext` turns one pinned BindingSet into the
  explicit data-plane capability passed to generated runtime-aware entries.
  It has no ambient Runtime lookup and preserves the same snapshot even if the
  host activates a replacement while the call is running.

A typed binding requirement consists of `symbolId`, `contractId`,
`declarationKind`, and `requiredFlags`. Once a switchable binding is created,
the runtime preserves the active binding's exact kind and flags as the minimum
requirement for every later activation. Compatibility is checked before the
new immutable generation pointer is published, never on each ordinary call.
`GenerationBindingFragmentContext` is valid only together with
`GenerationBindingCallable` on a Function binding. It tells the host that the
implementation pointer takes a leading opaque
`RuntimeFragmentExecutionContext` pointer before the declared source
parameters; it is an ABI capability, not a request to discover candidates.

## Lifecycle

The public spelling is:

```cpp
#include <luna/runtime/Evolution.h>

luna::runtime::MoonRuntime runtime;
luna::runtime::MoonRuntime::StagedGeneration staged;
std::string error;

bool ok = runtime.stage(request, verifier, resolver, initializer,
                        staged, error);
if (ok) {
    auto safePoint = runtime.safePoint();
    ok = runtime.activate(staged, safePoint, error);
}

auto pinned = runtime.pin(moduleId);
auto entry = pinned.find(requirement);

luna::runtime::MoonRuntime::SwitchableBinding switchable;
ok = runtime.makeSwitchable(moduleId, requirement, switchable, error);
auto currentEntry = switchable.pin();

auto rollbackPoint = runtime.safePoint();
ok = runtime.rollback(moduleId, oldGenerationId, rollbackPoint, error);

luna::runtime::RuntimeFragmentBindingSet selected;
ok = luna::runtime::makeRuntimeFragmentBindingSet(
    std::move(fragmentRefs), selected, error);
auto bindingPoint = runtime.safePoint();
ok = ok && runtime.activateFragmentBindings(
    selected, bindingPoint, error);
auto dispatchSnapshot = runtime.pinFragmentBindings();
luna::runtime::RuntimeFragmentExecutionContext executionContext;
ok = ok && luna::runtime::makeRuntimeFragmentExecutionContext(
    dispatchSnapshot, executionContext, error);
```

The compiler repository's Moon and Native generation adapters supply verified
artifact-specific verifier/resolver callbacks and retained loader/JIT leases.
The generic public `stage` entry is a trust boundary: an embedding host that
provides different callbacks is responsible for authenticating the artifact,
checking its target and descriptor ABI, and resolving only verified entries.
The API does not make an arbitrary implementation pointer trustworthy.

The compiler-owned Moon adapter accepts explicit `MoonJitOptimization::O0`,
`O2` or `O3` (default O0). These select the LLVM IR pipeline while ORC machine
code generation retains its defaults. Its materialization key includes the
adapter profile version, LLVM version, IR level and ORC policy. Target/layout,
integrity and dependency Slot evidence are still checked before cache reuse.
Unknown profile values and same-content/different-key requests fail without
changing publication or the output handle. The key is loader attestation, not
a signature or a safety proof; trusted native loaders must describe all their
code-generation choices faithfully. Native adapter behavior is unchanged.

## Failure and lifetime rules

Verification, resolution, initialization, compatibility checking, activation,
or rollback failure leaves the previous active generation unchanged. An
initializer runs during staging and may have external effects; Luna does not
reverse those effects if staging or later activation fails. Initializers should
therefore be host-controlled and designed with explicit failure behavior.

All activated generations and their module leases remain retained for 0.3.
Rollback only republishes a retained generation. There is no persistent-state
migration, automatic update discovery, implicit path-based identity, code
reclamation, hotspot JIT policy, or cross-target container activation in this
version.

See [the 0.3 overall design](luna_0.3_design.md#c014-moonruntime-owns-evolution-confirmed-direction)
for the EV001–EV004 decisions and [testing](testing.md) for the executable
state-machine and real-artifact evidence.
