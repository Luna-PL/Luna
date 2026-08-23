# Getting started with Luna

[English](getting_started.md) | [简体中文](getting_started.zh-CN.md)

This guide takes a new contributor from a source checkout to a checked, JIT-run
and AOT-built Luna program. Luna 0.3 development currently supports Linux, macOS and
Windows; it is a research compiler rather than a production release.

## 1. Prerequisites

The validated development configuration uses:

- LLVM and Clang 22;
- CMake 3.20 or newer;
- a C++17 host compiler;
- Ninja, recommended but not required;
- a system C++ linker for AOT executables.

Use the LLVM installation's own `llvm-config` rather than assuming a
distribution-specific CMake path. Windows users should first follow the
[MSYS2 UCRT64 guide](windows_build.md).

## 2. Build and test the compiler

From the repository root on Linux or macOS:

```sh
LLVM_DIR="$(llvm-config --cmakedir)"
cmake -S . -B build -G Ninja \
  -DCMAKE_C_COMPILER="$(command -v clang)" \
  -DCMAKE_CXX_COMPILER="$(command -v clang++)" \
  -DLLVM_DIR="$LLVM_DIR"
cmake --build build --parallel
ctest --test-dir build -LE hardware --output-on-failure
```

`-LE hardware` keeps the default test run portable. GPU hardware smoke tests
are opt-in and documented in [Testing](testing.md).

## 3. Write and run a program

Create `hello/src/main.luna`:

```luna
fn main() -> i32 {
    print("Hello, Luna!");
    return 0;
}
```

Then exercise each compiler boundary:

```sh
./build/luna check hello/src/main.luna
./build/luna run hello/src/main.luna -O2
./build/luna build hello -O2
./hello/build/native/hello
```

Add `hello/luna.package` before the artifact build:

```toml
[package]
id = "org.example.hello"
version = "1.0.0"
kind = "application"
sources = ["src"]
```

`check` stops after verified MoonIR and is therefore suitable for library
packages without a `main`. `run` uses LLVM JIT. Formal `build` requires a
manifest package, writes textual LLVM IR under `build/native`, links Luna's
Runtime ABI, and creates the package-named executable there.

For a multi-package program using most implemented language features, run the
[full showcase](../examples/full_showcase/README.md).

## 4. Standalone files and packages

A standalone `.luna` file needs no manifest for `check`, `run`, or `analyze`;
artifact-producing `build` is package-only. When the input is a directory
containing `luna.package`, the driver activates package resolution, reads the
nearest workspace and lockfile, and recursively loads declared local
dependencies.

```sh
./build/luna check examples/full_showcase/app
LUNA_GPU_BACKEND=sim ./build/luna run examples/full_showcase/app -O2
```

Package IDs, module paths, manifests and exports are described in
[Packages and modules](packages.md).

## 5. Install the development build

Install to a user-writable prefix:

```sh
cmake --install build --prefix "$PWD/.local/luna"
```

The installation contains the `luna` driver, `libruntime.a`, Runtime ABI
headers, standard-library skeleton, licenses and documentation. An installed
driver should receive explicit AOT toolchain paths:

```sh
.local/luna/bin/luna build hello -O2 \
  --runtime-lib "$PWD/.local/luna/lib/libruntime.a" \
  --cc "$(command -v clang++)"
```

`LUNA_RUNTIME_LIB` and `LUNA_CXX` provide environment defaults for the same
options. See the [compiler command reference](cli.md) for the complete boundary.

## Where to continue

- [Feature overview](features.md)
- [Compiler commands](cli.md)
- [Type-system reference](reference/type_system.md)
- [Packages and modules](packages.md)
- [Metadata and selectors](versioning.md)
- [Architecture and ownership decisions](decisions.md)
- [Heterogeneous compute](heterogeneous_compute.md)
- [Roadmap](roadmap.md)
