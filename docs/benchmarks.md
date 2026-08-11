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

The current recorded CPU sample was taken on 2026-08-11 from Luna commit
`6838788`, on Arch Linux `7.0.14-arch1-1`, Ryzen 5 7500F, and Clang 22.1.6.
Both paths used `-O3`; the runner used two warmups and ten alternating measured
runs. CPU frequency and core placement were not pinned. Values below are
`average / median / p95` wall milliseconds, including process startup:

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

The old `4.39x` allocation row is retired. The current workload initializes an
allocation on both sides and keeps the C++ allocator adapter in a separate
translation unit, so ordinary non-LTO builds execute both allocation paths.
Its checksum keeps the common initializer source live because current Luna
unique heap values are ownership tokens, not directly dereferenceable values.
The remaining Runtime ABI and abstraction differences still make this a trend
detector rather than an allocator ranking.

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
LUNA_BENCH_ITERATIONS=20 LUNA_BENCH_WARMUPS=2 \
  ctest --test-dir build -L benchmark --output-on-failure
```

Compare Luna primarily with the C++ `awaited` path, which matches Luna's
launch/await lifecycle. The `stream` path is retained as a throughput reference.
Wall time includes process startup, HIP initialization, module loading and
synchronization; `LUNA_GPU_PROFILE=1` reports device-event kernel time.

The current recorded GPU sample was taken on 2026-08-11 from Luna commit
`6838788`, on RX 7800 XT/gfx1101 with ROCm 7.2.4 (HIP 7.2.53211). Each
implementation received two unmeasured warmups before 20 measured processes;
reported values are arithmetic means:

| Path | AOT wall | Device-event kernel |
|---|---:|---:|
| Luna | 56.773 ms | 1.424 ms |
| C++23/HIP stream | 55.938 ms | 1.622 ms |
| C++23/HIP awaited | 56.272 ms | 1.708 ms |

The warmups matter: an excluded cold Luna process took about 330 ms while
initializing/cache-populating the ROCm path. This single-device result shows
similar end-to-end wall time and a lower measured kernel interval for this one
workload; it is not a general GPU performance claim.

## Contribution rules

1. Preserve equivalent computation and verify the same checksum.
2. Keep results observable so optimizers cannot remove the workload unnoticed.
3. State differences in safety, ownership, allocation or synchronization.
4. Report raw environment and sampling data beside summarized ratios.
5. Treat same-machine longitudinal trends as more useful than one-off rankings.
