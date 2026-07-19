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

Windows AOT 输出带 `.exe` 后缀；构建树中的兼容名称为 `llvm-demo.exe`，正式
命令仍然是 `luna.exe`。运行时动态加载层使用 Windows 原生 `LoadLibrary` /
`GetProcAddress`，因此不依赖 `dlopen` 或 `libdl`。CUDA 后端查找
`nvcuda.dll`；ROCm 后端查找 `amdhip64.dll`，两者都只有在显式选择对应后端
时才会加载。

Alpha 的 Windows CI 只验证 CPU、JIT/AOT、FFI、插件 ABI 和模拟器回归；GPU
硬件仍需分别安装厂商驱动和运行时，不作为默认 CI 门槛。
