# Luna full showcase

This workspace is the executable integration example for Luna 0.3.0 development.
It deliberately uses two packages and several modules instead of placing every
feature in one source file.

```text
full_showcase/
├── luna.workspace
├── luna.lock
├── foundation/  # org.luna.examples.showcase.foundation
│   └── src/{model,algorithms,dispatch,effects,device}.luna
└── app/         # org.luna.examples.showcase.app
    └── src/{main,foreign}.luna
```

Run it with the default CPU kernel simulator:

```sh
./build/luna check examples/full_showcase/app
LUNA_GPU_BACKEND=sim ./build/luna run examples/full_showcase/app -O2
LUNA_GPU_BACKEND=sim ./build/luna build examples/full_showcase/app -O2
LUNA_GPU_BACKEND=sim ./examples/full_showcase/app/org.luna.examples.showcase.app
```

The successful process exit code is `42`. The example covers the currently
implemented positive language surface: manifests/workspaces/locks, packages,
modules/submodules, aliases and exports; signature/local inference, constexpr
and generics; nominal named structs/enums, explicit shape relations, and
anonymous records; static traits; compile-time reflection; metadata with
static/dynamic selection and runtime retention; closures; arrays and slices;
ownership, affine values, move/borrow and linear kernel events; fragments,
slots, static/dynamic apply and continuations; C ABI declarations; and a
reachable heterogeneous kernel. Invalid programs and external plugin loading
remain in the dedicated regression fixtures because they cannot be demonstrated
by one successful self-contained executable.
