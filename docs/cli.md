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

The version 1 envelope and record meanings are stable, while string vocabularies such as
`symbol_kind` are open enumerations. A consumer must preserve an unknown value when possible
or degrade it to an unknown/generic symbol; an unfamiliar kind must not invalidate the JSONL
stream. A new mandatory field or changed existing meaning requires a protocol version bump.

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
luna build <package> [-O0|-O2|-O3] [options]
```

Formal artifact builds require a directory containing `luna.package`; its
manifest must explicitly select `kind = "application"` or `kind = "library"`.
Standalone files remain valid for `check`, `run`, and `analyze`, but `build`
rejects them. `-t native` is the default. A Native application writes
`<package-root>/build/native/<last-package-component>` plus sibling textual
LLVM IR; Windows adds `.exe` to the executable. Native libraries fail closed
unless their manifest kind is `library` and their public surface contains only
ordinary Luna exports. They emit `lib<name>.so` (`.dylib` on macOS or
`<name>.dll` on Windows), sibling textual IR, and an embedded proof section.
The build also writes `<library>.trust` as an installation candidate. It is not
searched or trusted automatically: a trusted installer must place the exact
record in an explicit trust store before a future Runtime loader may accept the
library. Each library also exports a versioned typed descriptor registry. Its
rows are bound by the proof's export digest and use SymbolId/ContractId
identity. The internal verified-loader primitive uses immutable/private staging
and publishes only proof-matched registry entries. EV004 exposes process-local
generation control through the C++17 host API, not an end-user load/activation
CLI; raw dynamic-loader symbol lookup is not a trusted loading path.

`luna build <package-directory> -t moon` requires an explicit manifest `kind`
and emits a self-verified, host-specific `.moon`. `-o` overrides the path;
otherwise the output is
`<package-root>/build/moon/<last-package-component>.moon`.
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
the header keeps the package-derived name beside it. The same package cannot
silently become trusted Native code: `export "C" fn` is rejected from a Native
public surface.

Development builds default to their own `libruntime.a` and `clang++`. Installed,
packaged or cross-environment builds should pass `--runtime-lib` and `--cc`, or
set `LUNA_RUNTIME_LIB` and `LUNA_CXX`. A missing runtime reports `DRV0001`; a
native linker failure reports `DRV0002`.

Native and CFFI links keep a sibling `<artifact>.luna-link-state`. On a repeat
build, Luna retains inspectable textual IR, emits a sibling native `.o`, and
passes that object to the native platform linker. A recognized x86-64
MinGW/Clang executable toolchain invokes its companion `ld.lld` directly to
avoid a redundant driver process; shared libraries, searched `-l` inputs,
driver environment overrides, and other toolchain layouts continue through
the selected clang driver. On that direct path, lld streams the executable to
Luna; Luna hashes it while writing a sibling pending file and publishes it only
after successful process completion. Luna prints `Up to date:`
only when the generated IR and object are byte-identical, the full link command
is unchanged, and the compiler, Runtime archive, object, and path-based link
libraries are no newer than the artifact. Direct-link toolchain files are also
tracked. Native applications additionally use
the guarded pre-frontend fingerprint described in the benchmark guide. Deleting
the state file or any required output forces the corresponding normal path.
Fresh sibling outputs and state files are published by same-directory rename;
changed existing outputs retain the overwrite-and-content-validation path.

### REPL

```sh
luna repl
```

The Alpha REPL deliberately exposes a narrow, tested contract:

```text
= 20 + 22
:decl fn twice(value: i32) -> i32 { return value + value; }
:type twice(1)
= twice(21)
print(7)
:undo
:reset
:quit
```

- `= <expression>` evaluates an expression whose result must be `i32`.
- `:decl <declaration>` validates and persists one complete single-line
  declaration. Validation includes LLVM code generation and a synthetic REPL
  entry point; failed declarations are not committed. Persisted declarations
  are recompiled with later submissions.
- `:type <expression>` reports the inferred type without executing the expression.
- `:paste decl`, `:paste expr`, or `:paste stmt` reads an explicit multiline
  cell terminated by a line containing only `:end`.
- `:load <path>` validates a source file as one declaration cell. Diagnostics
  retain the real source path and excerpts.
- Other input executes as one statement in a temporary `main`.
- `:undo` removes the last committed declaration. `:help`, `:reset` and
  `:quit` display the contract, discard declarations and exit respectively.
  `exit` remains a compatibility spelling for `:quit`.

This is an **Implemented Experimental** tool, not a persistent runtime. Local
variables, heap values, JIT globals and runtime state do not survive a
submission. Every declaration validation, type query, expression and statement
is compiled in a fresh worker process; executable cells are also JITed and run
there. While waiting for input, the session keeps exactly one unused,
single-shot worker prewarmed behind its containment gate. A cell is sent as a
bounded, length-delimited operation/path/source request; that worker consumes
only that request. Before publishing readiness, each worker initializes LLVM's
immutable target registries; no compiler, JIT, or user state is shared with
another cell. The request and the worker's bounded running/final result records
travel over inherited data channels; the parent drains result records
while the worker runs. After execution the worker flushes the cell's output and
publishes completion. Readiness, containment-gate and completion notifications use native
Windows Events or inherited POSIX socket pairs, not polled control files.
Captured stdout/stderr use separately drained channels with one shared byte
budget, so the REPL no longer needs per-cell temporary files or file-size
polling. Internal protocol endpoints are made non-inheritable before linked code runs. A
background reaper permits at most two completed workers and gives
each no more than 500 ms from publication to exit normally before terminating
its contained process tree. Preparation of the replacement starts
asynchronously at publication. Thus interactive think time can hide
process and LLVM-library startup without carrying compiler, JIT, linked-library
or runtime state between cells. Scripted back-to-back cells may reach the next
worker before it is ready and still pay the remaining startup cost. An idle
worker monitors the parent and exits if the session disappears. Process-exit
hooks from linked libraries are best-effort cleanup, not cell output semantics;
they must finish within the same 500 ms retirement grace.

A compiler crash, runtime abort or execution timeout therefore does not
terminate the REPL session. `--timeout <seconds>` sets the complete per-cell
compile-and-execute limit from 1 to 3600 seconds (default 30).
`--memory-limit <MiB>` limits the worker process tree to 256–65536 MiB (default
1024), while `--output-limit <MiB>` caps captured stdout plus stderr to 1–1024
MiB (default 16). Descendants are grouped with the worker and terminated when
the worker is retired (within the 500 ms grace) or immediately when the cell is
cancelled. This is crash/resource containment, not an
operating-system security sandbox; only run source and linked libraries you
trust. AddressSanitizer-instrumented development builds disable the memory cap
because ASan must reserve a very large shadow address range; timeout, output and
process-tree containment remain active. Multiline input is deliberately explicit
rather than inferred from parser recovery. Use `--no-prompt` for
transcript-driven input. `--timings` writes one `timing[repl]` line per
`validate`, `type`, or `run` operation to stderr.
`cache=hit` denotes a successful exact-source `:type` result served from the
bounded 32-entry/1 MiB session cache; `cache=miss` denotes the worker path.
Declaration changes, `:undo`, and `:reset` invalidate that cache. Validation and
execution results are never cached, so executable cells retain fresh-worker
isolation and repeat their side effects. On a cache hit, `prewarmed=unused` and
all worker phase fields are zero.
`prewarmed=yes` means the selected worker had reached its gate before that cell
was submitted; `prewarmed=no` means `total` includes the remaining startup wait.
`frontend` is the aggregate source-analysis and MoonIR duration. Its detailed
fields are `lexer`, `parser`, `semantic`, `traits`, `ownership`, `indexing`,
`lowering`, `verification`, `sealing`, and `moon-opt`; their rounded sum need not
exactly equal the aggregate. `codegen` covers LLVM generation,
`codegen-setup` covers generator construction and any target initialization not
already completed during worker startup, and `codegen-total` is their complete
envelope.
`jit-materialize` and `jit-lookup` expose ORC JIT setup, `execution` is only the
user entry-point call, `jit-cleanup` covers LLJIT teardown, and `jit-total` is
the complete `jitRun()` envelope. `overhead` is the
parent-observed time outside `worker-total`, and `total` is the complete
parent-observed submission latency. `worker-request` covers request receipt and
decoding, `worker-link` covers requested dynamic-library loading,
`worker-running-publish` covers the pre-execution Running frame,
`worker-total` runs from request-channel servicing until immediately before the
final result is serialized, and `worker-other` is the part of that interval not
assigned to request, link, frontend, the codegen envelope, Running publication, or the JIT
envelope. A zero detailed
phase can mean it completed below timer resolution or was not reached. Idle
preparation, worker teardown and replacement creation that continue outside the
submission are not charged to `total`.
The parent-side latency timeline is reported separately: `parent-cache` is the
complete cache-hit path; `parent-acquire` obtains the prepared worker,
`parent-submit` finishes readiness/containment and sends the request,
`parent-roundtrip` runs from submission until completion observation and thus
overlaps the worker phases, `parent-collect` drains and validates the result, and
`parent-replenish` queues retirement and the replacement worker. These parent
fields partition `total` on successful operations; they must not be added to the
worker phase fields. `parent-roundtrip-gap` subtracts `worker-total` from the
roundtrip and therefore isolates final-result serialization/publication, signal
wakeup, scheduling and cross-process measurement noise rather than one single
code phase.
On Windows, the memory limit is an aggregate Job Object limit. On Linux, the
worker installs a non-raiseable per-process `RLIMIT_AS` before readiness and its
descendants inherit that limit. Darwin rejects limits below the address space
already mapped at startup, so macOS adds the selected allowance to that initial
mapping footprint before installing the same non-raiseable limit.
`-O0/-O2/-O3`, `--opt`, `--link`, `--timeout`, `--memory-limit`,
`--output-limit`, `--timings`, and `--help` are parsed as normal REPL CLI options.

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
| `--moon-cost-report` | `run`, `build` | Print explicit runtime, instantiation, and kernel cost decisions. |
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
luna build path/to/application-package -O2 \
  --runtime-lib /opt/luna/lib/libruntime.a \
  --cc /usr/bin/clang++ \
  --link m
```
