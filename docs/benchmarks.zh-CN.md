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

基准 runner 会先把每个 Luna 源文件放入临时 0.3 application package，再执行
AOT 编译；它们不依赖已移除的 `luna build file.luna` 路径，也不会在基准
源码旁留下 package 产物。即使可选测量套件关闭，默认 release 测试
`luna.benchmark-package-smoke` 也会通过该 package 路径构建并执行 arithmetic
workload。

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

## 扩展 CPU 套件

在原有九个维度之外，扩展套件新增十一个 workload，把标量计算与内存行为、
延迟链和向量化区分开：

```sh
LUNA_CPU_ITERATIONS=7 LUNA_CPU_WARMUPS=2 \
  ./benchmarks/run_cpu_suite_extended.sh /path/to/luna .
```

两侧输入数组由 `tools/gen_cpu_bench_sources.py`（固定种子 RNG）逐位共享，
禁止手改。runner 交替执行顺序、校验 checksum 一致，并报告 mean/median/p95。
可选用 `LUNA_BENCH_PIN`（taskset 固定核心）与 `LUNA_BENCH_NICE` 降低噪声：

```sh
LUNA_BENCH_PIN=2 LUNA_BENCH_NICE=-5 \
  ./benchmarks/run_cpu_suite_extended.sh /path/to/luna .
```

新增维度及其隔离目标：

| Workload | 隔离目标 |
|---|---|
| `divmod` | 整数除法/取模延迟（无内存） |
| `chase` | 64 项置换追逐：访存延迟链 |
| `stream-read` / `stream-write` / `stream-copy` | 16 KiB 顺序访问（可向量化） |
| `saxpy` | 16 KiB 原地 `v*3+1`（可向量化） |
| `sort` | 64 元素插入排序 × 100k 轮（分支 + 数据搬运） |
| `hash` | 256 槽开放寻址，128 键，100k 次探测 |
| `find` | 16 KiB 线性查找 × 200k 次 |
| `recursion` | `fib(32)`，调用栈行为 |
| `rotate` | 手工位旋转 + 手工 popcount × 20M 次 |

样本于 2026-08-14 在 Luna commit `f1a5302` 记录，环境与上文相同（Clang
22.1.6、`-O3`、预热 2 次后测 7 次；中位 wall 毫秒，含进程启动）：

| Workload | Luna AOT | C++23 | Median Luna/C++23 |
|---|---:|---:|---:|
| arithmetic | 4.570 | 4.781 | 0.96x |
| branch | 27.533 | 26.655 | 1.03x |
| calls | 4.762 | 4.426 | 1.08x |
| array | 51.769 | 9.365 | 5.53x |
| allocation | 16.051 | 5.840 | 2.75x |
| bitmix | 39.972 | 36.440 | 1.10x |
| reduction | 9.221 | 11.100 | 0.83x |
| array-scan | 52.388 | 15.212 | 3.44x |
| nested | 5.014 | 4.807 | 1.04x |
| divmod | 84.968 | 83.538 | 1.02x |
| chase | 56.665 | 26.984 | 2.10x |
| stream-read | 17.337 | 2.478 | 7.00x |
| stream-write | 17.742 | 3.796 | 4.67x |
| stream-copy | 28.630 | 2.444 | 11.71x |
| saxpy | 28.631 | 3.318 | 8.63x |
| sort | 415.221 | 33.650 | 12.34x |
| hash | 2.597 | 2.271 | 1.14x |
| find | 479.905 | 35.955 | 13.35x |
| recursion | 7.069 | 6.205 | 1.14x |
| rotate | 93.169 | 76.033 | 1.23x |

规律非常明确：凡是触碰数组索引的 workload 全部落在 2.1x-13.4x，而纯标量
workload 全部持平（0.83x-1.24x）。相比 2026-08-11 样本（`array` 1.01x、
`array-scan` 0.87x）这是回退：当前 CFG 重构阶段把每次数组索引下放为
`rt_array_index_or_abort` 运行时调用，且 `-O3` 下既不内联也不消除
（详见下文归因工具）。

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

## 异构规模扫描

单一的 64 MiB ROCm 测试被启动开销主导，测不出计算差异。规模扫描覆盖
8 MiB-1 GiB，并带计算强度变体（每元素每遍 1x/4x/16x ALU），外加传输往返与
启动开销微基准。Luna 源码按规模由 `tools/gen_heterogeneous_scale.py` 生成；
C++23/HIP 端从 argv 读取相同参数，两侧始终执行相同的元素数、遍数、运算数
与传输序列，每次运行都交叉校验 checksum。

