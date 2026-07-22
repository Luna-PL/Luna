# Luna Alpha 0.2.0 发布说明

## 定位

Luna Alpha 0.2.0 是用于语言试验和内部项目的 Linux/macOS/Windows 编译器原型。三个平台的稳定核心均由 CI 工作流验证；Windows 使用 MSYS2 UCRT64 工具链。稳定核心包括多文件包与显式导出、验证后的 MoonIR、基础函数/泛型/ADT、线性所有权与借用、Runtime ABI v1、显式 C FFI、JIT/AOT、`-O0/-O2/-O3`，以及 CPU 模拟器上的初始异构 ABI。

默认回归覆盖语义负例、包、导出 ABI、FFI、控制流清理、JIT/AOT 一致性、优化、运行时边界和 CPU GPU 模拟器。硬件 ROCm 冒烟测试为可选项，见 [heterogeneous_compute.md](heterogeneous_compute.md)。

## 已知限制

- 显式 `interceptor` / `context` / `slot`、`constexpr` / 反射、复杂 trait 版本组合均仍是实验性语义。
- 外部片段插件已提供 Alpha v1 ABI，但仅支持 host-only、single-shot、显式参数的 `interceptor`；`context`、`resume()`、多发射和词法捕获仍不可跨共享库边界。
- Runtime ABI v1 已固定 allocator、console、外来资源和可选可执行内存的宿主边界；Moon 容器加载、验证和 hotspot/JIT 策略仍属于后续 MoonRuntime。
- 已有安全定长 `array<T, N>`、数组字面量和带边界检查的索引；借用切片与堆拥有容器仍在实现中。GPU 数据模型当前仍以 `device_buffer<i32>`、一维 grid 与固定 256 线程 block 为主；host 批量复制使用低层 `raw<i32>` 指针。
- 大规模 ROCm 对照可与 C++23/HIP 执行同一 64 MiB、十轮变换工作负载。设置 `LUNA_GPU_PROFILE=1` 可输出 CUDA/ROCm device-event 累计 kernel 时间；它目前是 runtime profiling 开关，尚未形成稳定的源语言计时 API。
- CUDA 代码生成已实现，但尚未在本仓库的 NVIDIA 硬件 CI 上验证。
- ROCm 冒烟测试需要可见 AMD GPU、HIP 运行时、内核驱动和匹配的 `--gpu-target=rocm:gfx*`。基础 JIT/AOT 冒烟已在 RX 7800 XT 上通过；更广泛的 GPU 型号、长时间稳定性与性能数据仍待积累。
- 包仅支持同一目录的 `.luna` 文件；尚无依赖元数据、锁文件和远程包解析。
- AOT 安装后须显式传入 `--runtime-lib` / `--cc`，或设置 `LUNA_RUNTIME_LIB` / `LUNA_CXX`。

## 升级承诺

Alpha 稳定核心的错误码和安全语义将尽量保持兼容；实验性能力在 Beta 前可能调整。每次语义变化都应添加正例、负例与迁移说明。
