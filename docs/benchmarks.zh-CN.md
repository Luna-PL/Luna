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

当前 CPU 样本于 2026-08-11 在 Luna commit `6838788` 上记录，环境为 Arch Linux
`7.0.14-arch1-1`、Ryzen 5 7500F 和 Clang 22.1.6。两端均使用 `-O3`；runner 先
预热两次，再交替采样十次。CPU 频率和核心位置没有固定。下表数值为包含进程启动的
wall 毫秒 `average / median / p95`：

| Workload | Luna AOT | C++23 | Median Luna/C++23 |
|---|---:|---:|---:|
| arithmetic | 4.237 / 4.189 / 4.494 | 4.297 / 4.238 / 4.652 | 0.99x |
| branch | 26.275 / 26.002 / 28.783 | 25.997 / 26.047 / 26.686 | 1.00x |
| calls | 4.235 / 4.210 / 4.399 | 4.279 / 4.224 / 4.669 | 1.00x |
| fixed array | 9.315 / 9.278 / 9.918 | 9.349 / 9.201 / 10.200 | 1.01x |
| allocation | 15.366 / 15.257 / 16.041 | 5.860 / 5.747 / 6.880 | 2.65x |
| bitmix | 35.975 / 35.879 / 38.263 | 36.086 / 36.218 / 36.852 | 0.99x |
| reduction | 9.147 / 8.982 / 10.024 | 10.686 / 10.805 / 11.109 | 0.83x |
| array scan | 13.182 / 13.096 / 13.644 | 15.038 / 15.081 / 15.665 | 0.87x |
| nested loops | 4.667 / 4.651 / 4.840 | 4.316 / 4.256 / 4.700 | 1.09x |

旧 `4.39x` allocation 行已经废弃。当前 workload 在两端都初始化分配，并将 C++
allocator adapter 放在独立翻译单元中，因此普通非 LTO 构建会真实执行两条分配路径。
由于当前 Luna unique heap value 是 ownership token、不能直接解引用，checksum 使用
共同的 initializer source 保持计算存活。剩余 Runtime ABI 与抽象差异意味着该项仍只能
观察趋势，不能作为 allocator 排名。

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
LUNA_BENCH_ITERATIONS=20 LUNA_BENCH_WARMUPS=2 \
  ctest --test-dir build -L benchmark --output-on-failure
```

Luna 应主要和生命周期相同的 C++ `awaited` 路径比较，`stream` 只作为连续提交吞吐
参考。wall time 包含进程、HIP 初始化、module 加载和同步；
`LUNA_GPU_PROFILE=1` 单独报告 device-event kernel time。

当前 GPU 样本于 2026-08-11 在 Luna commit `6838788` 上记录，设备为 RX 7800 XT /
gfx1101，使用 ROCm 7.2.4（HIP 7.2.53211）。每个实现先进行两次不计入结果的预热，
再运行 20 个测量进程；下表为算术平均值：

| 路径 | AOT wall | Device-event kernel |
|---|---:|---:|
| Luna | 56.773 ms | 1.424 ms |
| C++23/HIP stream | 55.938 ms | 1.622 ms |
| C++23/HIP awaited | 56.272 ms | 1.708 ms |

预热不可省略：一次被排除的 Luna 冷启动进程在初始化/填充 ROCm cache 时约为
330 ms。这一单设备结果只说明该 workload 的端到端 wall 接近，且 Luna 测得的 kernel
区间较低；它不构成通用 GPU 性能声明。

## 提交规则

1. 两端执行等价计算并验证相同 checksum。
2. 保持结果可观察，避免 workload 被优化器静默删除。
3. 标明安全、所有权、分配和同步语义差异。
4. 在汇总比例旁保留环境与采样数据。
5. 优先比较同一机器上的长期趋势。
