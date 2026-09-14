# Windows 构建

Luna 的 Windows CI 使用 GitHub Actions 的 `windows-2022` runner 和 MSYS2
UCRT64 工具链。这个组合提供原生 Windows 可执行文件，同时保留 CMake、Ninja
和 Clang/LLVM 的一致命令行行为。

本地构建建议安装 [MSYS2](https://www.msys2.org/)，打开 **UCRT64** shell，
然后执行：

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

MinGW `RelWithDebInfo` 构建默认生成紧凑的 `luna.exe` 与相邻 `luna.exe.debug`。
executable 中的 `.gnu_debuglink` 让兼容 debugger 与 `llvm-symbolizer` 仍能找到完整 DWARF，
同时避免每次编译器启动都扫描这些调试段；安装时会同时复制两个文件。若需要保留内嵌 DWARF，
可设置 `-DLUNA_SEPARATE_COMPILER_DEBUG_INFO=OFF`。

Windows AOT 输出带 `.exe` 后缀；构建树和安装树中的编译器驱动都只使用
`luna.exe`。运行时动态加载层使用 Windows 原生 `LoadLibrary` /
`GetProcAddress`，因此不依赖 `dlopen` 或 `libdl`。CUDA 后端查找
`nvcuda.dll`；ROCm 后端查找 `amdhip64.dll`，两者都只有在显式选择对应后端
时才会加载。

Alpha 的 Windows CI 只验证 CPU、JIT/AOT、FFI、插件 ABI 和模拟器回归；GPU
硬件仍需分别安装厂商驱动和运行时，不作为默认 CI 门槛。

Luna 接受 LLVM/Clang 20 及以上版本；目前经过兼容验证的 API 基线为 LLVM 20
和 22。`clang++`、`LLVM_DIR` 与 `PATH` 中的 DLL 必须来自同一个 MSYS2 环境。
已有 **CLANG64** 环境也可以用于本机原生开发构建，只需把上述命令中的
`/ucrt64` 替换为 `/clang64`。不要把 MSYS2 LLVM 包与 MSVC 目标文件或另一个
MSYS2 前缀混用。
对于可识别的扁平 x86-64 CLANG64 executable 工具链，Luna 会绕过冗余 clang driver
子进程，并让配套 lld 的输出流经完整性摘要器；其他布局与 shared-library 链接继续使用
常规 driver 路径。

