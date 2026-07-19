# RX 7800 XT / gfx1101 ROCm 基准记录（2026-07-19）

## 现场与方法

- GPU：AMD Radeon RX 7800 XT（`gfx1101`）。
- ROCm：本机安装的 ROCm 7.2 工具链；C++ 基线由 `hipcc -std=c++23 -O3 --offload-arch=gfx1101` 构建。
- Luna：AOT、`-O2`、ROCm backend；每次 run 均为独立进程。
- 负载：16,777,216 个 `i32`（64 MiB）；一次初始化与十次 `x = x * 3 + 1` 变换；校验值为 `29524`。
- 样本数：5；尚未预热或固定 GPU 时钟。因此本记录同时保留均值与中位数，不能把单个样本视为性能回归。
- `Luna kernel`：Luna runtime 在每个 launch 前后以 GPU event 累积的时间。
- `C++23 stream`：十一轮 dispatch 连续提交后同步；它是吞吐参考而非安全语义等价对照。
- `C++23 awaited`：每轮均创建、记录、同步、销毁 event；与 Luna 的 `launch`/`await` 生命周期对齐，是主要比较对象。

## 历史测量

| 版本 | Luna AOT wall 均值 | Luna kernel 均值 | C++ awaited wall 均值 | C++ awaited kernel 均值 |
| --- | ---: | ---: | ---: | ---: |
| 设备中端 O3 后、通用（flat）设备指针 | 62.071 ms | 2.456 ms | 54.524 ms | 1.717 ms |
| 全局地址空间 ABI 后 | 59.684 ms | 2.591 ms | 55.031 ms | 1.778 ms |

第二轮每次原始数据：

| Run | Luna wall | Luna kernel | C++ stream wall | C++ stream kernel | C++ awaited wall | C++ awaited kernel |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 54.510 ms | 3.408265 ms | 51.699 ms | 1.64068 ms | 57.798 ms | 1.73443 ms |
| 2 | 63.514 ms | 2.436139 ms | 53.833 ms | 1.60376 ms | 51.280 ms | 1.70668 ms |
| 3 | 59.772 ms | 2.385427 ms | 50.077 ms | 1.57342 ms | 54.841 ms | 2.01384 ms |
| 4 | 59.908 ms | 2.333969 ms | 58.004 ms | 1.61259 ms | 54.289 ms | 1.72043 ms |
| 5 | 60.717 ms | 2.393598 ms | 53.647 ms | 1.66187 ms | 56.946 ms | 1.71681 ms |
| 均值 | 59.684 ms | 2.591 ms | 53.452 ms | 1.618 ms | 55.031 ms | 1.778 ms |
| 中位数 | 59.908 ms | 2.394 ms | 53.647 ms | 1.613 ms | 54.841 ms | 1.720 ms |

## 已验证事实

1. 单轮 `await` 不是主要原因：C++ 的 kernel 中位数从 stream 的 1.613 ms 到 awaited 的 1.720 ms，只增加约 6.6%；Luna 相对语义等价 C++ 的稳定 kernel 差距约为 39%（2.394 / 1.720）。
2. Luna 端到端 wall 中位数为 59.908 ms，C++ awaited 为 54.841 ms；独立进程启动、HIP 初始化、模块加载和读回构成约 5 ms 的额外端到端差距。GPU kernel 差距与这一部分应分开处理。
3. 已对 Luna HSA code object 做最终 ISA 反汇编。旧 ABI 产生 `flat_load_b32` / `flat_store_b32`；全局地址空间 ABI 已产生 `global_load_b32` / `global_store_b32`，证明 ABI 修复生效。
4. 该 ABI 修复在此单一、连续访问的 RDNA3 微基准上尚未显示稳定收益：第二轮的中位数 2.394 ms 与修复前约 2.385 ms 基本相同。不能据此回退修复；它仍使 HSA 参数地址空间正确，并对更复杂访存模式更有利。

## 当前判断与后续工作

剩余瓶颈是 Luna 与 HIP/Clang 的设备代码生成质量，而不是所有权检查或每轮同步。Luna 的反汇编仍将简单的 `i32` 乘加选为 `v_mad_u64_u32`，同时其 kernel ABI 与 HIP 的完整 metadata/属性尚未逐项对齐；这是下一阶段最优先的检查方向。应先导出并逐条比较 HIP 与 Luna 的最终 ISA、SGPR/VGPR 使用量、wave occupancy 和 kernel descriptor，再做针对性 lowering，避免继续猜测。

建议下一次测量增加预热，并至少采样 20 次；同时报告最小值、中位数、p95 与均值。现有脚本可用 `LUNA_BENCH_ITERATIONS=20`，后续将补充显式预热与统计输出。

## 同日复测（二）

为排除偶然值，在相同命令、相同 5 次采样下进行了第二次完整运行：

