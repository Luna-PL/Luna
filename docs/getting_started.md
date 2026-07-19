# 安装与快速开始

Luna Alpha 在 Linux、macOS 和 Windows 上从源码构建。当前构建经过 LLVM 22.1 验证，要求 CMake 3.20+、C++17 编译器、Ninja（可选）和系统 C++ 链接器。CMake 不假设 Arch、Debian、Homebrew 或 MacPorts 的固定安装路径。Windows 的 MSYS2 UCRT64 安装方式见 [windows_build.md](windows_build.md)。

安装 LLVM、Clang、CMake 与 Ninja 后，使用本机 `llvm-config` 把 LLVM CMake 目录传给 CMake：

```sh
LLVM_DIR="$(llvm-config --cmakedir)"
cmake -S . -B build -G Ninja \
  -DCMAKE_C_COMPILER="$(command -v clang)" \
  -DCMAKE_CXX_COMPILER="$(command -v clang++)" \
  -DLLVM_DIR="$LLVM_DIR"
cmake --build build
ctest --test-dir build -LE hardware --output-on-failure
```

如果发行版将 LLVM 的 CMake 配置安装到别处，使用 `llvm-config --cmakedir` 的结果设置 `LLVM_DIR`。构建成功后可直接 JIT 运行或生成 AOT 可执行文件：

```sh
./build/luna run examples/operators.luna -O2
./build/luna build examples/operators.luna -O2
./examples/operators
```

安装到用户可写的暂存目录：

```sh
cmake --install build --prefix "$PWD/.local/luna"
```

如果需要安装到系统目录 `/opt/luna`，该目录通常需要管理员权限：

```sh
sudo cmake --install build --prefix /opt/luna
```

安装结果包含 `bin/luna`、`lib/libruntime.a`、许可证和文档。已安装的驱动在 AOT 构建时应显式指定运行时库和链接器，避免依赖原始构建目录：

```sh
/opt/luna/bin/luna build app.luna \
  --runtime-lib /opt/luna/lib/libruntime.a \
  --cc "$(command -v clang++)"
```

也可用 `LUNA_RUNTIME_LIB` 和 `LUNA_CXX` 设置这两个默认值。更多 AOT 细节见 [aot_build.md](aot_build.md)，错误码见 [diagnostics.md](diagnostics.md)。
