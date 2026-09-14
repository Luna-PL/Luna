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

The benchmark runners stage each Luna source in a temporary 0.3 application
package before AOT compilation. They do not depend on the removed standalone
`luna build file.luna` path and do not leave package artifacts beside the
benchmark sources. The default `luna.benchmark-package-smoke` release test
builds and executes the arithmetic workload through this package path even
when the opt-in measurement suites are disabled.

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
builds one executable per workload on both sides, alternates execution order,
verifies identical checksums, and reports mean/median/p95. Isolating the C++
workload avoids dispatcher and code-layout effects from a monolithic suite.
Optionally pin the measurement core and priority:

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
| `rotate` | bit-rotate + `u32` popcount intrinsic, 20M iterations |

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

The pattern in this historical `f1a5302` sample is unambiguous: every workload
that touches an array index sits between 2.1x and 13.4x slower, while pure
scalar workloads stay at parity (0.83x-1.24x). This was a regression against
the 2026-08-11 sample where `array` was 1.01x and `array-scan` 0.87x: that CFG
revision lowered every array index into a `rt_array_index_or_abort` runtime
call that was neither inlined nor eliminated at `-O3`. Current code removes
statically proven checks and lowers every remaining check to an inline fast
path; only its cold failure edge calls the Runtime diagnostic, allowing LLVM
to use dominating loop conditions without weakening bounds safety.

Host O2/O3 now also constructs a generic host `TargetMachine` before building
LLVM's pass pipeline. This supplies the target transform/cost model to loop
unrolling and vectorization without specializing AOT artifacts for the build
machine. In an isolated Windows/Clang 20 check on 2026-09-11, `find` IR compiled
without clang's second middle-end pass fell from 136.57 ms to 105.41 ms after
the change; the ordinary full AOT path was 104.24 ms. The same audit found that
integer literals were fixed to `i32` and unsigned LLVM operations were never
selected. Contextual integer literals plus unsigned divide/remainder, shift and
comparison lowering now expose rotate as `llvm.fshl`/x86 `rol`. The remaining
manual 32-step popcount dominated the test, so both benchmark sides now use
their typed popcount intrinsic. On that Windows host, Luna workload-only time
changed from 99.66 ms (1.24x C++) to 26.20 ms (0.97x C++); process cycles are
at parity (69.00M vs 68.78M). The process probe now reports Windows process
cycles in addition to CPU time, RSS and page faults.

A follow-up audit on 2026-09-13 found that `ONLY_WORKLOAD` was passed by the
attribution tool but ignored by the C++ suite, while the full runner timed each
sample by spawning two MSYS `date` processes. Both paths now build one C++
executable per workload and use the in-process paired probe. With the corrected
isolation, `chase` measured 1.01x in process cycles over 25 runs and `find`
measured 1.00x; their hot blocks match C++ throughput. The remaining wall-time
ratios on very short workloads are dominated by the roughly 3 ms AOT runtime
startup delta, not their generated loops.

The same audit found that AOT links inherited implementation-only DWARF from
the compiler build's `libruntime.a`. Per-symbol runtime sections, dead-section
elimination and debug-section stripping reduced a representative Windows AOT
executable from 498,688 bytes to 64,000 bytes. Across 31 paired `find` runs its
median wall time changed from 75.274 ms to 73.215 ms, with identical output.
Luna does not currently emit source-level debug metadata, so this only removes
compiler-build leakage rather than user-program debugging information.
After integrating the change, a 7-run pass over all 20 isolated workloads put
median wall ratios in the 0.76x-1.06x range and process-cycle ratios in the
0.72x-1.06x range; `find`, `divmod`, and `rotate` were all 1.00x by wall time,
while `chase` was 1.03x.

Compiler-throughput profiling on the same host found a second redundant LLVM
middle-end: Luna had already optimized target-aware IR, then asked clang to
optimize it again. Keeping clang's O2/O3 backend level while disabling that
second pass reduced 15-run paired full `find` builds from 238.079 ms to
231.205 ms median (p95 252.914 to 240.533 ms), with byte-identical emitted IR
and unchanged executable throughput. Luna now goes further by emitting the
native object directly and retaining textual IR only as an inspectable
artifact, so clang performs the final platform link without an IR frontend.

Unchanged native builds now compare newly generated IR by content and reuse
the executable only when the IR, complete link command, compiler, runtime
archive, and path-based link dependencies are all current. This reduced the
same `find` build to 111.822 ms. Phase attribution then exposed an immediately
repeated whole-module verification after sealing: the sealer already verifies
every new CFG, and the pipeline still performs its final post-optimizer
verification. Removing only the duplicate middle pass reduced the incremental
median to 105.907 ms and process cycles from 182.76M to 168.28M. Overall,
literal-leaf fast paths in CFG construction and verification then reduced that
median to 83.546 ms and cycles to 120.79M. Memoizing immutable builtin MoonIR
type references reduced the 4,096-element array's lowering phase from about
13 ms to 0.6 ms; lazily registering NVPTX/AMDGPU targets only for explicit
device output gave a paired 72.851 to 72.388 ms change while reducing cycles
from 96.05M to 94.02M and peak working set by about 896 KiB.

