#!/usr/bin/env bash
# Benchmark one Luna heterogeneous source through JIT and AOT.  The script is
# intentionally separate from CTest: timing is meaningful only on a quiet,
# known host and GPU.  It measures end-to-end JIT compilation+execution and
# repeated AOT execution; use dedicated inputs for launch, transfer, or kernel
# throughput experiments.
set -euo pipefail

driver="${LUNA_EXECUTABLE:-./build/luna}"
backend="${LUNA_GPU_BACKEND:-sim}"
gpu_target="${LUNA_GPU_TARGET:-sim}"
source="${LUNA_BENCH_SOURCE:-examples/heterogeneous.luna}"
iterations="${LUNA_BENCH_ITERATIONS:-10}"

if [[ ! -x "$driver" ]]; then
    echo "Luna driver is not executable: $driver" >&2
    exit 2
fi
if [[ ! -f "$source" ]]; then
    echo "benchmark source does not exist: $source" >&2
    exit 2
fi
if ! [[ "$iterations" =~ ^[1-9][0-9]*$ ]]; then
    echo "LUNA_BENCH_ITERATIONS must be a positive integer" >&2
    exit 2
fi

source_dir="$(dirname "$source")"
source_name="$(basename "$source" .luna)"
ir_path="${source}.ll"
exe_path="${source_dir}/${source_name}"
cleanup() { rm -f -- "$ir_path" "$exe_path"; }
trap cleanup EXIT

now_ns() { date +%s%N; }
elapsed_ms() {
    awk -v start="$1" -v end="$2" 'BEGIN { printf "%.3f", (end - start) / 1000000 }'
}

run_repeated() {
    local label="$1"
    shift
    local start end i
    start="$(now_ns)"
    for ((i = 0; i < iterations; ++i)); do
        env LUNA_GPU_BACKEND="$backend" "$@" >/dev/null
    done
    end="$(now_ns)"
    printf '%-28s total=%8sms  average=%8sms  iterations=%s\n' \
        "$label" "$(elapsed_ms "$start" "$end")" \
        "$(awk -v start="$start" -v end="$end" -v count="$iterations" 'BEGIN { printf "%.3f", (end - start) / 1000000 / count }')" \
        "$iterations"
}

echo "Luna heterogeneous benchmark"
echo "  source:     $source"
echo "  backend:    $backend"
echo "  target:     $gpu_target"
echo "  iterations: $iterations"

run_repeated "JIT compile + execute (-O2)" "$driver" run "$source" -O2 \
    "--gpu-target=$gpu_target"

build_start="$(now_ns)"
"$driver" build "$source" -O2 "--gpu-target=$gpu_target" >/dev/null
build_end="$(now_ns)"
printf '%-28s total=%8sms\n' "AOT build (-O2)" "$(elapsed_ms "$build_start" "$build_end")"

run_repeated "AOT execute (-O2)" "$exe_path"
