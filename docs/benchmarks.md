# 基础性能基准 / Basic benchmark

Luna-PL 提供一个默认关闭的轻量基准，用于快速观察编译、启动和简单标量循环
的变化。它不是正确性测试，也不是跨机器排行榜。

```sh
cmake -S . -B build -DLUNA_ENABLE_BASIC_BENCHMARK=ON
cmake --build build --parallel
LUNA_BASIC_BENCH_ITERATIONS=10 \
  ctest --test-dir build -V -R luna.basic-benchmark
```

也可以直接运行：

```sh
LUNA_BASIC_BENCH_ITERATIONS=10 \
  bash benchmarks/run_basic_benchmark.sh ./build/luna .
```

## 指标含义

基准使用 20,000,000 次整数混合运算，并分别报告：

- `Luna JIT compile+run`：包含解析、语义分析、LLVM 编译、进程启动和运行；
  它主要反映交互式首次运行体验，不能与纯运行时吞吐直接比较。
- `Luna AOT build`：只反映编译和链接成本。
- `Luna AOT run`：包含 AOT 进程启动和运行时初始化，更接近部署后的 CPU 端到端成本。
- `C++23 run`：C++ 可执行文件启动和运行时间，用作同一算术结果的参考。

Luna 和 C++23 的边界并不完全相同：Luna 经过自己的运行时、所有权和诊断边界，
而 C++ 编译器可能消除不同的临时对象或调用。因此 benchmark 输出会同时打印
checksum，但性能数值只能在相同机器、相同编译器、相同优化级别和相同采样方式下
作趋势比较。首次运行、CPU 频率、系统负载和缓存都会影响结果。

更完整的 CPU workload 见 [cpu_benchmarks.md](cpu_benchmarks.md)；GPU/ROCm 基准
见 [heterogeneous_benchmarks.md](heterogeneous_benchmarks.md)。
