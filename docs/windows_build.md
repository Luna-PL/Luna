# Windows Build

Luna's Windows CI uses the GitHub Actions `windows-2022` runner and the MSYS2 UCRT64
toolchain. This combination provides native Windows executables while preserving consistent
command-line behavior for CMake, Ninja, and Clang/LLVM.

For a local build, install [MSYS2](https://www.msys2.org/), open the **UCRT64** shell, and
run:

```sh
pacman -Syu
pacman -S --needed \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-llvm \
  mingw-w64-ucrt-x86_64-clang

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=/ucrt64/bin/clang.exe \
  -DCMAKE_CXX_COMPILER=/ucrt64/bin/clang++.exe \
  -DLLVM_DIR=/ucrt64/lib/cmake/llvm
cmake --build build --parallel
ctest --test-dir build -LE hardware --output-on-failure
```

MinGW `RelWithDebInfo` builds default to a compact `luna.exe` plus the sibling
`luna.exe.debug`. The executable's `.gnu_debuglink` lets compatible debuggers
and `llvm-symbolizer` find the full DWARF without making every compiler startup
scan it. Both files are installed together. Set
`-DLUNA_SEPARATE_COMPILER_DEBUG_INFO=OFF` to retain embedded DWARF instead.

Windows AOT output uses the `.exe` suffix, and compiler drivers in both the build tree and
installation tree use only `luna.exe`. The runtime dynamic-loading layer uses the native
Windows `LoadLibrary`/`GetProcAddress` APIs, so it does not depend on `dlopen` or
`libdl`. The CUDA backend looks for `nvcuda.dll`; the ROCm backend looks for
`amdhip64.dll`. Neither is loaded unless its backend is selected explicitly.

Alpha Windows CI verifies only CPU, JIT/AOT, FFI, plugin ABI, and simulator regressions.
GPU hardware still requires the vendor driver and runtime to be installed separately and is
not a default CI gate.

Luna accepts LLVM/Clang 20 through 22; LLVM 20 and 22 are the currently tested API
baselines. Keep `clang++`, `LLVM_DIR`, and the DLLs on `PATH` from the same MSYS2
environment. An existing **CLANG64** installation is also suitable for local native
development builds by replacing `/ucrt64` with `/clang64` in the commands above. Do not
mix an MSYS2 LLVM package with MSVC object files or with a different MSYS2 prefix.
For a recognized flat x86-64 CLANG64 executable toolchain, Luna bypasses the
redundant clang driver subprocess and streams the companion lld output through
its integrity hasher. Other layouts and shared-library links retain the normal
driver path.