| Run | Luna wall | Luna kernel | C++ stream wall | C++ stream kernel | C++ awaited wall | C++ awaited kernel |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 57.266 ms | 2.910634 ms | 53.470 ms | 1.61624 ms | 59.206 ms | 1.68534 ms |
| 2 | 56.062 ms | 2.355915 ms | 53.002 ms | 1.56881 ms | 52.586 ms | 1.48136 ms |
| 3 | 59.329 ms | 2.374955 ms | 54.924 ms | 1.63082 ms | 56.761 ms | 1.72938 ms |
| 4 | 53.852 ms | 2.382525 ms | 53.706 ms | 1.57252 ms | 56.079 ms | 1.65762 ms |
| 5 | 58.043 ms | 3.753236 ms | 55.492 ms | 1.58091 ms | 55.053 ms | 1.65449 ms |
| 均值 | 56.910 ms | 2.755 ms | 54.119 ms | 1.594 ms | 55.937 ms | 1.642 ms |
| 中位数 | 57.266 ms | 2.383 ms | 53.706 ms | 1.581 ms | 55.053 ms | 1.658 ms |

这轮 Luna 的 3.753236 ms 是第二个明显离群值。剔除该单点不是正式统计方法，但其余四个 Luna kernel 样本为 2.356--2.911 ms；与 C++ awaited 的中位数相比，Luna 的稳定 kernel 差距仍约 44%。因此不能把均值上升解释为 ABI 改动造成的回归，也不能把问题归因于缺少 LLVM middle-end 优化。

### 优化管线状态

Luna 并非未运行 LLVM pass。`CodeGenerator::optimizeDeviceModule` 以 LLVM `O3` 默认 module pipeline 优化每个克隆后的 AMDGPU kernel；全局地址空间 GEP lowering 后还会再运行一次同一 O3 pipeline，之后才调用 AMDGPU target machine 生成 object 并用 LLD 链接 HSACO。尚待调查的是与 HIP/Clang device compilation 的 target-specific 代码生成差异：kernel descriptor、参数属性、寄存器压力、wave occupancy 和最终指令选择，而不是是否调用了 LLVM O3。

## 性能优化阶段一（离线验证）

本阶段保留了“先 O3、再 global address-space lowering、最后收尾 O3”的顺序；实验表明把 lowering 提前会被后续 AMDGPU 中间端重新折叠成 `flat_load/flat_store`，因此该顺序不能简化。AMDGPU target machine 现在显式使用 aggressive codegen level。

同时，ROCm/CUDA runtime 会缓存 `(code object, kernel name)` 到 function handle 的映射，避免每次 launch 重复执行 `ModuleGetFunction`。这主要降低 AOT 主机端 dispatch 开销，不会改变 GPU kernel 的事件计时。

进一步对比 HIP/Clang 的 AMDGPU descriptor 后发现，手工创建的 Luna kernel 入口默认保留了 hostcall、queue、heap、completion 等未使用的隐式参数：kernarg 为 272 字节、SGPR 为 11。由于 Luna kernel 语义禁止这些能力，入口在 clone 完成后显式添加对应 `amdgpu-no-*` 属性；现在 gfx1101 descriptor 为 16 字节 kernarg、9 个 SGPR，且不含这些隐藏参数。属性必须在 `CloneFunctionInto` 之后设置，否则会被源函数属性覆盖。

当前容器无 ROCm-capable device，无法在本轮复测硬件数值；离线 `luna.rocm-isa-abi` 已通过，并确认最终 HSACO 含 `global_load/global_store`、不含 `flat_load/flat_store`。下一次硬件复测应使用：

```sh
LUNA_BENCH_ITERATIONS=20 LUNA_BENCH_OPT_LEVEL=-O3 \
  ctest --test-dir build -R luna.rocm-cpp23-comparison --output-on-failure
```

## Descriptor 修复后的硬件复测

使用 `LUNA_BENCH_ITERATIONS=20 LUNA_BENCH_OPT_LEVEL=-O3`，同一台 gfx1101 设备、同一负载完成复测：

| 指标 | Luna | C++23 stream | C++23 awaited |
| --- | ---: | ---: | ---: |
| AOT wall 平均值 | 55.765 ms | 55.389 ms | 54.569 ms |
| kernel 平均值 | 1.438 ms | 1.594 ms | 1.681 ms |

相较 descriptor 修复前的 Luna kernel 平均值 `2.421 ms`，本轮下降约 `40.6%`；相较 C++23 awaited kernel，Luna 快约 `14.5%`。AOT wall 仍比 C++23 awaited 高约 `2.2%`，主要属于进程启动、HIP 初始化、模块装载和读回边界，而不是设备 kernel 本身。

这组数据确认：额外 hidden kernarg/SGPR 确实是主要设备端性能瓶颈之一。性能阶段达到可接受基线，后续可以转入结构化 CPS；任何新的 CPS lowering 都必须保留当前 `luna.rocm-isa-abi` 和 benchmark 回归。
