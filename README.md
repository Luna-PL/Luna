# Luna 0.2.0-alpha

[English](README.md) | [简体中文](README.zh-CN.md)

[![Linux CI](https://github.com/Luna-PL/Luna/actions/workflows/linux-ci.yml/badge.svg)](https://github.com/Luna-PL/Luna/actions/workflows/linux-ci.yml)
[![macOS CI](https://github.com/Luna-PL/Luna/actions/workflows/macos-ci.yml/badge.svg)](https://github.com/Luna-PL/Luna/actions/workflows/macos-ci.yml)
[![Windows CI](https://github.com/Luna-PL/Luna/actions/workflows/windows-ci.yml/badge.svg)](https://github.com/Luna-PL/Luna/actions/workflows/windows-ci.yml)

Luna is an experimental LLVM-backed systems programming language developed by
the [Luna-PL](https://github.com/Luna-PL) organization. Its compiler preserves
language semantics in verified MoonIR before lowering to LLVM for JIT or AOT
compilation.

The project is currently an Alpha compiler for language research and internal
experiments. APIs and experimental syntax may still change before Beta.
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
formatted API will live in `std::io`. Continue with the [English getting-started guide](docs/getting_started.en.md).

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

See the [complete build and installation guide](docs/getting_started.en.md).
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
| `luna repl` | Start the experimental JIT REPL. |

The driver also exposes explicit linker, runtime, MoonIR, cost-report and GPU target options.
Runtime backend selection is separate from code-object generation.
See the [compiler command reference](docs/cli.md) for commands, options, environment variables and examples.

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

The immediate direction is to finish the stable language foundation before
adding broad new surface area:

1. introduce first-class error handling and use it as the basis of the
   standard library;
2. complete package/module tooling and grow `org.luna.core` / `org.luna.std`,
   including formatted `std::io`;
3. refine ownership, affine/linear checking and the explicit cost model;
4. freeze Moon container validation and reserve MoonRuntime loading, JIT and
   hotspot evolution interfaces;
5. generalize heterogeneous memory, kernel targets and hardware validation;
6. improve diagnostics, formatting, language-server and package ecosystem
   tooling.

See the [English roadmap](docs/roadmap.md) and the [detailed evolution roadmap](docs/evolution_roadmap.md).

## Documentation

Each topic starts with a short overview and links onward to its detailed design
or reference material:

- [Getting started](docs/getting_started.en.md)
- [Feature overview](docs/features.md)
- [Compiler command reference](docs/cli.md)
- [Full language showcase](examples/full_showcase/README.md)
- [Packages and modules](docs/packages.md)
- [Types and structural/nominal identity](docs/types.md)
- [Metadata and selectors](docs/versioning.md)
- [Ownership and affine/linear design](docs/Arch/Ownership_Affine_Model_RFC.md)
- [Fragments and slots](docs/fragments.md)
- [Runtime ABI](docs/runtime_abi.md)
- [C FFI](docs/ffi.md)
- [Heterogeneous compute](docs/heterogeneous_compute.md)
- [Standard-library skeleton](docs/standard_library.md)
- [Diagnostics](docs/diagnostics.md)
- [Testing](docs/testing.md)
- [Roadmap](docs/roadmap.md)
- [Architecture documents](docs/Arch/README.md)

## License

Unless a source file states otherwise, Luna is distributed under either
the [MIT License](LICENSE-MIT) or [Apache License 2.0](LICENSE-APACHE), at your option.