Native application builds now also keep a conservative pre-frontend input
fingerprint. It covers package/workspace Luna sources, manifests and lockfiles,
code-generation options, the Luna compiler, runtime, AOT compiler, and
path-resolved link inputs. The cache stores SHA-256 digests for emitted IR,
the native object, and the executable; missing or ambiguous inputs fall back to the exact IR
comparison path. On the final 15-run guarded-cache pass, unchanged `find` builds had a
42.493 ms median, 45.634 ms p95, 55.56M median cycles, and 22.65 MiB median peak
working set. This is 82.2% shorter, or 5.61x the build throughput, versus the
238.079 ms baseline. Compared with the final full frontend/MoonIR/LLVM cache
path at 70.758 ms, the preflight hit removes another 40.0%.

The direct-object cold path was measured separately with seven fresh package
directories per workload. Small O3 builds fell from 219.87 to 184.16 ms
(-16.2%), the 4,096-element `find` build from 257.64 to 241.39 ms (-6.3%), and
the 98 KiB two-array `stream-copy` build from 260.05 to 238.94 ms (-8.1%). The
small-to-large delta is now primarily Luna/LLVM work; the remaining common
roughly 180 ms floor is dominated by process startup and the final CRT/runtime
archive link.

A follow-up dependency audit found that every `main` installed the full
application host profile even when it only returned a constant or printed a
value. The profile's dynamic initialization pulled the filesystem registry and
libc++ into otherwise small artifacts. Host-profile injection now runs after
optimization and is restricted to input/filesystem/direct-host/GPU users; the
profile implementation is archive-separated and its filesystem registry is
lazy. A constant-return executable shrank from 47,104 to 20,480 bytes
(-56.5%); the 4,096-element output-producing `find` executable shrank from
64,000 to 39,424 bytes (-38.4%), with unchanged output.

Host-profile pruning alone did not materially shorten the noisy cold path: an
immediate 15-package production pass measured 252.156 ms median. Instrumented
phase isolation then found the hidden cost. LLVM IR itself took 0.862 ms to
write but 8-23 ms to copy into place; native object emission took about 4.1 ms
but its copy-based commit took 20-40 ms; final cache digest/state publication
took another 21-43 ms. On Windows, the cold path wrote each new output to a
temporary file, copied it to a previously absent destination, then deleted the
temporary file, multiplying filesystem and scanner work.

New outputs now commit with a same-directory rename; overwrite-by-copy remains
for an existing changed destination. In a 10-sample instrumented pass, IR
commit fell to 2.224 ms median and object commit to 4.976 ms, reducing the AOT
layer to 119.090 ms. The final uninstrumented 15-package `find` pass measured
200.716 ms median and 211.710 ms p95, 20.4% below the immediate 252.156 ms
baseline. Preflight cache hits bypass this path and remain at the roughly
43-46 ms process floor. The largest remaining cold segment is the external
clang/lld CRT link at about 88-90 ms. A 20-pair alternating link test also
rejected `-nostdlib++` as an optimization (91.616 vs 89.914 ms, same output
size), so that extra policy was not retained.

The MinGW/Clang path was then isolated into its two processes. Across 24 paired
links of the same object and Runtime archive, the clang++ driver measured
88.540 ms median (102.640 ms p95), while invoking its companion `ld.lld`
with the driver-expanded CRT recipe measured 55.970 ms (64.590 ms p95). Luna
now selects that direct subprocess only for a recognized x86-64 MinGW/Clang
layout, an executable, path-resolved user libraries, and an unmodified driver
environment. Shared libraries, bare `-l` inputs, custom driver environments,
and unknown layouts retain clang++ semantics. The linker, CRT, builtin and
system archives are included in cache invalidation. No Clang/LLD libraries are
embedded into Luna, avoiding a multi-megabyte linker payload and its broad
startup cost. The direct and driver-mediated representative executables were
SHA-256 identical. In 16 fresh `find` packages, complete cold O3 builds
measured 169.953 ms median and 186.307 ms p95, 15.3% below the preceding
200.716 ms median. A 30-run guarded
cache check remained within its process floor at 45.701 ms median.
Parallelizing the post-link input recheck and three artifact digests was also
tested over 20 alternating pairs: 171.490 ms serial versus 170.470 ms parallel
was below the noise floor, while the parallel path produced the worse tail
outlier. It was not retained.

