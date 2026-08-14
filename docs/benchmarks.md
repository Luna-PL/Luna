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

## Extended CPU suite

Beyond the nine original dimensions, the extended suite adds eleven workloads
that separate scalar compute from memory behavior, latency chains and
vectorization:

```sh
LUNA_CPU_ITERATIONS=7 LUNA_CPU_WARMUPS=2 \
  ./benchmarks/run_cpu_suite_extended.sh /path/to/luna .
```

Input arrays are shared bit-for-bit between both sides through
`tools/gen_cpu_bench_sources.py` (seeded RNG, never hand-edited). The runner
alternates execution order, verifies identical checksums, and reports
mean/median/p95. Optionally pin the measurement core and priority:

```sh
LUNA_BENCH_PIN=2 LUNA_BENCH_NICE=-5 \
  ./benchmarks/run_cpu_suite_extended.sh /path/to/luna .
```

New dimensions and what they isolate:

| Workload | Isolation target |
|---|---|
| `divmod` | integer division/modulo latency (no memory) |
| `chase` | 64-entry permutation chase: load-latency chain |
| `stream-read` / `stream-write` / `stream-copy` | sequential 16 KiB access (vectorizable) |
| `saxpy` | in-place `v*3+1` over 16 KiB (vectorizable) |
| `sort` | 64-element insertion sort, 100k rounds (branch + data motion) |
| `hash` | 256-slot open addressing, 128 keys, 100k probes |
| `find` | 16 KiB linear scan, 200k searches |
| `recursion` | `fib(32)`, call-stack behavior |
| `rotate` | manual bit-rotate + manual popcount, 20M iterations |

Sample recorded on 2026-08-14, Luna commit `f1a5302`, same host and toolchain
as above (Clang 22.1.6, `-O3`, 7 measured runs after 2 warmups; median wall ms
including process startup):

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

The pattern is unambiguous: every workload that touches an array index sits
between 2.1x and 13.4x slower, while pure scalar workloads stay at parity
(0.83x-1.24x). This is a regression against the 2026-08-11 sample where
`array` was 1.01x and `array-scan` 0.87x; the CFG-refactor phase currently
lowers every array index into a `rt_array_index_or_abort` runtime call that is
neither inlined nor eliminated at `-O3` (see the attribution tool below).

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

## Heterogeneous scale sweep

The single 64 MiB ROCm workload is dominated by startup, not compute. The
scale sweep covers 8 MiB .. 1 GiB with compute-intensity variants (1x/4x/16x
ALU per element per pass), plus transfer-roundtrip and launch-overhead
microbenchmarks. Luna sources are generated per size by
`tools/gen_heterogeneous_scale.py`; the C++23/HIP counterpart takes the same
parameters from argv, so both sides always execute identical element counts,
pass counts, op counts and transfer sequences, and every run cross-verifies
checksums.

```sh
LUNA_HETERO_ITERATIONS=5 LUNA_HETERO_WARMUPS=2 \
LUNA_HETERO_OUT=/tmp/hetero.tsv \
  ./benchmarks/run_heterogeneous_scale.sh /path/to/luna .
```

`LUNA_GPU_BACKEND=sim` runs the Luna-only simulator sweep (capped at
`LUNA_HETERO_SIM_MAX_MIB`, default 64; the simulator executes one host thread
per element, so 1 GiB is not meaningful there). ROCm runs also record
device-event kernel time via `LUNA_GPU_PROFILE=1`.

Sample recorded 2026-08-14 on RX 7800 XT / gfx1101, ROCm 7.2.53211, Luna
commit `f1a5302`, 5 measured runs after 2 warmups (arithmetic means):

10-pass vector sweep, device-event kernel ms (lower is better):

| Size | ops | Luna kernel | C++ stream | C++ awaited |
|---|---:|---:|---:|---:|
| 8 MiB | 1 | 0.291 | 0.550 | 0.630 |
| 64 MiB | 1 | 1.395 | 1.908 | 1.744 |
| 1024 MiB | 1 | 41.185 | 40.953 | 41.450 |
| 8 MiB | 16 | 0.311 | 0.788 | 0.880 |
| 64 MiB | 16 | 1.465 | 4.062 | 4.113 |
| 1024 MiB | 16 | 40.669 | 64.010 | 65.764 |

Memory-bound (ops=1) kernels are at parity; compute-bound (ops=16) Luna
kernels are 1.4-2.5x faster. The cause is structural: the generated Luna
source unrolls the per-element operations as straight-line code (compile-time
known count), while the C++ kernel takes `ops` as a runtime parameter and
cannot unroll. Wall time at small sizes is startup-dominated for both sides
(~55 ms), which confirms why the old single-size test could not resolve
kernel differences.

Transfer roundtrip (H2D + D2H, no compute) wall time is equivalent at every
size (e.g. 1 GiB: Luna 168.6 ms, C++ 176.9 ms). Launch overhead for 1000
sequential launch/await pairs on an 8-thread kernel is also equivalent
(Luna wall 81.3 ms / kernel 8.79 ms; C++ wall 78.5 ms / kernel 9.04 ms).

The simulator sweep shows the JIT compilation cost: at 8 MiB, JIT wall is
41.1 ms vs AOT 12.9 ms; at 64 MiB, 112.9 ms vs 82.9 ms.

## Gap attribution tooling

`tools/benchmark_analyze.sh` combines static and dynamic signals for one
workload and prints likely causes, so a ratio is never reported without an
explanation attempt:

```sh
LUNA_ANALYZE_ITERATIONS=5 \
  ./tools/benchmark_analyze.sh saxpy benchmarks/luna_cpu_saxpy.luna \
  benchmarks/cpp23_cpu_suite_extended.cpp /path/to/luna . -O3 [--mca]
```

Signal families, all from the same LLVM 22.1.6 toolchain:

1. **Static IR/asm**: both sides are compiled with the same LLVM tools
   (`clang++ -O3 -DONLY_WORKLOAD=<name>` for a single-workload C++ build,
   `luna build` + `opt -O3` for Luna). Reports instruction counts, call
   sites, memory/vector ops, and counts `rt_*` runtime-guard calls in the
   Luna IR — the strongest signal.
2. **Vectorization remarks**: clang `-Rpass=loop-vectorize` vs `opt
   -pass-remarks=loop-vectorize` on the Luna IR, so "not vectorized because
   of a call" shows up explicitly.
3. **Dynamic resources**: `tools/benchmark_probe.py` (no dependencies) samples
   wall/user/sys time, max RSS, page faults and context switches via
   `getrusage`; with `--perf` it additionally runs one `perf stat` pass with
   instruction/branch/cache counters when perf is installed.
4. **Startup decomposition**: an empty-program baseline (Luna AOT and C++)
   is subtracted from both sides, separating startup from workload time.
5. **`--mca`**: extracts the largest assembly block from each side and runs
   `llvm-mca` on it (CPU auto-detected, e.g. znver4) for cycle/IPC estimates.

For the sample above, the analyzer attributes the `array` gap to "12 runtime
guard calls in the hot path (`rt_array_index_or_abort`), Luna IR 63 vs 29
instructions, 12 asm calls vs 2", and the scalar workloads to "no gap". On
machines without perf, the report says so and the remaining signals still
apply; installing `linux-tools` adds hardware counters automatically.

## Contribution rules

1. Preserve equivalent computation and verify the same checksum.
2. Keep results observable so optimizers cannot remove the workload unnoticed.
3. State differences in safety, ownership, allocation or synchronization.
4. Report raw environment and sampling data beside summarized ratios.
5. Treat same-machine longitudinal trends as more useful than one-off rankings.
