# CPU 性能基准

仓库提供一组可选的 Luna AOT 与 C++23 `-O3` CPU 对照基准。它们不属于默认正确性测试，运行时间包含独立进程启动。

启用并运行：

```sh
cmake -S . -B build -DLUNA_ENABLE_CPU_BENCHMARK=ON
cmake --build build -j4
LUNA_CPU_ITERATIONS=10 ctest --test-dir build -V -R luna.cpu-comparison
```

也可以直接运行：

```sh
LUNA_CPU_ITERATIONS=10 \
  bash benchmarks/run_cpu_comparison.sh ./build/luna .
```

当前覆盖五类 workload：

| workload | 检查内容 |
| --- | --- |
| `arithmetic` | 整数乘加、位运算和循环优化 |
| `branch` | 整数分支、除法和控制流 |
| `calls` | 可被 LLVM 内联的普通函数调用 |
| `array` | 定长数组动态索引；Luna 包含安全边界检查 |
| `allocation` | 线性堆值创建与作用域自动释放 |

脚本会报告平均值、中位数和 p95，并验证 Luna 与 C++ 的校验值一致。`array` 不是完全同抽象对照：C++ 使用 `std::array::operator[]`，Luna 使用带运行时边界检查的安全数组，因此该项用于量化安全语义成本。

数组索引的安全证明会识别非负字面量、`x & mask`，以及由这类初始化式产生且未被重新赋值的局部变量；证明失败时仍保留 `rt_array_index_or_abort`，不会以优化为由取消安全检查。

## 当前基线

本地 `-O3`、10 次采样结果如下；这些数字用于定位优化方向，不作为跨机器排名：

| workload | Luna 中位数 | C++23 中位数 | Luna/C++ |
| --- | ---: | ---: | ---: |
| arithmetic | 4.223 ms | 4.043 ms | 1.04x |
| branch | 25.879 ms | 25.917 ms | 1.00x |
| calls | 4.003 ms | 3.779 ms | 1.06x |
| array | 8.828 ms | 8.815 ms | 1.00x |
| allocation | 5.344 ms | 3.444 ms | 1.55x |

这组结果显示，最终 AOT 优化级别修正后，标量循环、分支、调用和安全数组都已接近 C++23。`allocation` 仍然是有意保留的真实 `rt_alloc/rt_dealloc` Runtime ABI 成本；C++23 `-O3` 可证明本例的 `new/delete` 没有可观察结果并将其消除，因此这一项不是严格等价的分配器对照，应单独解读。GPU 性能基准仍使用独立的 ROCm 对照，不与本表混合。
