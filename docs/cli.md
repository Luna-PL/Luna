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
           [--message-format=json]
```

`check` runs lexing, parsing, semantic analysis, trait checking, ownership
checking, MoonIR lowering, optimization and verification. It does not invoke
LLVM code generation, making it the normal command for library packages.

`--message-format=json` switches the command to `luna.diagnostic` JSONL version
1 for editor and CI integration. The stdout stream contains exactly one
`hello`, zero or more `diagnostic` records, and one `summary`; stderr remains
empty. Disk-backed locations use absolute paths and UTF-8 byte offsets with an
exclusive end. Exit status is `0` for no errors, `1` for reported diagnostics,
and `2` for command/protocol misuse.

### Analyze

```sh
luna analyze <file-or-package> --message-format=json
luna analyze <file-or-package> --message-format=json \
    --overlay <document> < current-buffer.luna
luna analyze <file-or-package> --message-format=json \
    --overlays-from-stdin < overlays.json
```

`analyze` emits a compiler-owned `luna.analysis` version 1 semantic snapshot:
one `hello`, declarations, resolved direct-function and user trait-method
calls, user type-syntax and trait references, struct field accesses, and enum
variant construction/match records, followed by one `summary`. With
`--overlay` retains the original transport in which stdin replaces one existing
source file. `--overlays-from-stdin` reads a `luna.overlay` version 1 JSON object:

```json
{
  "protocol": "luna.overlay",
  "version": 1,
  "overlays": [
    {"path": "/workspace/src/api.luna", "text": "..."},
    {"path": "/workspace/src/main.luna", "text": "..."}
  ]
}
```

All listed files atomically replace existing sources in the selected root
package without temporary files. Duplicate, foreign, or dependency paths are
rejected. Other files and dependencies follow normal resolution. Clients must
check `single-document-overlay` or `multi-document-overlay` before selecting a
transport.

### JIT run

```sh
luna run <file-or-package> [-O0|-O2|-O3] [options]
```

`run` lowers verified MoonIR to LLVM, JIT-compiles the program and executes
`main`. The driver's own process exit status is the Luna program's status.

### Artifact build

```sh
luna build <file-or-package> [-O0|-O2|-O3] [options]
```

`-t native` is the default. For a file input, `build app.luna` writes
`app.luna.ll` and `app`. For a package
directory, it writes `<package-id>.ll` and `<package-id>` inside that directory.
Windows adds `.exe` to the executable.

`luna build <package-directory> -t moon` requires an explicit manifest `kind`
and emits a self-verified, host-specific `.moon`. `-o` overrides the path;
otherwise the output is
`<package-root>/build/<host-target>/<last-package-component>.moon`.
Applications require exactly one package `main`, libraries require none, and
standalone sources and native linker/GPU artifact options are rejected. Generic
recipes may remain in compiler input, but only concrete instances are emitted;
an exported or entrypoint generic recipe is rejected.

`luna build <package-directory> -t cffi` accepts only a manifest-declared
library package and requires at least one `export "C" fn`. Ordinary `export fn`,
applications, package `main`, an empty C export set, and standalone sources are
rejected. The default output is
`<package-root>/build/cffi/lib<last-package-component>.so` (`.dylib` on macOS,
or `<last-package-component>.dll` on Windows) plus a sibling
`<last-package-component>.h`. `-o` overrides the complete shared-library path;
the header keeps the package-derived name beside it. Native library builds are
explicitly rejected until proof-section emission is implemented; they never
downgrade to an unproved shared library or executable.

Development builds default to their own `libruntime.a` and `clang++`. Installed,
packaged or cross-environment builds should pass `--runtime-lib` and `--cc`, or
set `LUNA_RUNTIME_LIB` and `LUNA_CXX`. A missing runtime reports `DRV0001`; a
native linker failure reports `DRV0002`.

### REPL

```sh
luna repl
```

The Alpha REPL deliberately exposes a narrow, tested contract:

```text
= 20 + 22
:decl fn twice(value: i32) -> i32 { return value + value; }
= twice(21)
print(7)
:reset
:quit
```

- `= <expression>` evaluates an expression whose result must be `i32`.
- `:decl <declaration>` validates and persists one complete single-line
  declaration. Persisted declarations are recompiled with later submissions.
- Other input executes as one statement in a temporary `main`.
- `:help`, `:reset` and `:quit` display the contract, discard declarations and
  exit respectively. `exit` remains a compatibility spelling for `:quit`.

This is an **Implemented Experimental** tool, not a persistent runtime. Multiline
input is unsupported. Local variables, heap values, JIT globals and runtime
state do not survive a submission. A declaration that cannot be represented on
one line should be placed in a source file and run with `luna run`.

## Common options

| Option | Commands | Meaning |
|---|---|---|
| `-O0`, `-O2`, `-O3` | `run`, `build` | Select MoonIR and LLVM optimization level. The default is `-O0`. |
| `--opt O2` | `run`, `build` | Long form of the optimization option; `--opt=O2` is also accepted. |
| `--link <library>` | `run`, `build` | Load a JIT shared library or add an AOT linker dependency. Repeatable. |
| `-t native|moon|cffi` | `build` | Select native (default), a host-specific Moon Container, or a C ABI shared library plus header. |
| `-o <path>` | `build` | Override the native, Moon, or primary CFFI artifact output path. |
| `--emit-moonir <path>` | `check`, `run`, `build` | Write verified, optimized textual MoonIR. |
| `--message-format=json` | `check`, `analyze` | Emit the command's versioned JSONL protocol. |
| `--overlay <document>` | `analyze` | Read one in-memory source replacement from stdin. |
| `--overlays-from-stdin` | `analyze` | Read a versioned multi-document overlay JSON object from stdin. |
| `--moon-cost-report` | `run`, `build` | Print explicit runtime, dynamic, instantiation and kernel cost decisions. |
| `--gpu-target <list>` | `run`, `build` | Generate requested device code objects. Accepts comma-separated targets. |
| `--reserve-kernel-runtime` | `run`, `build` | Retain kernel runtime capability even without a reachable launch. |
| `--runtime-lib <path>` | `build` | Select the Luna `libruntime.a` used for AOT linking. |
| `--cc <compiler>` | `build` | Select the C++ linker driver. |

Options accepting values also support `--name=value`.

`-O0` keeps the most direct experimental control-flow IR and is the Alpha
default. `-O2` runs the standard LLVM speed pipeline; `-O3` selects the more
aggressive pipeline and gives small straight-line, call-free while loops a
bounded four-way unroll hint. Host modules are verified before and after
optimization, and AOT passes the same level to the native compiler. Device
kernels use a separate target-specific O3 pipeline.

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
[Fragments and plugins](fragments.md).

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
