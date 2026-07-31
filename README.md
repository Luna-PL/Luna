# Luna 0.2.0-alpha

[English](README.md) | [简体中文](README.zh-CN.md)

[![Linux CI](https://github.com/Luna-PL/Luna/actions/workflows/linux-ci.yml/badge.svg)](https://github.com/Luna-PL/Luna/actions/workflows/linux-ci.yml)
[![macOS CI](https://github.com/Luna-PL/Luna/actions/workflows/macos-ci.yml/badge.svg)](https://github.com/Luna-PL/Luna/actions/workflows/macos-ci.yml)
[![Windows CI](https://github.com/Luna-PL/Luna/actions/workflows/windows-ci.yml/badge.svg)](https://github.com/Luna-PL/Luna/actions/workflows/windows-ci.yml)

Luna is an experimental LLVM-backed systems programming language developed by
the [Luna-PL](https://github.com/Luna-PL) organization. Its compiler preserves
language semantics in verified MoonIR before lowering to LLVM for JIT or AOT
compilation.

The project is entering a long-lived `0.2.0-alpha` maintenance line for
language research and internal experiments. There is no scheduled Beta or
language-version bump. Near-term development is focused on the toolchain:
diagnostics, editor/build/package integration, distribution, reproducibility
and reliability. Language work is limited to correctness, safety and closing
documented contract gaps; experimental syntax may still change within that
boundary.
[Read the Alpha release notes](docs/alpha_release.md) for the precise support
boundary and known limitations.

## Key features

Luna combines static, zero-overhead language mechanisms with explicitly
opt-in runtime and dynamic capabilities:

- verified `Luna -> MoonIR -> LLVM IR` compilation for both JIT and AOT;
- structural types by default, explicit nominal identity, ADTs, generics and statically resolved traits;
- affine/linear ownership, `move`, shared/mutable borrowing and automatic cleanup;
- first-class Metadata, compile-time selectors, and explicit `runtime` / `dynamic` retention and dispatch;
- reverse-DNS Package IDs, `::` module paths, workspaces, lockfiles, aliases and explicit exports;
- structured `interceptor`, `context`, `slot` and `apply` control effects;
- a versioned Runtime ABI and an explicit C FFI boundary;
- reachable-only kernels with a CPU simulator and CUDA/ROCm code-generation paths.

[Explore the feature overview](docs/features.md), or open the [complete executable showcase](examples/full_showcase/README.md).

## CPU microbenchmark snapshot

On the local Ryzen 5 7500F validation host, Luna AOT and Clang C++23 were both
built with LLVM/Clang 22.1.6 at `-O3`. The table reports median end-to-end
execution time over 10 measured runs after two warmups; lower is better, and
`Luna/C++23` is a time ratio.

| Workload | Luna AOT | C++23 | Luna/C++23 |
|---|---:|---:|---:|
| Integer recurrence | 4.127 ms | 4.066 ms | 1.02x |
| Branch-heavy loop | 25.665 ms | 26.248 ms | 0.98x |
| Inlineable calls | 4.005 ms | 3.950 ms | 1.01x |
| Safe fixed array | 8.811 ms | 9.086 ms | 0.97x |
| Bit mixing | 35.232 ms | 35.711 ms | 0.99x |
| Four-chain reduction | 13.761 ms | 10.851 ms | 1.27x |
| Stateful array scan | 14.192 ms | 14.577 ms | 0.97x |
| Nested loops | 4.489 ms | 4.137 ms | 1.09x |
| Allocation boundary† | 15.320 ms | 3.490 ms | 4.39x |

> **Important:** these are highly idealized microbenchmarks. They cannot
> establish the performance gap in real applications. Both paths share the
> LLVM backend family, process startup is included, CPU frequency was not
> pinned, and the tiny working sets do not represent application memory,
> concurrency, I/O or latency behavior. †Clang eliminates the C++ `new/delete`
> pair in the allocation case while Luna deliberately retains its Runtime ABI
> allocation, so that row is not an allocator ranking.

[Read the workload definitions, caveats and CPU/GPU reproduction steps](docs/benchmarks.md).

## Hello, World

A Luna program is a `.luna` source file with a `main` function:

```luna
fn main() -> i32 {
    print("Hello, Luna!");
    return 0;
}
```

Save it as `hello.luna`, then check it, run it through the JIT, or build a
native executable:

```sh
./build/luna check hello.luna
./build/luna run hello.luna -O2
./build/luna build hello.luna -O2
./hello
```

`print` is currently the minimal language/runtime output operation; the future
formatted API will live in `std::io`. Continue with the [English getting-started guide](docs/getting_started.md).

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
| `luna run <input> [-O0\|-O2\|-O3]` | JIT-compile and execute a program. |
| `luna build <input> [-O0\|-O2\|-O3]` | Emit LLVM IR, link the Runtime ABI, and produce a native executable. |
| `luna repl` | Start the limited Alpha REPL (`=`, `:decl`, single-line statements). |

The driver also exposes explicit linker, runtime, MoonIR, cost-report and GPU target options.
Runtime backend selection is separate from code-object generation.
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

The immediate direction is toolchain development on the existing Alpha
language baseline:

1. improve diagnostics, source mapping and machine-readable output;
2. add formatter, language-server and editor integration;
3. complete build/test/package/workspace workflows and local caching;
4. harden installation, prebuilt distribution and reproducible builds;
5. improve benchmark, profiling, MoonIR inspection and developer-audit tools;
6. fix language correctness or safety defects without expanding the active
   language surface.

See the [single active roadmap](docs/roadmap.md).

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
- [Fragments and slots](docs/fragments.md)
- [Runtime ABI and C FFI](docs/runtime_abi.md)
- [Heterogeneous compute](docs/heterogeneous_compute.md)
- [Standard-library skeleton](docs/standard_library.md)
- [Testing](docs/testing.md)
- [Performance benchmarks](docs/benchmarks.md)
- [Roadmap](docs/roadmap.md)

## License

Unless a source file states otherwise, Luna is distributed under either
the [MIT License](LICENSE-MIT) or [Apache License 2.0](LICENSE-APACHE), at your option.
