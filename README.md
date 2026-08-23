# Luna 0.3.0 Development

[English](README.md) | [简体中文](README.zh-CN.md)

[![Linux CI](https://github.com/Luna-PL/Luna/actions/workflows/linux-ci.yml/badge.svg)](https://github.com/Luna-PL/Luna/actions/workflows/linux-ci.yml)
[![macOS CI](https://github.com/Luna-PL/Luna/actions/workflows/macos-ci.yml/badge.svg)](https://github.com/Luna-PL/Luna/actions/workflows/macos-ci.yml)
[![Windows CI](https://github.com/Luna-PL/Luna/actions/workflows/windows-ci.yml/badge.svg)](https://github.com/Luna-PL/Luna/actions/workflows/windows-ci.yml)

Luna is an experimental, static-first systems programming language developed by
the [Luna-PL](https://github.com/Luna-PL) organization. It targets high
performance without charging for unused capabilities: ordinary code is AOT/JIT
compiled through LLVM, while ownership, runtime binding and future evolution
support are explicit choices. The compiler preserves language semantics in
verified MoonIR before LLVM lowering.

The project is implementing a clean-break `0.3.0` line. The 0.3 compiler
does not carry a `language=` or edition compatibility mode; source that depends
on 0.2.1 semantics should use the frozen 0.2.1 compiler. The design remains a
draft and the implementation is advancing in gated phases.
[Read the 0.3 overall design](docs/luna_0.3_design.md) and
[migration guide](docs/migration_0.2_to_0.3.md).

## Current development surface

Luna combines static, zero-overhead language mechanisms with explicitly
opt-in runtime capabilities:

- verified `Luna -> MoonIR -> LLVM IR` compilation for both JIT and AOT;
- nominal named types, structural anonymous records, ADTs, constrained generics and statically resolved traits;
- affine/linear ownership, `move`, shared/mutable borrowing and automatic cleanup;
- first-class Metadata, compile-time selectors, and explicit runtime visibility;
- reverse-DNS Package IDs, `::` module paths, workspaces, lockfiles, aliases and explicit exports;
- experimental structured `interceptor`, `context`, `slot` and `apply` control-flow/extension constructs;
- a versioned Runtime ABI and an explicit C FFI boundary;
- reachable-only kernels with a CPU simulator and CUDA/ROCm code-generation paths.

The 0.3 migration has completed the Sema split, nominal named-type default,
usage blocks, generic Resource/Drop contracts, and library-owned `Rc`/`Arc`.
The single MoonIR now seals executable function bodies to canonical tables and
CFG unconditionally. The currently supported surface passes the complete
56-test gate, including per-element move-only iterator cleanup. Finite linked runtime
contexts and replay-safe multi-shot continuations now run canonically; persistent
external-plugin continuation callbacks remain outside plugin ABI v1. Legacy
structured executable-body codegen has been deleted, and the backend rejects
unsealed bodies at its boundary. Host-specific Moon Containers now support
deterministic serialization, atomic verified loading, and
`luna build <package> -t moon`. `-t cffi` now emits a C ABI shared library and
header and is gated through a real C consumer. The evolution runtime and trusted
native proof format remain unimplemented. Legacy `dynamic` source forms
that remain in the development compiler are migration input, not the 0.3
phase model or a compatibility promise.

[Explore the feature overview](docs/features.md), or open the [complete executable showcase](examples/full_showcase/README.md).

## CPU microbenchmark snapshot

On 2026-08-11 at commit `6838788`, the local Ryzen 5 7500F validation host ran
Luna AOT and Clang C++23 with LLVM/Clang 22.1.6 at `-O3`. The table reports
median end-to-end execution time over 10 measured runs after two warmups;
lower is better, and `Luna/C++23` is a time ratio.

| Workload | Luna AOT | C++23 | Luna/C++23 |
|---|---:|---:|---:|
| Integer recurrence | 4.189 ms | 4.238 ms | 0.99x |
| Branch-heavy loop | 26.002 ms | 26.047 ms | 1.00x |
| Inlineable calls | 4.210 ms | 4.224 ms | 1.00x |
| Safe fixed array | 9.278 ms | 9.201 ms | 1.01x |
| Bit mixing | 35.879 ms | 36.218 ms | 0.99x |
| Four-chain reduction | 8.982 ms | 10.805 ms | 0.83x |
| Stateful array scan | 13.096 ms | 15.081 ms | 0.87x |
| Nested loops | 4.651 ms | 4.256 ms | 1.09x |
| Allocation boundary† | 15.257 ms | 5.747 ms | 2.65x |

> **Important:** these are highly idealized microbenchmarks. They cannot
> establish the performance gap in real applications. Both paths share the
> LLVM backend family, process startup is included, and the tiny working sets
> do not represent application memory,
> concurrency, I/O or latency behavior. CPU frequency and core placement were
> not pinned. †The current allocation workload forces both allocation paths to
> execute, but Luna Runtime ABI ownership and the separate C++ allocator adapter
> are still different abstractions, so that row is not an allocator ranking.

[Read the workload definitions, caveats and CPU/GPU reproduction steps](docs/benchmarks.md).

## Hello, World

A Luna program is a `.luna` source file with a `main` function:

```luna
fn main() -> i32 {
    print("Hello, Luna!");
    return 0;
}
```

Save it as `hello/src/main.luna`. Standalone files can be checked or run; a
formal artifact additionally needs `hello/luna.package`:

```toml
[package]
id = "org.example.hello"
version = "1.0.0"
kind = "application"
sources = ["src"]
```

```sh
./build/luna check hello/src/main.luna
./build/luna run hello/src/main.luna -O2
./build/luna build hello -O2
./hello/build/native/hello
```

`print` is currently the minimal language/runtime output operation. A temporary,
typed `std::io` package already exposes console reads/writes, flush and basic
integer parsing; the final `Read`/`Write`/formatting surface remains design work.
Continue with the [English getting-started guide](docs/getting_started.md).

## Build from source

The validated development toolchain is LLVM/Clang 22, CMake 3.20 or newer, a C++17 compiler, and optionally Ninja. On Linux and macOS:

```sh
LLVM_DIR="$(llvm-config --cmakedir)"
cmake -S . -B build -G Ninja \
  -DCMAKE_C_COMPILER="$(command -v clang)" \
  -DCMAKE_CXX_COMPILER="$(command -v clang++)" \
  -DLLVM_DIR="$LLVM_DIR"
cmake --build build --parallel
ctest --test-dir build -LE hardware --output-on-failure
```

See the [complete build and installation guide](docs/getting_started.md).
Windows uses the MSYS2 UCRT64 toolchain documented in the [Windows build guide](docs/windows_build.md).

## Compiler commands

The `luna` driver accepts either a standalone `.luna` file or a package
directory containing `luna.package`:

| Command | Purpose |
|---|---|
| `luna --version` | Print the compiler version. |
| `luna check <input>` | Verify source through MoonIR without generating machine code. |
| `luna analyze <input> --message-format=json` | Emit a semantic tooling snapshot; optionally read one or more source overlays from stdin. |
| `luna run <input> [-O0\|-O2\|-O3]` | JIT-compile and execute a program. |
| `luna build <package> [-O0\|-O2\|-O3] [-t native\|moon\|cffi]` | Produce the selected native, Moon Container, or CFFI artifact. |
| `luna repl` | Start the limited Alpha REPL (`=`, `:decl`, single-line statements). |

The driver also exposes explicit linker, runtime, MoonIR, cost-report and GPU target options.
Runtime backend selection is separate from code-object generation.
The 0.3 `-t native|moon|cffi` artifact selector is now a driver option. Native
library builds fail explicitly until proof-section emission is implemented;
they never downgrade to an unproved artifact.
See the [compiler command reference](docs/cli.md) for commands, options, environment variables and examples.
Repository contributors should use the [file and responsibility guide](docs/file_guide.md)
before adding or moving implementation files.

## Platform and test status

Linux and macOS are Luna's primary targets. Windows is supported through MSYS2 UCRT64. The stable core is exercised by native CI on all three platforms, and all repository changes are tested locally on Linux.

The current local hardware validation environment is:

- OS: Arch Linux x86-64 (`7.0.14-arch1-1`)
- CPU: AMD Ryzen 5 7500F (x86-64)
- GPU: AMD Radeon RX 7800 XT (`gfx1101` / Navi 32)
- RAM: 32 GiB DDR5

Luna supports native heterogeneous computation.
The CPU simulator and AMD ROCm paths have been tested on the available hardware;
CUDA code generation exists but still requires broader NVIDIA hardware validation.
See the [testing guide](docs/testing.md) and [heterogeneous-compute guide](docs/heterogeneous_compute.md).

## Next evolution

The immediate work follows the 0.3 completion gates rather than the frozen 0.2
Alpha roadmap:

1. implement the trusted Luna-native proof section, trust records, and loader;
2. implement the minimal Moon
   staging/activation runtime without adding ordinary-call hot-path cost;
3. converge Slot/Fragment and runtime query surfaces after their remaining
   syntax/ordering decisions are frozen;
4. update formatter, LSP, packaging, benchmarks and release gates against only
   the final 0.3 semantics.

See the [0.3 implementation priorities](docs/luna_0.3_design.md#9-implementation-priority).

## Documentation

Each topic starts with a short overview and links onward to its detailed design
or reference material:

- [Getting started](docs/getting_started.md)
- [Feature overview](docs/features.md)
- [Compiler command reference](docs/cli.md)
- [Full language showcase](examples/full_showcase/README.md)
- [0.2 Alpha semantic reference](docs/reference/README.md)
- [Complete type-system reference](docs/reference/type_system.md)
- [Documentation recording rules](docs/reference/documentation_rules.md)
- [Packages and modules](docs/packages.md)
- [Metadata and selectors](docs/versioning.md)
- [Architecture](docs/architecture.md)
- [Architecture decisions](docs/decisions.md)
- [Luna 0.3 overall design draft](docs/luna_0.3_design.md)
- [Fragments and slots](docs/fragments.md)
- [Runtime ABI and C FFI](docs/runtime_abi.md)
- [Heterogeneous compute](docs/heterogeneous_compute.md)
- [Standard-library design](docs/standard_library.md)
- [Testing](docs/testing.md)
- [Ecosystem release snapshot](docs/ecosystem_release.md)
- [Performance benchmarks](docs/benchmarks.md)
- [0.3 overall design and implementation priorities](docs/luna_0.3_design.md)

## License

Unless a source file states otherwise, Luna is distributed under either
the [MIT License](LICENSE-MIT) or [Apache License 2.0](LICENSE-APACHE), at your option.
