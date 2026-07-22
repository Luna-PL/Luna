# Luna compiler command reference

[English](cli.md) | [简体中文](cli.zh-CN.md)

The `luna` driver accepts either one standalone `.luna` source file or a
package directory. Running `luna` without arguments prints the built-in usage
summary. The current Alpha does not yet provide a dedicated `--help` alias.

## Commands

### Version

```sh
luna --version
luna -V
luna version
```

All forms print the compiler release version.

### Check

```sh
luna check <file-or-package> [--emit-moonir <path>]
```

`check` runs lexing, parsing, semantic analysis, trait checking, ownership
checking, MoonIR lowering, optimization and verification. It does not invoke
LLVM code generation, making it the normal command for library packages.

### JIT run

```sh
luna run <file-or-package> [-O0|-O2|-O3] [options]
```

`run` lowers verified MoonIR to LLVM, JIT-compiles the program and executes
`main`. The driver's own process exit status is the Luna program's status.

### AOT build

```sh
luna build <file-or-package> [-O0|-O2|-O3] [options]
```

For a file input, `build app.luna` writes `app.luna.ll` and `app`. For a package
directory, it writes `<package-id>.ll` and `<package-id>` inside that directory.
Windows adds `.exe` to the executable. See [AOT builds](aot_build.md).

### REPL

```sh
luna repl
```

The current REPL wraps each entered statement in a temporary `main` and runs it
through JIT. It is experimental and is not yet a persistent declaration-based
interactive environment.

## Common options

| Option | Commands | Meaning |
|---|---|---|
| `-O0`, `-O2`, `-O3` | `run`, `build` | Select MoonIR and LLVM optimization level. The default is `-O0`. |
| `--opt O2` | `run`, `build` | Long form of the optimization option; `--opt=O2` is also accepted. |
| `--link <library>` | `run`, `build` | Load a JIT shared library or add an AOT linker dependency. Repeatable. |
| `--emit-moonir <path>` | `check`, `run`, `build` | Write verified, optimized textual MoonIR. |
| `--moon-cost-report` | `run`, `build` | Print explicit runtime, dynamic, instantiation and kernel cost decisions. |
| `--gpu-target <list>` | `run`, `build` | Generate requested device code objects. Accepts comma-separated targets. |
| `--reserve-kernel-runtime` | `run`, `build` | Retain kernel runtime capability even without a reachable launch. |
| `--runtime-lib <path>` | `build` | Select the Luna `libruntime.a` used for AOT linking. |
| `--cc <compiler>` | `build` | Select the C++ linker driver. |

Options accepting values also support `--name=value`.

## GPU targets and runtime backends

Device artifact generation and execution backend selection are deliberately
separate:

```sh
LUNA_GPU_BACKEND=rocm luna run app.luna -O2 \
  --gpu-target=rocm:gfx1101
```

`--gpu-target` accepts `sim`, `cuda[:sm_*]` and `rocm[:gfx*]`. Multiple targets
may be comma-separated. Omitting a hardware target avoids paying its code-object
generation cost.

At execution time, `LUNA_GPU_BACKEND` selects `sim`, `cuda` or `rocm`; the
default is `sim`. Selecting an unavailable backend fails explicitly and does
not silently fall back. See [Heterogeneous compute](heterogeneous_compute.md).

## Environment variables

| Variable | Purpose |
|---|---|
| `LUNA_RUNTIME_LIB` | Default AOT Runtime ABI library when `--runtime-lib` is absent. |
| `LUNA_CXX` | Default AOT linker driver when `--cc` is absent. |
| `LUNA_GPU_BACKEND` | Runtime GPU backend: `sim`, `cuda` or `rocm`. |
| `LUNA_GPU_PROFILE=1` | Print accumulated CUDA/ROCm device-event kernel time. |
| `LUNA_GPU_DUMP_HSACO=<dir>` | Save generated ROCm HSACO files for inspection. |
| `LUNA_FRAGMENT_PLUGIN=<path>` | Load an Alpha v1 external fragment plugin. |
| `LUNA_FRAGMENT_<SLOT>=<name>` | Choose a linked dynamic fragment candidate for a slot. |

Plugin constraints and environment-driven selection are described in
[External fragment plugins](dynamic_plugins.md).

## Useful workflows

Check a library package and inspect MoonIR:

```sh
luna check path/to/library --emit-moonir library.moonir
```

Compare explicit costs while running the portable simulator:

```sh
LUNA_GPU_BACKEND=sim luna run examples/full_showcase/app -O2 \
  --moon-cost-report
```

Build an installed compiler's AOT executable reproducibly:

```sh
luna build app.luna -O2 \
  --runtime-lib /opt/luna/lib/libruntime.a \
  --cc /usr/bin/clang++ \
  --link m
```
