# Luna 0.3.0 开发版

[English](README.md) | [简体中文](README.zh-CN.md)

[![Linux CI](https://github.com/Luna-PL/Luna/actions/workflows/linux-ci.yml/badge.svg)](https://github.com/Luna-PL/Luna/actions/workflows/linux-ci.yml)
[![macOS CI](https://github.com/Luna-PL/Luna/actions/workflows/macos-ci.yml/badge.svg)](https://github.com/Luna-PL/Luna/actions/workflows/macos-ci.yml)
[![Windows CI](https://github.com/Luna-PL/Luna/actions/workflows/windows-ci.yml/badge.svg)](https://github.com/Luna-PL/Luna/actions/workflows/windows-ci.yml)

Luna 是由 GitHub 组织 [Luna-PL](https://github.com/Luna-PL) 维护的、静态为主的
实验性系统编程语言。它追求高性能且不为未使用的能力付费：普通代码通过 LLVM
进行 AOT/JIT 编译，所有权、运行时绑定和未来的进化支持都是显式选择。编译器会在
LLVM lowering 前用经过验证的 MoonIR 保存语言语义和安全边界。

项目正在实施 clean-break `0.3.0` 主线。0.3 编译器不保留 `language=`
或 edition 兼容模式；依赖 0.2.1 语义的源码应使用冻结的 0.2.1 编译器。
当前设计仍是草案，实现按完成门分阶段推进。详见
[0.3 总体设计](docs/luna_0.3_design.zh-CN.md)与
[0.2 到 0.3 迁移指南](docs/migration_0.2_to_0.3.zh-CN.md)。

## 当前开发版能力

Luna 将静态、无额外运行时开销的语言机制，与需要显式选择的运行时能力区分开来：

- 统一且经过验证的 `Luna -> MoonIR -> LLVM IR` JIT/AOT 编译路径；
- 具名类型默认名义、匿名 record 结构化、ADT、受约束泛型和静态解析的 trait；
- 仿射/线性所有权、`move`、共享/可变借用和自动清理；
- 一等 Metadata、编译期 selector，以及显式 runtime 可见性；
- 反向 DNS Package ID、`::` module 路径、workspace、lockfile、别名和显式导出；
- 实验性的 `interceptor`、`context`、`slot` 和 `apply` 结构化控制流/扩展构造；
- 带版本的 Runtime ABI 和显式 C FFI 边界；
- 仅为可达 kernel 付费，并提供 CPU 模拟器及 CUDA/ROCm 代码生成路径。

0.3 迁移已经完成 Sema 拆分、具名类型默认名义化、usage block、通用
Resource/Drop contract，以及由库拥有的 `Rc`/`Arc`。唯一 MoonIR 现在无条件把可执行函数体
seal 为 canonical table 与 CFG；当前受支持表面已通过完整 56 项门禁，
包括 move-only iterator 的逐元素清理。有限静态链接的 runtime context 与 replay-safe multi-shot 已可 canonical 执行；
持久外部插件 continuation callback 仍不在 plugin ABI v1 内。host-specific Moon
Container 已支持确定性序列化、原子验证装载和 `luna build <package> -t moon`；
`-t cffi` 已可生成 C ABI 共享库与头文件，并由真实 C 消费者门禁验证。进化 runtime 和
可信 Luna native 证明格式仍未实现。开发编译器里暂存的旧
`dynamic` 源码形式只是迁移输入，不属于 0.3 phase 模型，也不构成兼容承诺。

可以先阅读[主要特性概览](docs/features.zh-CN.md)，或直接查看[可编译运行的完整示例](examples/full_showcase/README.md)。

## CPU 微基准快照

2026-08-11 在 commit `6838788` 上，本地 Ryzen 5 7500F 验证设备使用
LLVM/Clang 22.1.6 和 `-O3` 运行 Luna AOT 与 Clang C++23。下表是在两次预热后
采样 10 次所得的端到端执行时间中位数；数值越低越好，`Luna/C++23` 是耗时比。

| 工作负载 | Luna AOT | C++23 | Luna/C++23 |
|---|---:|---:|---:|
| 整数递推 | 4.189 ms | 4.238 ms | 0.99x |
| 高分支循环 | 26.002 ms | 26.047 ms | 1.00x |
| 可内联调用 | 4.210 ms | 4.224 ms | 1.00x |
| 安全定长数组 | 9.278 ms | 9.201 ms | 1.01x |
| 位混合 | 35.879 ms | 36.218 ms | 0.99x |
| 四链归约 | 8.982 ms | 10.805 ms | 0.83x |
| 有状态数组扫描 | 13.096 ms | 15.081 ms | 0.87x |
| 嵌套循环 | 4.651 ms | 4.256 ms | 1.09x |
| 分配边界† | 15.257 ms | 5.747 ms | 2.65x |

> **重要：这些性能对比是高度理想化的微基准，无法说明实际项目中的性能差距。**
> 两条路径使用同一 LLVM 后端家族，计时包含进程启动，测试没有固定 CPU 频率，
> 很小的工作集也无法代表真实应用的内存、并发、I/O 或延迟行为；测试没有固定
> CPU 频率或核心位置。†当前分配 workload 会强制两条分配路径真实执行，但 Luna
> Runtime ABI 所有权与独立 C++ allocator adapter 仍是不同抽象，所以该行不能作为
> 分配器排名。

[查看 CPU/GPU 工作负载、限制与复现步骤](docs/benchmarks.zh-CN.md)。

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

`print` 当前仍是语言/运行时提供的最小输出操作。临时且类型明确的 `std::io`
package 已提供 console 读写、flush 和基本整数解析；最终 `Read`/`Write`/formatting
表面仍在设计中。接下来请阅读[中文快速入门](docs/getting_started.md)。

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
| `luna analyze <输入> --message-format=json` | 输出语义工具快照，可从 stdin 读取一个或多个源码 overlay。 |
| `luna run <输入> [-O0\|-O2\|-O3]` | 使用 JIT 编译并运行程序。 |
| `luna build <输入> [-O0\|-O2\|-O3] [-t native\|moon\|cffi]` | 生成所选的 native、Moon Container 或 CFFI 产物。 |
| `luna repl` | 启动有限 Alpha REPL（`=`、`:decl`、单行语句）。 |

驱动还提供显式链接、运行时库、MoonIR 导出、成本报告和 GPU target 选项。
运行时后端选择与设备代码生成是两个独立决策。完整参数、环境变量和示例见
[编译器命令参考](docs/cli.zh-CN.md)。
0.3 `-t native|moon|cffi` 产物 selector 已成为 driver 选项。Native library 会在
证明段实现前明确拒绝，不会降级为无证明产物。
新增或移动实现文件前，请先查阅[仓库文件与职责指南](docs/file_guide.md)。

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

近期工作遵循 0.3 完成门，而不是冻结的 0.2 Alpha roadmap：

1. 收敛 formal package build 与平台输出约定，完成 Native application/library 产物边界；
2. 实现可信 Luna native 证明边界和最小 Moon staging/activation runtime，且不向
   普通调用热路径增加成本；
3. 在剩余语法/顺序决策冻结后，收敛 Slot/Fragment 和 runtime query 表面；
4. 让 formatter、LSP、package、benchmark 和 release gate 只面向最终 0.3 语义。

详细实现顺序与完成门见[0.3 总体设计](docs/luna_0.3_design.zh-CN.md#9-实现优先级)。

## 文档

下列入口均先提供简要说明，再链接到更具体的设计或参考文档：

- [快速入门](docs/getting_started.md)
- [主要特性概览](docs/features.zh-CN.md)
- [编译器命令参考](docs/cli.zh-CN.md)
- [完整语言示例](examples/full_showcase/README.md)
- [0.2 Alpha 语义参考](docs/reference/README.md)
- [完整类型系统参考](docs/reference/type_system.md)
- [文档记录规则](docs/reference/documentation_rules.md)
- [Package 与 module](docs/packages.md)
- [Metadata 与 selector](docs/versioning.md)
- [当前架构](docs/architecture.md)
- [架构决策](docs/decisions.md)
- [Luna 0.3 总体设计草案](docs/luna_0.3_design.zh-CN.md)
- [Fragment 与 slot](docs/fragments.md)
- [Runtime ABI 与 C FFI](docs/runtime_abi.md)
- [异构计算](docs/heterogeneous_compute.md)
- [标准库设计](docs/standard_library.zh-CN.md)
- [测试与回归](docs/testing.md)
- [生态发布快照](docs/ecosystem_release.zh-CN.md)
- [性能基准](docs/benchmarks.zh-CN.md)
- [0.3 总体设计与实现优先级](docs/luna_0.3_design.zh-CN.md)

## 许可证

除非源文件另有说明，Luna 可由使用者选择以
[MIT License](LICENSE-MIT) 或 [Apache License 2.0](LICENSE-APACHE) 发布和使用。
