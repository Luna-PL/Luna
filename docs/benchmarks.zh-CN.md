# Luna 性能基准

[English](benchmarks.md) | [简体中文](benchmarks.zh-CN.md)

> 文档类别：测量指南
> 状态：非规范

基准默认关闭，只用于观察同一环境中的长期趋势，不是正确性测试或跨机器排名。报告
必须记录 Luna commit、CPU/GPU、OS、编译器/驱动、优化级别、预热、采样次数和
频率/核心隔离状态。编译、JIT 编译加执行、AOT 执行和 device-event 时间具有不同
边界，不能混成一个性能结论。

## 轻量分阶段基准

```sh
cmake -S . -B build -DLUNA_ENABLE_BASIC_BENCHMARK=ON
cmake --build build --parallel
LUNA_BASIC_BENCH_ITERATIONS=10 \
  ctest --test-dir build -V -R luna.basic-benchmark
```

它分别报告 Luna JIT compile+run、AOT build、AOT run 和 C++23 run，使用确定性的
整数 workload。

## CPU 对照

```sh
cmake -S . -B build -DLUNA_ENABLE_CPU_BENCHMARK=ON
cmake --build build --parallel
LUNA_CPU_ITERATIONS=10 LUNA_CPU_WARMUPS=2 \
  ctest --test-dir build -V -R luna.cpu-comparison
```

套件覆盖算术、分支、函数调用、定长数组、分配、位混合、归约、数组扫描和嵌套循环，
交替运行顺序，报告 mean/median/p95，并验证 checksum 一致。这些都是小型标量/L1
工作负载，不能代表真实应用、内存带宽、并发、I/O 或真实分配架构。

2026-07-23 的 Ryzen 5 7500F / Clang 22 基线中，八项非分配 workload 有七项位于
C++23 的 `0.97x–1.09x`，归约为 `1.27x`。分配项为 `4.39x`，因为 Luna 保留真实
`rt_alloc/rt_dealloc`，Clang 则删除了 C++ `new/delete`；该结果现已废弃。当前分配
workload 在两端都初始化分配，并将 C++ allocator adapter 放在独立翻译单元中，
因此普通非 LTO 构建会真实执行两端分配路径。由于 Alpha 的 unique heap value 是
ownership token，不能直接解引用，checksum 使用两端共同的 initializer source 保持
计算存活。使用该项进行比较前应先记录新基线。

## 异构与 ROCm 对照

模拟器或已配置 backend 的 JIT/AOT 分阶段采样：

```sh
LUNA_BENCH_ITERATIONS=20 \
LUNA_GPU_BACKEND=sim \
LUNA_GPU_TARGET=sim \
  ./tools/benchmark_heterogeneous.sh
```

可选 ROCm 对照让 Luna 与 C++23/HIP 处理相同的 64 MiB 输入和十轮变换：

```sh
cmake -S . -B build \
  -DLUNA_ENABLE_ROCM_SMOKE=ON \
  -DLUNA_ROCM_SMOKE_ARCH=gfx1101 \
  -DLUNA_ENABLE_ROCM_BENCHMARK=ON
cmake --build build
LUNA_BENCH_ITERATIONS=20 \
  ctest --test-dir build -L benchmark --output-on-failure
```

Luna 应主要和生命周期相同的 C++ `awaited` 路径比较，`stream` 只作为连续提交吞吐
参考。wall time 包含进程、HIP 初始化、module 加载和同步；
`LUNA_GPU_PROFILE=1` 单独报告 device-event kernel time。

在已记录的 RX 7800 XT/gfx1101 descriptor 修复后样本中，Luna kernel 平均
`1.438 ms`，C++23 awaited 为 `1.681 ms`；AOT wall 分别为 `55.765 ms` 和
`54.569 ms`。这一单设备结果验证了 hidden kernarg/SGPR 修复，不构成通用性能声明。

## 提交规则

1. 两端执行等价计算并验证相同 checksum。
2. 保持结果可观察，避免 workload 被优化器静默删除。
3. 标明安全、所有权、分配和同步语义差异。
4. 在汇总比例旁保留环境与采样数据。
5. 优先比较同一机器上的长期趋势。
