# Luna 0.1.0-alpha

> 中文简介：Luna 是由 GitHub 组织 [Luna-PL](https://github.com/Luna-PL)
> 维护的 LLVM 系统编程语言原型，仓库为
> [Luna-PL/Luna](https://github.com/Luna-PL/Luna)，命令行工具为 `luna`。

Luna is an LLVM-backed systems-language prototype with explicit package
exports, affine/linear ownership and Place-based borrowing, C FFI, structured fragments/slots,
JIT/AOT compilation, and an initial CPU/CUDA/ROCm heterogeneous-compute
surface.

This release is an experimental Linux/macOS/Windows Alpha. Linux, macOS, and
Windows have native stable-core CI workflows; Windows uses the MSYS2 UCRT64
toolchain. GPU hardware tests are optional and require the corresponding vendor
runtime and device.

## Warnings && Errors

Windows CI has been restored with explicit ORC runtime-symbol registration and
parameterized AOT process launching. The candidate fix is locally validated on
Linux and awaits confirmation from the GitHub Windows runner.

[details](docs/bug&warnings.md)

## Build

The validated toolchain is LLVM/Clang 22, CMake 3.20 or newer, a C++17
compiler, and Ninja. The LLVM CMake directory is discovered from the local
`llvm-config`, so the command does not assume an Arch, Debian, Homebrew, or
MacPorts filesystem layout:

```sh
LLVM_DIR="$(llvm-config --cmakedir)"
cmake -S . -B build -G Ninja \
  -DCMAKE_C_COMPILER="$(command -v clang)" \
  -DCMAKE_CXX_COMPILER="$(command -v clang++)" \
  -DLLVM_DIR="$LLVM_DIR"
cmake --build build
ctest --test-dir build -LE hardware --output-on-failure
```

Run an example through the JIT:

```sh
./build/luna run examples/operators.luna -O2
```

For AOT, use an explicit installed runtime when building outside the source
tree:

```sh
./build/luna build app.luna -O2 \
  --runtime-lib /opt/luna/lib/libruntime.a --cc "$(command -v clang++)"
```

The driver reports its release version with:

```sh
./build/luna --version
```

## Documentation

- [Getting started](docs/getting_started.md)
- [Alpha release notes](docs/alpha_release.md)
- [Release checklist](docs/release_checklist.md)
- [Packages and exports](docs/packages.md)
- [Types and structural/nominal identity](docs/types.md)
- [Fragments and slots](docs/fragments.md)
- [External fragment plugins](docs/dynamic_plugins.md)
- [Heterogeneous compute](docs/heterogeneous_compute.md)
- [Post-Alpha roadmap](docs/future_roadmap.md)
- [Basic benchmark](docs/benchmarks.md)
- [Windows build](docs/windows_build.md)

The full known limitations and the distinction between stable-core and
experimental features are documented in the Alpha release notes.

## License

Unless a source file states otherwise, Luna is distributed under either the
[MIT License](LICENSE-MIT) or [Apache License 2.0](LICENSE-APACHE), at your
option.
