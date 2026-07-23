# CPU performance benchmarks

[English](cpu_benchmarks.en.md) | [简体中文](cpu_benchmarks.md)

The repository includes an opt-in set of Luna AOT versus C++23 CPU
microbenchmarks. They provide a reproducible baseline for code generation,
regression detection and optimization work; they are not default correctness
tests.

> **Scope of the conclusion: this comparison is highly idealized and cannot
> establish the performance gap between Luna and C++23 in real applications.**
> It does not cover large working sets, cache misses, concurrency, system calls,
> I/O, realistic allocation patterns or application architecture, and it is
> not a cross-machine or cross-compiler ranking.

## Running the suite

Enable and run it through CTest:

```sh
cmake -S . -B build -DLUNA_ENABLE_CPU_BENCHMARK=ON
cmake --build build -j4
LUNA_CPU_ITERATIONS=10 LUNA_CPU_WARMUPS=2 \
  ctest --test-dir build -V -R luna.cpu-comparison
```

Or invoke the script directly:

```sh
LUNA_CPU_ITERATIONS=10 LUNA_CPU_WARMUPS=2 \
  LUNA_CPU_OPT_LEVEL=-O3 \
  bash benchmarks/run_cpu_comparison.sh ./build/luna .
```

The script supports `-O0`, `-O2` and `-O3`, and uses `CXX` to select the C++
compiler. It warms up each executable/workload pair, alternates whether Luna or
C++23 runs first, reports mean, median and p95, and verifies that both
executables produce exactly the same checksum.

## Workloads

| Workload | Scale | Primary behavior |
|---|---:|---|
| `arithmetic` | 20,000,000 iterations | Integer multiply-add, masking and a loop-carried recurrence |
| `branch` | 20,000,000 iterations | Data-dependent branches, integer division and control flow |
| `calls` | 20,000,000 iterations | Ordinary calls that LLVM may inline |
| `array` | 20,000,000 accesses | Dynamic indexing into a fixed array of eight `i32` values |
| `allocation` | 500,000 allocations | Linear heap values and automatic scope cleanup |
| `bitmix` | 20,000,000 iterations | Data-dependent shifts, XOR and masking |
| `reduction` | 10,000,000 iterations | Reduction across four independent multiply-add chains |
| `array-scan` | 20,000,000 accesses | Stateful circular scan of 64 `i32` values |
| `nested` | 2,500 × 4,000 iterations | Two loop levels, induction variables and integer recurrence |

These are small scalar/L1 working sets. In particular, `array` and `array-scan`
are not memory-bandwidth tests; they primarily exercise fixed-array lowering,
dynamic indexing and safety proofs. Luna proves non-negative literals,
`x & mask`, and unmodified locals initialized from such expressions safe. When
it cannot prove an index safe, it retains `rt_array_index_or_abort`.

## Local baseline from 2026-07-23

Environment and sampling conditions:

- CPU: AMD Ryzen 5 7500F, 6 cores / 12 threads;
- OS: Arch Linux x86-64, Linux `7.0.14-arch1-1`;
- Luna: `0.2.0-alpha`, AOT `-O3`;
- C++: Clang `22.1.6`, `-std=c++23 -O3`;
- warmup: twice per executable and workload;
- samples: 10 per workload, alternating Luna/C++23 run order;
- metric: median wall time of a separate process, including process startup and
  final checksum output but excluding AOT/C++ compilation;
- the host was not isolated and CPU frequency was not pinned.

| Workload | Luna median | C++23 median | Luna/C++23 |
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

A `Luna/C++23` value below 1 means the Luna sample was shorter in this run;
a value above 1 means the C++23 sample was shorter. Seven of the eight
non-allocation workloads fall between `0.97x` and `1.09x`; the four-chain
reduction is `1.27x`. This only shows that the current toolchains behave
similarly on these short, deterministic and highly optimizable loops.
Millisecond-scale process startup, the shared LLVM optimizer family and
unpinned CPU frequency can all hide or amplify differences, so the numbers
must not be extrapolated to real applications.

† `allocation` deliberately retains Luna's real `rt_alloc/rt_dealloc` Runtime
ABI cost. Clang 22 proves that the C++ example's `new/delete` pair has no
externally observable effect and removes it. This row exposes a current
optimization boundary; it is not an abstraction-equivalent allocator
comparison and cannot rank the allocators.

## Interpreting and contributing results

Long-term trends on the same machine are more useful than a one-off ranking.
A result submission should record the CPU, OS, Luna commit, C++ compiler and
version, optimization level, warmup/sample counts, and whether frequency was
pinned or cores isolated. Workload changes must preserve these rules:

1. Luna and C++23 perform the same core computation and produce the same
   checksum.
2. Loop results remain observable at the process boundary so the whole
   workload is not accidentally removed.
3. Different safety, ownership or runtime semantics are called out beside the
   result.
4. Compilation, JIT, AOT execution and hardware-kernel timing are reported
   separately.

See the [basic staged JIT/AOT benchmark](benchmarks.md) and the separate
[heterogeneous benchmark guide](heterogeneous_benchmarks.md).
