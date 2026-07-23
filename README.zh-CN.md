# Luna 0.2.0-alpha

[English](README.md) | [简体中文](README.zh-CN.md)

[![Linux CI](https://github.com/Luna-PL/Luna/actions/workflows/linux-ci.yml/badge.svg)](https://github.com/Luna-PL/Luna/actions/workflows/linux-ci.yml)
[![macOS CI](https://github.com/Luna-PL/Luna/actions/workflows/macos-ci.yml/badge.svg)](https://github.com/Luna-PL/Luna/actions/workflows/macos-ci.yml)
[![Windows CI](https://github.com/Luna-PL/Luna/actions/workflows/windows-ci.yml/badge.svg)](https://github.com/Luna-PL/Luna/actions/workflows/windows-ci.yml)

Luna 是由 GitHub 组织 [Luna-PL](https://github.com/Luna-PL) 维护的、基于
LLVM 的实验性系统编程语言。编译器首先把 Luna 源程序降级到经过验证的
MoonIR，在保留语言语义和安全边界后，再统一对接 LLVM JIT 与 AOT。

项目目前处于面向语言研究和内部试验的 Alpha 阶段，实验性语法和 API 在 Beta
前仍可能调整。准确的支持边界和已知限制请见[Alpha 发布说明](docs/alpha_release.md)。

## 主要特性

Luna 将静态、无额外运行时开销的语言机制，与需要显式选择的运行时/动态能力区分开来：

- 统一且经过验证的 `Luna -> MoonIR -> LLVM IR` JIT/AOT 编译路径；
- 默认结构化类型、显式名义身份、ADT、泛型和静态解析的 trait；
- 仿射/线性所有权、`move`、共享/可变借用和自动清理；
- 一等 Metadata、编译期 selector，以及显式 `runtime` / `dynamic` 保留和分派；
- 反向 DNS Package ID、`::` module 路径、workspace、lockfile、别名和显式导出；
- 结构化的 `interceptor`、`context`、`slot` 和 `apply` 控制效果；
- 带版本的 Runtime ABI 和显式 C FFI 边界；
- 仅为可达 kernel 付费，并提供 CPU 模拟器及 CUDA/ROCm 代码生成路径。

可以先阅读[主要特性概览](docs/features.zh-CN.md)，或直接查看[可编译运行的完整示例](examples/full_showcase/README.md)。

## CPU 微基准快照

在本地 Ryzen 5 7500F 验证设备上，Luna AOT 与 Clang C++23 均使用
LLVM/Clang 22.1.6 和 `-O3`。下表是在两次预热后采样 10 次所得的端到端执行时间
中位数；数值越低越好，`Luna/C++23` 是耗时比。

| 工作负载 | Luna AOT | C++23 | Luna/C++23 |
|---|---:|---:|---:|
| 整数递推 | 4.127 ms | 4.066 ms | 1.02x |
| 高分支循环 | 25.665 ms | 26.248 ms | 0.98x |
| 可内联调用 | 4.005 ms | 3.950 ms | 1.01x |
| 安全定长数组 | 8.811 ms | 9.086 ms | 0.97x |
| 位混合 | 35.232 ms | 35.711 ms | 0.99x |
| 四链归约 | 13.761 ms | 10.851 ms | 1.27x |
| 有状态数组扫描 | 14.192 ms | 14.577 ms | 0.97x |
| 嵌套循环 | 4.489 ms | 4.137 ms | 1.09x |
| 分配边界† | 15.320 ms | 3.490 ms | 4.39x |

> **重要：这些性能对比是高度理想化的微基准，无法说明实际项目中的性能差距。**
> 两条路径使用同一 LLVM 后端家族，计时包含进程启动，测试没有固定 CPU 频率，
> 很小的工作集也无法代表真实应用的内存、并发、I/O 或延迟行为。†分配用例中
> Clang 消除了 C++ 的 `new/delete`，而 Luna 有意保留 Runtime ABI 分配，所以该行
> 不能作为分配器排名。

[查看详细 CPU 对比、工作负载定义、限制与复现步骤](docs/cpu_benchmarks.md)。
GPU 数据单独记录在[异构性能基准](docs/heterogeneous_benchmarks.md)中。

## Hello, World

一个最小 Luna 程序是包含 `main` 函数的 `.luna` 文件：

```luna
fn main() -> i32 {
    print("Hello, Luna!");
    return 0;
}
```

保存为 `hello.luna` 后，可以检查 MoonIR、使用 JIT 运行，或者生成本机可执行文件：

```sh
./build/luna check hello.luna
./build/luna run hello.luna -O2
./build/luna build hello.luna -O2
./hello
```

`print` 当前仍是语言/运行时提供的最小输出操作；未来的泛型格式化接口将位于
`std::io`。接下来请阅读[中文快速入门](docs/getting_started.md)。

## 从源码构建

当前经过验证的开发工具链是 LLVM/Clang 22、CMake 3.20 或更新版本、C++17
编译器，以及可选的 Ninja。Linux 和 macOS 可使用：

```sh
LLVM_DIR="$(llvm-config --cmakedir)"
cmake -S . -B build -G Ninja \
  -DCMAKE_C_COMPILER="$(command -v clang)" \
  -DCMAKE_CXX_COMPILER="$(command -v clang++)" \
  -DLLVM_DIR="$LLVM_DIR"
cmake --build build --parallel
ctest --test-dir build -LE hardware --output-on-failure
```

安装、运行测试和安装后 AOT 配置请见[完整构建与安装教程](docs/getting_started.md)。
Windows 使用 MSYS2 UCRT64 工具链，单独说明见[Windows 构建指南](docs/windows_build.md)。

## 编译器命令

`luna` 驱动既可以接收单独的 `.luna` 文件，也可以接收包含 `luna.package` 的
package 目录：

| 命令 | 用途 |
|---|---|
| `luna --version` | 输出编译器版本。 |
| `luna check <输入>` | 验证源码直到 MoonIR，不生成机器码。 |
| `luna run <输入> [-O0\|-O2\|-O3]` | 使用 JIT 编译并运行程序。 |
| `luna build <输入> [-O0\|-O2\|-O3]` | 生成 LLVM IR、链接 Runtime ABI，并输出本机可执行文件。 |
| `luna repl` | 启动实验性的 JIT REPL。 |

驱动还提供显式链接、运行时库、MoonIR 导出、成本报告和 GPU target 选项。
运行时后端选择与设备代码生成是两个独立决策。完整参数、环境变量和示例见
[编译器命令参考](docs/cli.zh-CN.md)。

## 平台与测试状态

Linux 和 macOS 是 Luna 的最优先支持目标；Windows 通过 MSYS2 UCRT64 获得
支持。稳定核心在三个平台上均有原生 CI，所有仓库改动也会在 Linux 本机验证。

当前本地测试设备为：

- OS：Arch Linux x86-64（`7.0.14-arch1-1`）
- CPU：AMD Ryzen 5 7500F（x86-64）
- GPU：AMD Radeon RX 7800 XT（`gfx1101` / Navi 32）
- RAM：32 GiB DDR5

Luna 支持原生异构计算。受现有设备条件限制，目前已经验证 CPU 模拟器和 AMD
ROCm 路径；CUDA 代码生成已经存在，但仍需要更广泛的 NVIDIA 硬件验证。详情见
[测试与回归](docs/testing.md)和[异构计算指南](docs/heterogeneous_compute.md)。

## 下一步演进

近期将先完成稳定的语言基础，再扩大新的语言表面：

1. 引入一等错误处理，并以它作为标准库接口的基础；
2. 完善 package/module 工具链，扩展 `org.luna.core` / `org.luna.std`，包括格式化
   `std::io`；
3. 继续收敛所有权、仿射/线性检查和显式成本模型；
4. 冻结 Moon 容器验证格式，并为 MoonRuntime 加载、JIT 和 hotspot 演进预留接口；
5. 泛化异构内存与 kernel target，扩大真实硬件验证；
6. 完善诊断、格式化器、语言服务器和 package 生态工具。

详细阶段、稳定性承诺和长期计划见
[Luna 演进路线图](docs/evolution_roadmap.md)与[Alpha 之后的发展路线](docs/future_roadmap.md)。

## 文档

下列入口均先提供简要说明，再链接到更具体的设计或参考文档：

- [快速入门](docs/getting_started.md)
- [主要特性概览](docs/features.zh-CN.md)
- [编译器命令参考](docs/cli.zh-CN.md)
- [完整语言示例](examples/full_showcase/README.md)
- [Package 与 module](docs/packages.md)
- [结构化/名义类型身份](docs/types.md)
- [Metadata 与 selector](docs/versioning.md)
- [所有权与仿射/线性模型](docs/Arch/Ownership_Affine_Model_RFC.md)
- [Fragment 与 slot](docs/fragments.md)
- [Runtime ABI](docs/runtime_abi.md)
- [C FFI](docs/ffi.md)
- [异构计算](docs/heterogeneous_compute.md)
- [标准库雏形](docs/standard_library.md)
- [诊断参考](docs/diagnostics.md)
- [测试与回归](docs/testing.md)
- [CPU 性能对比](docs/cpu_benchmarks.md)
- [演进路线图](docs/evolution_roadmap.md)
- [架构设计文档](docs/Arch/README.md)

## 许可证

除非源文件另有说明，Luna 可由使用者选择以
[MIT License](LICENSE-MIT) 或 [Apache License 2.0](LICENSE-APACHE) 发布和使用。
