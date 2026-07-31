# Luna 安装与快速开始

[English](getting_started.md) | [简体中文](getting_started.zh-CN.md)

本文从源码构建开始，带领新贡献者完成 Luna 程序的检查、JIT 运行和 AOT 构建。
Luna Alpha 支持 Linux、macOS 和 Windows，但目前仍是研究型编译器，而非生产版本。

## 1. 准备工具链

当前构建经过 LLVM/Clang 22 验证，要求 CMake 3.20+、C++17 编译器、Ninja
（可选）和系统 C++ 链接器。CMake 不假设 Arch、Debian、Homebrew 或 MacPorts
的固定安装路径。Windows 的 MSYS2 UCRT64 安装方式见
[Windows 构建指南](windows_build.md)。

## 2. 构建并测试编译器

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

`-LE hardware` 让默认测试在没有 GPU 的主机上也可运行。硬件测试的启用方式见
[测试与回归](testing.md)。

## 3. 编写并运行程序

新建 `hello.luna`：

```luna
fn main() -> i32 {
    print("Hello, Luna!");
    return 0;
}
```

依次验证 MoonIR、JIT 和 AOT：

```sh
./build/luna check hello.luna
./build/luna run hello.luna -O2
./build/luna build hello.luna -O2
./hello
```

`check` 在验证后的 MoonIR 处停止，适合没有 `main` 的库 package；`run` 使用
LLVM JIT；`build` 会生成 `hello.luna.ll` 和本机可执行文件（Windows 为
`hello.exe`）。完整参数见[编译器命令参考](cli.zh-CN.md)。

## 4. 单文件与 package

独立 `.luna` 文件不要求 manifest。输入目录中存在 `luna.package` 时，驱动会启用
package 解析，读取最近的 workspace/lockfile，并递归装载声明的本地依赖：

```sh
./build/luna check examples/full_showcase/app
LUNA_GPU_BACKEND=sim ./build/luna run examples/full_showcase/app -O2
```

Package ID、module 路径、manifest 和导出规则见
[Package 与 module](packages.md)。完整语言能力可以通过
[全方位示例](../examples/full_showcase/README.md)了解。

## 5. 安装开发构建

安装到用户可写的暂存目录：

```sh
cmake --install build --prefix "$PWD/.local/luna"
```

如果需要安装到系统目录 `/opt/luna`，该目录通常需要管理员权限：

```sh
sudo cmake --install build --prefix /opt/luna
```

安装结果包含 `bin/luna`、`lib/libruntime.a`、
`include/luna/runtime/{RuntimeABI,FragmentPluginABI}.h`、许可证和文档。
已安装的驱动在 AOT 构建时应显式指定运行时库和链接器，避免依赖原始构建目录：

```sh
/opt/luna/bin/luna build app.luna \
  --runtime-lib /opt/luna/lib/libruntime.a \
  --cc "$(command -v clang++)"
```

也可用 `LUNA_RUNTIME_LIB` 和 `LUNA_CXX` 设置这两个默认值。完整 AOT 参数见
[编译器命令参考](cli.zh-CN.md)，错误码见
[错误模型](reference/error_model.md#10-编译器诊断编号)。

## 下一步阅读

- [主要特性概览](features.zh-CN.md)
- [编译器命令参考](cli.zh-CN.md)
- [完整类型系统参考](reference/type_system.md)
- [架构与设计决策](architecture.md)
- [Metadata 与 selector](versioning.md)
- [异构计算](heterogeneous_compute.md)
- [演进路线图](roadmap.md)
