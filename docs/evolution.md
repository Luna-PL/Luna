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

A typed binding requirement consists of `symbolId`, `contractId`,
`declarationKind`, and `requiredFlags`. Once a switchable binding is created,
the runtime preserves the active binding's exact kind and flags as the minimum
requirement for every later activation. Compatibility is checked before the
new immutable generation pointer is published, never on each ordinary call.

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
```

The compiler repository's Moon and Native generation adapters supply verified
artifact-specific verifier/resolver callbacks and retained loader/JIT leases.
The generic public `stage` entry is a trust boundary: an embedding host that
provides different callbacks is responsible for authenticating the artifact,
checking its target and descriptor ABI, and resolving only verified entries.
The API does not make an arbitrary implementation pointer trustworthy.

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
