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
禁止手改。runner 对两端都按 workload 构建独立 executable、交替执行顺序、校验
checksum 一致，并报告 mean/median/p95；C++ 不再受单体分发程序的 dispatcher 与代码布局
干扰。可选用 `LUNA_BENCH_PIN`（taskset 固定核心）与 `LUNA_BENCH_NICE` 降低噪声：

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
| `rotate` | 位旋转 + `u32` popcount intrinsic × 20M 次 |

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

这个历史 `f1a5302` 样本的规律非常明确：凡是触碰数组索引的 workload 全部落在
2.1x-13.4x，而纯标量 workload 全部持平（0.83x-1.24x）。相比 2026-08-11
样本（`array` 1.01x、`array-scan` 0.87x）这是一次回退：当时的 CFG 重构把
每次数组索引下放为 `rt_array_index_or_abort` 运行时调用，且 `-O3` 下既不内联
也不消除。当前代码会先删除可静态证明的检查，其余检查生成内联快路径；只有冷失败
边才调用 Runtime 诊断，因此 LLVM 可以利用支配循环条件，同时不弱化越界安全。

当前 host O2/O3 还会在构造 LLVM pass pipeline 之前创建 generic host
`TargetMachine`，让循环展开和向量化获得目标变换信息与成本模型，同时不会把 AOT
产物特化到构建机器。2026-09-11 的 Windows/Clang 20 隔离验证中，在关闭 clang
第二轮中端优化的条件下，`find` 从 136.57 ms 降至 105.41 ms；常规完整 AOT 路径为
104.24 ms。同一轮审计还发现整数常量被固定为 `i32`，后端从不选择无符号 LLVM
操作。现在上下文整数常量和无符号除法、取模、右移、比较已完整传递，rotate 会生成
`llvm.fshl`/x86 `rol`。剩余热点是 32 步手工 popcount，因此两端基准都改用各自的
类型化 popcount intrinsic。该 Windows 主机上，Luna 的 workload-only 时间从
99.66 ms（C++ 的 1.24x）降至 26.20 ms（0.97x），进程周期持平（69.00M 对
68.78M）。进程探针现在除 CPU 时间、RSS、缺页外也报告 Windows 进程周期。

2026-09-13 的后续审计发现，归因工具虽然传入了 `ONLY_WORKLOAD`，C++ 套件却没有消费
这个宏；完整 runner 还会为每个样本额外启动两个 MSYS `date` 进程。现在两条路径都按
workload 构建独立 C++ executable，并由单个探针进程进行成对交替采样。修正隔离后，
`chase` 的25次进程周期比为 1.01x，`find` 为 1.00x，热点块吞吐也与 C++ 一致。
极短 workload 剩余的 wall 比值主要来自约 3 ms 的 AOT runtime 启动差，而不是循环代码。

同一轮审计还发现，AOT 链接会从编译器构建目录的 `libruntime.a` 继承仅供实现调试的
DWARF。运行时按符号分节、死区段消除及调试段剥离后，代表性 Windows AOT executable
从 498,688 字节降至 64,000 字节；31次 `find` 成对采样的 wall 中位数从
75.274 ms 降至 73.215 ms，输出不变。Luna 当前尚未生成源级调试元数据，因此这里只是
消除编译器构建配置泄漏，不会删除用户程序已有的调试信息。
集成后对20个隔离 workload 各测7次，wall 中位比范围为 0.76x-1.06x，进程周期比范围为
0.72x-1.06x；`find`、`divmod`、`rotate` 的 wall 均为 1.00x，`chase` 为 1.03x。

同一主机的编译吞吐剖析又发现一轮重复 LLVM 中端：Luna 已经生成经过 target-aware
优化的 IR，随后又要求 clang 对它完整优化一次。保留 clang 的 O2/O3 后端级别、仅关闭
第二轮中端后，`find` 的15轮成对全量构建 wall 中位数从 238.079 ms 降至 231.205 ms
（p95 从 252.914 降至 240.533 ms）；两端生成的 IR 逐字节相同，executable 吞吐不变。
现在 Luna 进一步直接生成 native object，文本 IR 只作为可检查产物保留，因此 clang
只执行最终平台链接，不再经过 IR frontend。

现在未变更的 native build 会按内容比较新 IR，并且只有 IR、完整链接命令、编译器、
Runtime archive 及路径型链接依赖都未过期时才复用 executable。这让同一个 `find` 构建
降至 111.822 ms。阶段归因随后定位到 sealing 后紧邻的一轮整模块重复验证：sealer 已经
验证每个新 CFG，pipeline 之后仍保留 post-optimizer 最终验证。仅移除这次中间重复遍历后，
增量中位数进一步降至 105.907 ms，进程周期从 182.76M 降至 168.28M。字面量叶节点
在 CFG 构建和验证中的快速路径又将中位数降到 83.546 ms，周期降到 120.79M。缓存
不可变 builtin MoonIR 类型引用后，4096 元素数组的 lowering 阶段从约 13 ms 降至
0.6 ms；仅在显式请求设备产物时注册 NVPTX/AMDGPU 目标，使成对中位数从 72.851 ms
降至 72.388 ms，周期从 96.05M 降至 94.02M，峰值工作集约减少 896 KiB。

