# Luna Alpha 0.2.0 发布说明

## 定位

Luna Alpha 0.2.0 是用于语言试验和内部项目的 Linux/macOS/Windows 编译器原型。三个平台的稳定核心均由 CI 工作流验证；Windows 使用 MSYS2 UCRT64 工具链。稳定核心包括多文件包与显式导出、验证后的 MoonIR、基础函数/泛型/ADT、线性所有权与借用、Runtime ABI v1、显式 C FFI、JIT/AOT、`-O0/-O2/-O3`，以及 CPU 模拟器上的初始异构 ABI。

默认回归覆盖语义负例、包、导出 ABI、FFI、控制流清理、JIT/AOT 一致性、优化、运行时边界和 CPU GPU 模拟器。硬件 ROCm 冒烟测试为可选项，见 [heterogeneous_compute.md](heterogeneous_compute.md)。

## 已知限制

- 显式 `interceptor` / `context` / `slot`、`constexpr` / 反射、复杂 trait 版本组合均仍是实验性语义。
- 外部片段插件已提供 Alpha v1 ABI，但仅支持 host-only、single-shot、显式参数的 `interceptor`；`context`、`resume()`、多发射和词法捕获仍不可跨共享库边界。
- Runtime ABI v1 已固定 allocator、console、外来资源和可选可执行内存的宿主边界；Moon 容器加载、验证和 hotspot/JIT 策略仍属于后续 MoonRuntime。
- 已有安全定长 `array<T, N>`、只读借用 `slice<T>`、数组字面量和带边界检查的索引；可变切片与堆拥有容器仍未开放。GPU 数据模型当前仍以 `device_buffer<i32>`、一维 grid 与固定 256 线程 block 为主；host 批量复制使用低层 `raw<i32>` 指针。
- 大规模 ROCm 对照可与 C++23/HIP 执行同一 64 MiB、十轮变换工作负载。设置 `LUNA_GPU_PROFILE=1` 可输出 CUDA/ROCm device-event 累计 kernel 时间；它目前是 runtime profiling 开关，尚未形成稳定的源语言计时 API。
- CUDA 代码生成已实现，但尚未在本仓库的 NVIDIA 硬件 CI 上验证。
- ROCm 冒烟测试需要可见 AMD GPU、HIP 运行时、内核驱动和匹配的 `--gpu-target=rocm:gfx*`。基础 JIT/AOT 冒烟已在 RX 7800 XT 上通过；更广泛的 GPU 型号、长时间稳定性与性能数据仍待积累。
- Package 已支持严格的 `luna.package`、本地 `luna.workspace`、`luna.lock`、
  精确版本依赖和递归 workspace 依赖装载；远程 registry、内容摘要验证、缓存和
  网络依赖解析尚未实现。
- AOT 安装后须显式传入 `--runtime-lib` / `--cc`，或设置 `LUNA_RUNTIME_LIB` / `LUNA_CXX`。

## 升级承诺

Alpha 稳定核心的错误码和安全语义将尽量保持兼容；实验性能力在 Beta 前可能调整。每次语义变化都应添加正例、负例与迁移说明。

## 发布检查清单

- [x] 版本元数据与 `luna --version` 统一为 `0.2.0-alpha`。
- [x] Linux C++17/C++23、macOS 和 Windows UCRT64 原生 CI 覆盖稳定核心。
- [x] Linux 自有 target 使用严格警告，并执行完整 ASan/UBSan 非硬件回归。
- [x] 非硬件 CTest 覆盖语义、包、FFI、所有权、CPS、优化和外部插件 ABI。
- [x] 安装树包含驱动、runtime、公开 ABI 头文件、标准库 workspace 和文档。
- [x] `luna.install-smoke` 从隔离安装树验证 `--version`、`check`、JIT 和 AOT。
- [x] 构建产物已忽略，历史 `build-cpp23` 工作树产物已删除。
- [x] MIT / Apache-2.0 双许可证随安装结果发布。

发布机最终确认：

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DLUNA_STRICT_WARNINGS=ON
cmake --build build --parallel
ctest --test-dir build -LE hardware --output-on-failure
sudo cmake --install build --prefix /opt/luna
/opt/luna/bin/luna --version
```

正式目标前缀还应重复安装后 JIT/AOT。AOT 必须显式传递匹配的
`--runtime-lib` 与 `--cc`，或设置对应环境变量。

ROCm 发布门要求可见设备、驱动和匹配 ISA；CUDA 仍缺少本仓库 NVIDIA 硬件 CI。
无 GPU 主机不把硬件失败算作编译器核心回归。

Alpha 封版不纳入远程 registry/网络依赖、外部 context/resume、多发射插件、通用
堆拥有容器或稳定源语言 profiling API。