```sh
LUNA_HETERO_ITERATIONS=5 LUNA_HETERO_WARMUPS=2 \
LUNA_HETERO_OUT=/tmp/hetero.tsv \
  ./benchmarks/run_heterogeneous_scale.sh /path/to/luna .
```

`LUNA_GPU_BACKEND=sim` 运行仅 Luna 的模拟器扫描（上限
`LUNA_HETERO_SIM_MAX_MIB`，默认 64；模拟器每元素一个宿主线程，1 GiB 无意义）。
ROCm 运行通过 `LUNA_GPU_PROFILE=1` 同时记录设备事件 kernel 时间。

样本于 2026-08-14 在 RX 7800 XT / gfx1101、ROCm 7.2.53211、Luna commit
`f1a5302` 记录，预热 2 次后测 5 次（算术平均）：

10 遍向量扫描，设备事件 kernel ms（越低越好）：

| 规模 | ops | Luna kernel | C++ stream | C++ awaited |
|---|---:|---:|---:|---:|
| 8 MiB | 1 | 0.291 | 0.550 | 0.630 |
| 64 MiB | 1 | 1.395 | 1.908 | 1.744 |
| 1024 MiB | 1 | 41.185 | 40.953 | 41.450 |
| 8 MiB | 16 | 0.311 | 0.788 | 0.880 |
| 64 MiB | 16 | 1.465 | 4.062 | 4.113 |
| 1024 MiB | 16 | 40.669 | 64.010 | 65.764 |

内存受限（ops=1）kernel 持平；计算受限（ops=16）Luna kernel 快 1.4-2.5x。
原因是结构性的：生成的 Luna 源码把每元素运算展开为直线代码（运算数编译期
已知），而 C++ kernel 把 `ops` 作为运行时参数，无法展开。小规模下两侧 wall
都被启动主导（约 55 ms），印证了旧单规模测试无法分辨 kernel 差异。

传输往返（H2D + D2H，无计算）wall 在各规模下等效（如 1 GiB：Luna 168.6 ms，
C++ 176.9 ms）。1000 次顺序 launch/await（8 线程）的启动开销也等效
（Luna wall 81.3 ms / kernel 8.79 ms；C++ wall 78.5 ms / kernel 9.04 ms）。

模拟器扫描显示 JIT 编译开销：8 MiB 时 JIT wall 41.1 ms 对 AOT 12.9 ms；
64 MiB 时 112.9 ms 对 82.9 ms。

## 差距归因工具

`tools/benchmark_analyze.sh` 对单个 workload 组合静态与动态信号并输出可能
原因，任何比值都附带归因尝试：

```sh
LUNA_ANALYZE_ITERATIONS=5 \
  ./tools/benchmark_analyze.sh saxpy benchmarks/luna_cpu_saxpy.luna \
  benchmarks/cpp23_cpu_suite_extended.cpp /path/to/luna . -O3 [--mca]
```

信号族（全部来自同一套 LLVM 22.1.6 工具链）：

1. **静态 IR/asm**：两侧都用相同 LLVM 工具编译（C++ 用 `-DONLY_WORKLOAD=<name>`
   单 workload 构建，Luna 用 `luna build` + `opt -O3`）。报告指令数、调用点、
   访存/向量指令数，并统计 Luna IR 中的 `rt_*` 运行时守卫调用——最强信号。
2. **向量化诊断**：clang `-Rpass=loop-vectorize` 对照 Luna IR 的
   `opt -pass-remarks=loop-vectorize`，"因调用无法向量化"会直接显示。
3. **动态资源**：`tools/benchmark_probe.py`（零依赖）通过 `getrusage` 采样
   wall/user/sys 时间、最大 RSS、缺页与上下文切换；加 `--perf` 后若安装了
   perf 还会额外跑一次 `perf stat`，取指令/分支/缓存计数器。
4. **启动分解**：空程序基线（Luna AOT 与 C++）从两侧减去，把启动与工作量
   时间分开。
5. **`--mca`**：从两侧汇编中提取最大代码块，用 `llvm-mca` 分析
   （CPU 自动探测，如 znver4）得到周期/IPC 估计。

对上述样本，分析器把 `array` 差距归因为"热路径 12 处运行时守卫调用
（`rt_array_index_or_abort`），Luna IR 63 vs 29 条，asm 调用 12 vs 2"，
标量 workload 归因为"无差距"。没有 perf 的机器会明确提示，其余信号仍然
有效；安装 `linux-tools` 后硬件计数器自动启用。

## 提交规则

1. 两端执行等价计算并验证相同 checksum。
2. 保持结果可观察，避免 workload 被优化器静默删除。
3. 标明安全、所有权、分配和同步语义差异。
4. 在汇总比例旁保留环境与采样数据。
5. 优先比较同一机器上的长期趋势。
