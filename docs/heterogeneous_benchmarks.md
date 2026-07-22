# 异构计算基准方法

基准不属于正确性 CTest：运行时间受 CPU 调度、GPU 时钟、PCIe 状态、首次驱动初始化和缓存影响。仓库提供 `tools/benchmark_heterogeneous.sh` 作为可复现的采样入口。

```sh
LUNA_BENCH_ITERATIONS=20 \
LUNA_GPU_BACKEND=sim \
LUNA_GPU_TARGET=sim \
./tools/benchmark_heterogeneous.sh
```

脚本输出三项端到端时间：重复 JIT 编译并执行、一次 AOT 构建、重复 AOT 执行。默认输入是仅八线程的基础 kernel 示例，适合正确性与启动开销，负载太小而不应期待风扇或功耗有可感知变化。对 ROCm 运行前，先确认设备可见：

```sh
LUNA_GPU_BACKEND=rocm ./build/luna run examples/heterogeneous.luna -O2 \
  --gpu-target=rocm:gfx1101
LUNA_GPU_BACKEND=rocm LUNA_GPU_TARGET=rocm:gfx1101 \
  ./tools/benchmark_heterogeneous.sh
```

## Luna 与 C++23/HIP 大规模对照

仓库提供相同 GPU、相同 16,777,216 元素（64 MiB）和相同十轮逐元素变换的 C++23/HIP 基线。它验证所有运行的 checksum 均为 `29524`，并报告 Luna AOT 的端到端 wall time 与 runtime 以设备 event 累计的 kernel time，以及两种 C++23/HIP 基线：连续提交的 `stream` 和每次 launch 都同步的 `awaited`。

```sh
cmake -S . -B build \
  -DLUNA_ENABLE_ROCM_SMOKE=ON \
  -DLUNA_ROCM_SMOKE_ARCH=gfx1101 \
  -DLUNA_ENABLE_ROCM_BENCHMARK=ON
cmake --build build
LUNA_BENCH_ITERATIONS=5 \
ctest --test-dir build -L benchmark --output-on-failure
```

所有 wall time 都包含独立进程、HIP 初始化、module 加载和同步，适合 Alpha 的端到端对照。比较脚本会为 Luna 设置 `LUNA_GPU_PROFILE=1`，使 runtime 在每个 launch 前后记录设备 event，并在退出时汇总纯 kernel 时间；应用也可单独设置该环境变量，获得 `Luna GPU profile: kernel_ms=<value>` 输出。

`C++23 stream` 只在整串 dispatch 完成后同步，代表同一计算的连续提交吞吐上限。Luna 当前因线性设备缓冲区的借用规则，在每次 `await` 后完成同步与 event 回收；因此应优先将 Luna 的 kernel/wall 时间与 `C++23 awaited` 对照。后者在每个 launch 后创建、记录、同步并销毁 event，刻意匹配这一安全语义的生命周期成本。两列共同保留，可以明确区分 kernel 代码生成差距与同步策略成本。

如需检查最终 AMDGPU 指令而不是中间 LLVM IR，可在 AOT 构建时设置一个已存在的目录：`LUNA_GPU_DUMP_HSACO=/tmp/luna-hsaco ./build/luna build benchmarks/luna_gpu_vector.luna -O2 --gpu-target=rocm:gfx1101`。这会写出每个 kernel 的独立 `.hsaco`，可用 ROCm 的 `llvm-objdump -d --mcpu=gfx1101` 反汇编；该变量只用于诊断，不改变生成的可执行文件。

建议分别记录：

- 使用最小 kernel 的 launch/await 开销；
- 使用 `gpu_copy_from_host_i32` / `gpu_copy_to_host_i32` 且固定元素数的传输吞吐；
- 逐元素 kernel 的总时间和每元素时间；
- 同一后端、同一优化等级下的 JIT 首次运行与 AOT 重复运行。

报告应包含 GPU 型号、`--gpu-target`、ROCm/驱动版本、CPU、优化等级、元素数、预热次数和采样次数。基础 ROCm JIT/AOT 冒烟已在 RX 7800 XT 上通过；性能结果仍应在固定频率、预热和足够采样次数后单独记录。