The next phase trace separated cache publication: the required post-link input
recheck took 3.2-4.1 ms, IR hashing about 0.38 ms, and object hashing about
0.15 ms. Reading the newly linked 39 KiB executable for its integrity digest,
however, stalled for 13-27 ms in the Windows filesystem/scanner path. The
compatible direct-lld path now requests the PE on stdout; Luna writes those
bytes once to a sibling pending file while updating SHA-256, then publishes it
after lld succeeds. Restricted inherited handles, failed-link cleanup, atomic
fresh-output rename, and the post-link source recheck are preserved. With
`SOURCE_DATE_EPOCH` fixed, file-output and streamed-output artifacts were
SHA-256 identical. Across 24 warmed alternating full-build pairs, streaming
reduced the cold `find` median from 174.910 to 162.930 ms (-6.8%) and p95 from
188.740 to 172.460 ms (-8.6%). A final 30-run hot-cache check measured
46.326 ms median, remaining in the same process-startup range.

That hot result was then compared with a no-work `luna --version`: 42.100 ms
of the 46.426 ms build was process/image startup, not cache validation. The
RelWithDebInfo executable contained 3.6 MiB of code but about 113 MiB of DWARF.
On MinGW RelWithDebInfo builds, Luna now keeps that DWARF in a sibling
`luna.exe.debug`, adds a `.gnu_debuglink`, and strips only the compiler image;
Release, Debug, and non-MinGW builds are unchanged. `llvm-symbolizer` still
resolved `main` to `src/main.cpp:3`. In 40 warmed alternating pairs the image
changed from 119.0 to 5.27 MB, `--version` fell from 42.830 to 36.030 ms
(-15.9%), and guarded hot builds fell from 47.690 to 40.910 ms (-14.2%). The
same streamed linker across 24 paired cold builds fell from 161.340 to
153.110 ms median (-5.1%), with p95 falling from 169.990 to 163.430 ms
(-3.9%). The behavior can be disabled with
`LUNA_SEPARATE_COMPILER_DEBUG_INFO=OFF`.
Delay-loading LLVM itself was rejected: lld cannot delay-load the imported
data symbol `llvm::sys::DynamicLibrary::Invalid`.

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
   On Windows the dynamic probe also records `QueryProcessCycleTime` cycles.

For the historical `f1a5302` sample above, the analyzer attributes the `array` gap to "12 runtime
guard calls in the hot path (`rt_array_index_or_abort`), Luna IR 63 vs 29
instructions, 12 asm calls vs 2", and the scalar workloads to "no gap". On
machines without perf, the report says so and the remaining signals still
apply; installing `linux-tools` adds hardware counters automatically.

## Whole-toolchain performance plan

Performance work is an explicit post-semantics project phase, not an informal
cleanup item. The order is:

1. Freeze reproducible compile-time, JIT, AOT, startup, peak-memory and runtime
   baselines by Luna commit and toolchain. Add budgets only after noise and
   empty-process costs are recorded.
2. Treat loops and arrays as the first runtime priority: fixed-array access,
   scans, reductions, nested loops, range/iterator lowering, bounds-check cost,
   alias information and vectorization all receive isolated workloads.
3. Attribute each gap at MoonIR, LLVM IR, optimization-remark, assembly and
   hardware-counter levels. Redundant checks may be removed only when a
   dominating proof preserves the same failure boundary; required checks need
   an inlineable fast path. No optimization may weaken bounds or ownership.
4. Continue with allocation, calls, recursion, generic specialization, cleanup
   code size and runtime ABI crossings after the loop/array gate is stable.
5. Track compiler throughput and interactive latency separately. REPL timing
   splits lexing, parsing, semantic/trait/ownership/indexing work, four MoonIR
   stages, LLVM codegen, JIT materialization/lookup/cleanup, entry execution and
   orchestration. Track exact-source `:type` cache hits separately from worker
   submissions; no cache or warm-worker design may weaken crash, timeout, memory,
   output or process-tree containment, or preserve user JIT state across cells.
6. Maintain Windows LLVM 20, WSL/Linux LLVM 22 and sanitizer coverage. GPU
   kernels, transfers and launch latency remain a separate hardware matrix.

Completion requires checksum-equivalent workloads, `-O0/-O2/-O3` JIT/AOT
correctness, safety-negative tests, before/after raw measurements and a stated
IR/assembly explanation. A single favorable microbenchmark is not completion.

## Contribution rules

1. Preserve equivalent computation and verify the same checksum.
2. Keep results observable so optimizers cannot remove the workload unnoticed.
3. State differences in safety, ownership, allocation or synchronization.
4. Report raw environment and sampling data beside summarized ratios.
5. Treat same-machine longitudinal trends as more useful than one-off rankings.
