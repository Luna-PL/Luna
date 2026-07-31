# Luna performance benchmarks

[English](benchmarks.md) | [简体中文](benchmarks.zh-CN.md)

> Document type: measurement guide
> Status: non-normative

Benchmarks are opt-in trend detectors, not correctness tests or cross-machine
rankings. Always record the Luna commit, CPU/GPU, OS, compiler and driver
versions, optimization level, warmups, sample count and clock/isolation state.
Compilation, JIT compile-and-run, AOT execution and device-event time are
different metrics and must not be compared as if they had the same boundary.

## Lightweight staged benchmark

```sh
cmake -S . -B build -DLUNA_ENABLE_BASIC_BENCHMARK=ON
cmake --build build --parallel
LUNA_BASIC_BENCH_ITERATIONS=10 \
  ctest --test-dir build -V -R luna.basic-benchmark
```

This reports Luna JIT compile+run, Luna AOT build, Luna AOT run and C++23 run
separately for a deterministic integer workload.

## CPU comparison

```sh
cmake -S . -B build -DLUNA_ENABLE_CPU_BENCHMARK=ON
cmake --build build --parallel
LUNA_CPU_ITERATIONS=10 LUNA_CPU_WARMUPS=2 \
  ctest --test-dir build -V -R luna.cpu-comparison
```

The suite compares Luna AOT and C++23 on arithmetic, branches, calls, fixed
arrays, allocation, bit mixing, reduction, array scanning and nested loops. It
alternates run order, reports mean/median/p95 and verifies identical checksums.
These are small scalar/L1 workloads; they do not represent realistic
applications, memory bandwidth, concurrency, I/O or allocation architecture.

The 2026-07-23 Ryzen 5 7500F / Clang 22 baseline placed seven of eight
non-allocation workloads between `0.97x` and `1.09x` of C++23; reduction was
`1.27x`. Its former `4.39x` allocation result is retired: Luna retained real
`rt_alloc/rt_dealloc` calls while Clang eliminated the C++ `new/delete` pair.
The current allocation workload initializes an allocation on both sides and
keeps the C++ allocator adapter in a separate translation unit, so ordinary
non-LTO builds execute both allocation paths. Its checksum keeps the common
initializer source live because Alpha unique heap values are ownership tokens,
not directly dereferenceable values. Record a new baseline before using that
row for comparisons.

## Heterogeneous and ROCm comparison

For JIT/AOT staging on the simulator or a configured backend:

```sh
LUNA_BENCH_ITERATIONS=20 \
LUNA_GPU_BACKEND=sim \
LUNA_GPU_TARGET=sim \
  ./tools/benchmark_heterogeneous.sh
```

The optional ROCm comparison uses the same 64 MiB input and ten transforms in
Luna and C++23/HIP:

```sh
cmake -S . -B build \
  -DLUNA_ENABLE_ROCM_SMOKE=ON \
  -DLUNA_ROCM_SMOKE_ARCH=gfx1101 \
  -DLUNA_ENABLE_ROCM_BENCHMARK=ON
cmake --build build
LUNA_BENCH_ITERATIONS=20 \
  ctest --test-dir build -L benchmark --output-on-failure
```

Compare Luna primarily with the C++ `awaited` path, which matches Luna's
launch/await lifecycle. The `stream` path is retained as a throughput reference.
Wall time includes process startup, HIP initialization, module loading and
synchronization; `LUNA_GPU_PROFILE=1` reports device-event kernel time.

On the recorded RX 7800 XT/gfx1101 run after hidden kernarg/SGPR cleanup, Luna
reported a `1.438 ms` average kernel time versus `1.681 ms` for C++23 awaited,
while AOT wall time was `55.765 ms` versus `54.569 ms`. This single-device
result confirms the descriptor fix but is not a general performance claim.

## Contribution rules

1. Preserve equivalent computation and verify the same checksum.
2. Keep results observable so optimizers cannot remove the workload unnoticed.
3. State differences in safety, ownership, allocation or synchronization.
4. Report raw environment and sampling data beside summarized ratios.
5. Treat same-machine longitudinal trends as more useful than one-off rankings.
