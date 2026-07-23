# CPU 性能基准

[English](cpu_benchmarks.en.md) | [简体中文](cpu_benchmarks.md)

仓库提供一组可选的 Luna AOT 与 C++23 CPU 对照微基准。它们用于建立可复现的
基础代码生成基线、发现回归和定位优化方向，不属于默认正确性测试。

> **结论边界：这组对比非常理想化，无法说明 Luna 与 C++23 在实际项目中的性能
> 差距。** 它没有覆盖大型工作集、缓存未命中、并发、系统调用、I/O、真实分配
> 模式或复杂应用架构，也不是跨机器或跨编译器排名。

## 运行方式

通过 CTest 启用并运行：

```sh
cmake -S . -B build -DLUNA_ENABLE_CPU_BENCHMARK=ON
cmake --build build -j4
LUNA_CPU_ITERATIONS=10 LUNA_CPU_WARMUPS=2 \
  ctest --test-dir build -V -R luna.cpu-comparison
```

也可以直接运行：

```sh
LUNA_CPU_ITERATIONS=10 LUNA_CPU_WARMUPS=2 \
  LUNA_CPU_OPT_LEVEL=-O3 \
  bash benchmarks/run_cpu_comparison.sh ./build/luna .
```

脚本支持 `-O0`、`-O2` 和 `-O3`，使用 `CXX` 选择 C++ 编译器。每个 workload
都会先执行指定次数的预热，然后交替改变 Luna/C++23 的采样先后顺序，报告平均
值、中位数和 p95，并验证两个可执行文件的 checksum 完全一致。

## 工作负载

| workload | 规模 | 主要检查内容 |
|---|---:|---|
| `arithmetic` | 20,000,000 轮 | 整数乘加、掩码和循环递推 |
| `branch` | 20,000,000 轮 | 数据相关分支、整数除法和控制流 |
| `calls` | 20,000,000 轮 | 可被 LLVM 内联的普通函数调用 |
| `array` | 20,000,000 次访问 | 8 个 `i32` 的定长数组动态索引 |
| `allocation` | 500,000 次 | 线性堆值创建与作用域自动释放 |
| `bitmix` | 20,000,000 轮 | 数据相关移位、异或和掩码 |
| `reduction` | 10,000,000 轮 | 四条独立乘加依赖链的归约 |
| `array-scan` | 20,000,000 次访问 | 64 个 `i32` 的有状态顺序循环扫描 |
| `nested` | 2,500 × 4,000 轮 | 两层循环、归纳变量和整数递推 |

这些都是很小的标量/L1 工作集。`array` 和 `array-scan` 特别不是内存带宽测试；
它们主要观察固定数组降低、动态索引和安全证明。Luna 的数组索引安全证明会识别
非负字面量、`x & mask`，以及由这类初始化式产生且未被重新赋值的局部变量；
证明失败时仍保留 `rt_array_index_or_abort`，不会以优化为由取消安全检查。

## 2026-07-23 本地基线

环境与采样条件：

- CPU：AMD Ryzen 5 7500F，6 核 12 线程；
- OS：Arch Linux x86-64，Linux `7.0.14-arch1-1`；
- Luna：`0.2.0-alpha`，AOT `-O3`；
- C++：Clang `22.1.6`，`-std=c++23 -O3`；
- 预热：每个可执行文件、每项 2 次；
- 采样：每项 10 次，Luna/C++23 交替先运行；
- 指标：独立进程 wall time 中位数，包含进程启动和最终 checksum 输出，不包含
  AOT/C++ 编译时间；
- 设备没有隔离其他进程，也没有固定 CPU 频率。

| workload | Luna 中位数 | C++23 中位数 | Luna/C++23 |
|---|---:|---:|---:|
| arithmetic | 4.127 ms | 4.066 ms | 1.02x |
| branch | 25.665 ms | 26.248 ms | 0.98x |
| calls | 4.005 ms | 3.950 ms | 1.01x |
| array | 8.811 ms | 9.086 ms | 0.97x |
| allocation† | 15.320 ms | 3.490 ms | 4.39x |
| bitmix | 35.232 ms | 35.711 ms | 0.99x |
| reduction | 13.761 ms | 10.851 ms | 1.27x |
| array-scan | 14.192 ms | 14.577 ms | 0.97x |
| nested | 4.489 ms | 4.137 ms | 1.09x |

`Luna/C++23` 小于 1 表示本次 Luna 样本更短，大于 1 表示 C++23 样本更短。
八项非分配用例中，七项的中位数耗时比处于 `0.97x–1.09x`，四链归约为
`1.27x`。这些结果只说明当前工具链在这组短小、确定、可高度优化的循环上的
表现非常接近；毫秒级进程启动成本、共享 LLVM 优化器和未固定的 CPU 频率都可能
放大或掩盖差异，因此不能外推到真实应用。

† `allocation` 有意保留 Luna 的真实 `rt_alloc/rt_dealloc` Runtime ABI 成本；
Clang 22 能证明 C++ 样例的 `new/delete` 没有外部可观察结果并将其消除。该项
用于暴露当前优化边界，不是抽象等价的分配器对照，也不能据此宣称分配器快慢。

## 如何解读和提交新结果

同一台机器上的长期趋势比一次横向排名更有意义。提交结果时至少记录 CPU、OS、
Luna commit、C++ 编译器与版本、优化级别、预热次数、采样次数，以及 CPU 是否
固定频率或隔离核心。若修改工作负载，必须保持：

1. Luna 与 C++23 执行相同的核心计算并得到相同 checksum；
2. 循环结果在程序边界可观察，避免整项被无意删除；
3. 不同的安全、所有权或运行时语义在表旁显式标注；
4. 编译时间、JIT 时间、AOT 运行时间和硬件 kernel 时间分别报告。

轻量的 JIT/AOT 分阶段基准见[基础性能基准](benchmarks.md)，GPU/ROCm 对照见
[异构性能基准](heterogeneous_benchmarks.md)。