原生应用构建现在还维护保守的前端前输入指纹。它覆盖 package/workspace 的 Luna
源码、manifest 和 lockfile、代码生成选项、Luna 编译器、Runtime、AOT 编译器以及
可按路径解析的链接输入；缓存同时保存 LLVM IR、native object 与可执行文件的 SHA-256。任何输入
缺失或链接目标有歧义时，都会退回精确 IR 比较路径。最终 15 次带完整保护的缓存测量中，
未变更 `find` 的中位数为 42.493 ms，p95 为 45.634 ms，中位周期为 55.56M，中位峰值
工作集为 22.65 MiB。相对 238.079 ms 基线缩短 82.2%，构建吞吐为 5.60 倍；相对仍
完整执行前端/MoonIR/LLVM 的最终 70.758 ms 路径，前置命中又减少 40.0%。

直接 object 的冷路径使用每个 workload 七个全新 package 目录单独测量。小型 O3 构建
从 219.87 ms 降至 184.16 ms（-16.2%），4096 元素 `find` 从 257.64 ms 降至
241.39 ms（-6.3%），98 KiB、双数组的 `stream-copy` 从 260.05 ms 降至 238.94 ms
（-8.1%）。小程序到大程序的差值现在主要来自 Luna/LLVM；共同的约 180 ms 下限主要
是进程启动以及最终 CRT/Runtime archive 链接。

后续依赖审计发现，即使 `main` 只返回常量或输出一个值，也会安装完整 application host
profile；该 profile 的动态初始化会把 filesystem registry 与 libc++ 拉进本来很小的产物。
现在 host profile 在优化后按需注入，只覆盖 input/filesystem/直接 host 查询/GPU 用户；
profile 实现与 Runtime archive 分离，filesystem registry 也改为惰性构造。常量返回 executable 从 47,104 字节降至
20,480 字节（-56.5%）；带输出的 4096 元素 `find` 从 64,000 字节降至 39,424 字节
（-38.4%），输出不变。

仅裁剪 host profile 没有显著缩短有噪声的冷路径：紧邻的一轮 15 package 生产测量中位数
为 252.156 ms。分段探针随后找到了隐藏成本：LLVM IR 实际写出只需 0.862 ms，但复制提交
需 8-23 ms；native object emission 约 4.1 ms，复制提交却需 20-40 ms；最终缓存摘要与
state 发布还需 21-43 ms。Windows 冷路径会先写临时文件，再复制到尚不存在的目标，最后
删除临时文件，重复触发 filesystem 与扫描器工作。

现在新输出使用同目录 rename 提交，只有已存在且内容变化的目标继续走安全覆盖复制。
10 次带探针测量中，IR commit 中位数降至 2.224 ms，object commit 降至 4.976 ms，AOT
层降至 119.090 ms。最终无探针的 15 package `find` 中位数为 200.716 ms，p95 为
211.710 ms，相对紧邻 252.156 ms 基线缩短 20.4%。前置缓存命中绕过这条路径，仍处于约
43-46 ms 的进程下限。当前最大的冷构建段是约 88-90 ms 的外部 clang/lld CRT 链接。
20 对交替链接还排除了 `-nostdlib++`：91.616 ms 对默认路径 89.914 ms，产物大小相同，
因此没有保留这项额外策略。

随后把 MinGW/Clang 链接路径拆成两个进程单独测量。同一 object 与 Runtime archive 的
24 对链接中，clang++ driver 中位数为 88.540 ms（p95 102.640 ms），使用 driver 展开的
CRT 参数直接调用其配套 `ld.lld` 则为 55.970 ms（p95 64.590 ms）。现在 Luna 只在识别到
x86-64 MinGW/Clang 布局、构建 executable、用户库均可按路径解析且 driver 环境未被修改时
选择该直接子进程；shared library、裸 `-l` 输入、自定义 driver 环境与未知布局仍保持
clang++ 语义。linker、CRT、builtin 及系统 archive 也进入缓存失效条件。实现没有把
Clang/LLD 库嵌入 Luna，避免引入数 MB 的 linker 负载及其普遍启动成本。driver 路径与
直接路径生成的代表性 executable 的 SHA-256 完全相同。16 个全新 `find` package 的完整
O3 冷构建中位数为 169.953 ms，p95 为 186.307 ms；相对上一阶段 200.716 ms 中位数再降
15.3%。30 次保护缓存复测的中位数为 45.701 ms，仍处在相同进程下限。
另以 20 对交替构建测试了并行执行链接后的输入复查与三个产物摘要：串行 171.490 ms，
并行 170.470 ms，差异低于噪声，同时并行路径出现更差的尾部离群值，因此没有保留。

下一轮阶段探针进一步拆开 cache publication：必须保留的链接后输入复查耗时 3.2-4.1 ms，
IR 摘要约 0.38 ms，object 摘要约 0.15 ms；但为了完整性摘要首次读取刚链接出的 39 KiB
executable，会在 Windows filesystem/scanner 路径停顿 13-27 ms。兼容的直接 lld 路径现在
要求 PE 写到 stdout；Luna 将字节单次写入同目录 pending 文件的同时更新 SHA-256，并只在
lld 成功后发布。受限继承 handle、失败清理、首次产物原子 rename 及链接后源码复查均保留。
固定 `SOURCE_DATE_EPOCH` 后，文件输出与流式输出的产物 SHA-256 完全相同。24 对已预热、
交替执行的完整构建中，流式路径让冷 `find` 中位数从 174.910 降至 162.930 ms（-6.8%），
p95 从 188.740 降至 172.460 ms（-8.6%）。最终 30 次热缓存复测中位数为
46.326 ms，仍处于相同的进程启动区间。

随后用无实际工作的 `luna --version` 对照该热结果：46.426 ms 构建中有 42.100 ms
来自进程/映像启动，而非缓存校验。RelWithDebInfo executable 的代码只有 3.6 MiB，DWARF
却约 113 MiB。现在 MinGW RelWithDebInfo 构建把 DWARF 保存在相邻 `luna.exe.debug`，
写入 `.gnu_debuglink`，并且只剥离编译器映像；Release、Debug 及非 MinGW 构建不变。
`llvm-symbolizer` 仍能把 `main` 定位到 `src/main.cpp:3`。40 对预热交替测试中，映像从
119.0 降至 5.27 MB，`--version` 从 42.830 降至 36.030 ms（-15.9%），保护缓存热构建
从 47.690 降至 40.910 ms（-14.2%）。使用同一流式 linker 的 24 对冷构建中，中位数
从 161.340 降至 153.110 ms（-5.1%），p95 从 169.990 降至 163.430 ms（-3.9%）。可用
`LUNA_SEPARATE_COMPILER_DEBUG_INFO=OFF` 关闭该行为。LLVM 自身的 delay-load 实验未保留：
lld 无法延迟加载导入数据符号 `llvm::sys::DynamicLibrary::Invalid`。

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
   （CPU 自动探测，如 znver4）得到周期/IPC 估计；Windows 动态探针还记录
   `QueryProcessCycleTime` 进程周期。

对上述历史 `f1a5302` 样本，分析器把 `array` 差距归因为"热路径 12 处运行时守卫调用
（`rt_array_index_or_abort`），Luna IR 63 vs 29 条，asm 调用 12 vs 2"，
标量 workload 归因为"无差距"。没有 perf 的机器会明确提示，其余信号仍然
有效；安装 `linux-tools` 后硬件计数器自动启用。

## 全工具链性能规划

性能工作是语义收口后的正式项目阶段，不作为零散清理项处理，顺序如下：

1. 按 Luna commit 与工具链冻结可复现的编译时间、JIT、AOT、启动、峰值内存和
   运行时基线；先记录噪声与空进程成本，再建立性能预算。
2. 将循环和数组列为首要运行时目标：为定长数组访问、扫描、归约、嵌套循环、
   range/iterator lowering、边界检查成本、alias 信息和向量化建立隔离 workload。
3. 对每项差距同时检查 MoonIR、LLVM IR、优化 remark、汇编和硬件计数器。只有存在
   dominating proof 且保持同一失败边界时才能消除冗余检查；不可消除的检查需要可内联
   fast path。任何优化都不得弱化边界或所有权安全。
4. 循环/数组门稳定后，再处理分配、调用、递归、泛型特化、cleanup 代码体积和
   Runtime ABI crossing。
5. 编译吞吐与交互延迟分开跟踪。REPL 计时区分 lexer、parser、
   semantic/traits/ownership/indexing、四项 MoonIR 阶段、LLVM codegen、JIT
   materialization/lookup/cleanup、入口执行和编排开销，并将源码完全一致的 `:type` 缓存命中
   与 worker 提交分开统计；任何缓存或 warm-worker 设计都必须保持崩溃、超时、内存、
   输出与进程树隔离，并且不得让用户 JIT 状态跨 cell 保留。
6. 持续覆盖 Windows LLVM 20、WSL/Linux LLVM 22 和 sanitizer；GPU kernel、传输和
   launch 延迟继续使用独立硬件矩阵。

完成门要求 workload checksum 等价、`-O0/-O2/-O3` JIT/AOT 正确性、安全负例、
前后原始测量以及明确的 IR/汇编解释；单个有利微基准不构成完成。

## 提交规则

1. 两端执行等价计算并验证相同 checksum。
2. 保持结果可观察，避免 workload 被优化器静默删除。
3. 标明安全、所有权、分配和同步语义差异。
4. 在汇总比例旁保留环境与采样数据。
5. 优先比较同一机器上的长期趋势。
