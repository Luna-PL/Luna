#!/usr/bin/env bash
# Extended CPU comparison: 20 workloads through Luna AOT and C++23.
# Includes the 9 original dimensions plus divmod, chase, stream-read,
# stream-write, stream-copy, saxpy, sort, hash, find, recursion and rotate.
# This is an opt-in microbenchmark, not a correctness test or an
# application-performance claim: process startup is included and some
# language abstractions are not identical.
set -euo pipefail

luna_driver="${1:?missing Luna executable}"
source_root="${2:?missing source root}"
iterations="${LUNA_CPU_ITERATIONS:-7}"
warmups="${LUNA_CPU_WARMUPS:-2}"
optimization="${LUNA_CPU_OPT_LEVEL:--O3}"
cpp_compiler="${CXX:-clang++}"
temp_dir="$(mktemp -d /tmp/luna-cpu-ext.XXXXXX)"
source "${source_root}/benchmarks/package_source.sh"
declare -A luna_binaries
declare -A cpp_binaries

workloads=(arithmetic branch calls array allocation bitmix reduction array-scan nested \
    divmod chase stream-read stream-write stream-copy saxpy sort hash find recursion rotate)

luna_source_for() {
    case "$1" in
        arithmetic) echo "${source_root}/benchmarks/luna_cpu_arithmetic.luna" ;;
        branch) echo "${source_root}/benchmarks/luna_cpu_branch.luna" ;;
        calls) echo "${source_root}/benchmarks/luna_cpu_calls.luna" ;;
        array) echo "${source_root}/benchmarks/luna_cpu_array.luna" ;;
        allocation) echo "${source_root}/benchmarks/luna_cpu_allocation.luna" ;;
        bitmix) echo "${source_root}/benchmarks/luna_cpu_bitmix.luna" ;;
        reduction) echo "${source_root}/benchmarks/luna_cpu_reduction.luna" ;;
        array-scan) echo "${source_root}/benchmarks/luna_cpu_array_scan.luna" ;;
        nested) echo "${source_root}/benchmarks/luna_cpu_nested.luna" ;;
        divmod) echo "${source_root}/benchmarks/luna_cpu_divmod.luna" ;;
        chase) echo "${source_root}/benchmarks/luna_cpu_chase.luna" ;;
        stream-read) echo "${source_root}/benchmarks/luna_cpu_stream_read.luna" ;;
        stream-write) echo "${source_root}/benchmarks/luna_cpu_stream_write.luna" ;;
        stream-copy) echo "${source_root}/benchmarks/luna_cpu_stream_copy.luna" ;;
        saxpy) echo "${source_root}/benchmarks/luna_cpu_saxpy.luna" ;;
        sort) echo "${source_root}/benchmarks/luna_cpu_sort.luna" ;;
        hash) echo "${source_root}/benchmarks/luna_cpu_hash.luna" ;;
        find) echo "${source_root}/benchmarks/luna_cpu_find.luna" ;;
        recursion) echo "${source_root}/benchmarks/luna_cpu_recursion.luna" ;;
        rotate) echo "${source_root}/benchmarks/luna_cpu_rotate.luna" ;;
        *) return 2 ;;
    esac
}

cpp_function_for() {
    case "$1" in
        arithmetic) echo arithmetic ;;
        branch) echo branchy ;;
        calls) echo calls ;;
        array) echo safe_array_reference ;;
        allocation|bitmix|reduction|nested|divmod|chase|saxpy|sort|hash|find|recursion|rotate) echo "$1" ;;
        array-scan) echo array_scan ;;
        stream-read) echo stream_read ;;
        stream-write) echo stream_write ;;
        stream-copy) echo stream_copy ;;
        *) return 2 ;;
    esac
}

cleanup() {
    rm -rf -- "${temp_dir}"
}
trap cleanup EXIT

if ! [[ "${iterations}" =~ ^[1-9][0-9]*$ ]]; then
    echo "LUNA_CPU_ITERATIONS must be a positive integer" >&2
    exit 2
fi
if ! [[ "${warmups}" =~ ^[0-9]+$ ]]; then
    echo "LUNA_CPU_WARMUPS must be a non-negative integer" >&2
    exit 2
fi
case "${optimization}" in
    -O0|-O2|-O3) ;;
    *)
        echo "LUNA_CPU_OPT_LEVEL must be -O0, -O2, or -O3" >&2
        exit 2
        ;;
esac
if ! command -v "${cpp_compiler}" >/dev/null; then
    echo "C++ compiler not found: ${cpp_compiler}" >&2
    exit 2
fi

for workload in "${workloads[@]}"; do
    source="$(luna_source_for "${workload}")"
    package_name="${workload//-/_}"
    cpp_function="$(cpp_function_for "${workload}")"
    cpp_binary="${temp_dir}/cpp_${package_name}"
    "${cpp_compiler}" -std=c++23 "${optimization}" \
        -DONLY_WORKLOAD="${cpp_function}" \
        "${source_root}/benchmarks/cpp23_cpu_suite_extended.cpp" \
        "${source_root}/benchmarks/cpp23_allocation_support.cpp" \
        -o "${cpp_binary}"
    cpp_binaries["${workload}"]="${cpp_binary}"
    luna_benchmark_build_package "${luna_driver}" "${source}" \
        "${temp_dir}/packages/${package_name}" "${package_name}" "${optimization}"
    luna_binaries["${workload}"]="${LUNA_BENCHMARK_AOT}"
done

bench_prefix=()
if [[ -n "${LUNA_BENCH_PIN:-}" ]]; then
    if command -v taskset >/dev/null; then
        bench_prefix=(taskset -c "${LUNA_BENCH_PIN}")
    else
        echo "LUNA_BENCH_PIN set but taskset is not available" >&2
        exit 2
    fi
fi
if [[ -n "${LUNA_BENCH_NICE:-}" ]]; then
    bench_prefix=(nice -n "${LUNA_BENCH_NICE}" "${bench_prefix[@]}")
fi

summary_values() {
    local tag="$1"
    python3 -c '
import json, sys
tag = sys.argv[1]
summary = next(
    json.loads(line) for line in sys.stdin
    if line.strip() and json.loads(line).get("tag") == tag
    and "samples" in json.loads(line)
)
print(f"{summary['"'"'wall_ms_mean'"'"']:.3f} "
      f"{summary['"'"'wall_ms_median'"'"']:.3f} "
      f"{summary['"'"'wall_ms_p95'"'"']:.3f} "
      f"{summary['"'"'cpu_cycles_median'"'"']}")
' "${tag}"
}

echo "Luna CPU/C++23 extended comparison (${#workloads[@]} workloads)"
echo "  iterations: ${iterations}"
echo "  warmups per executable/workload: ${warmups}"
echo "  optimization: ${optimization}"
echo "  host: $(uname -srm)"
if command -v lscpu >/dev/null; then
    echo "  cpu: $(lscpu | awk -F'[:：] +' '/Model name|型号名称/ {print $2; exit}')"
fi
echo "  C++ compiler: $("${cpp_compiler}" --version | head -n 1)"
echo "  Luna: $("${luna_driver}" --version 2>&1 | head -n 1)"
echo "  note: wall time includes process startup; CPU frequency is not pinned"
echo "  warning: idealized microbenchmarks cannot establish a real-world performance gap"

for workload in "${workloads[@]}"; do
    cpp_binary="${cpp_binaries[${workload}]}"
    expected="$(${cpp_binary} | tail -n 1)"
    luna_binary="${luna_binaries[${workload}]}"
    luna_output="$("${bench_prefix[@]}" "${luna_binary}")"
    cpp_output="$("${bench_prefix[@]}" "${cpp_binary}")"
    if [[ "${luna_output}" != "${expected}" ||
          "${cpp_output}" != "${expected}" ]]; then
        echo "checksum mismatch for ${workload}: expected ${expected}, luna=${luna_output}, cpp=${cpp_output}" >&2
        exit 1
    fi

    probe_output="$(python3 "${source_root}/tools/benchmark_probe.py" \
        --iterations "${iterations}" --warmups "${warmups}" \
        --tag luna --compare-tag cpp -- \
        "${bench_prefix[@]}" "${luna_binary}" --vs \
        "${bench_prefix[@]}" "${cpp_binary}")"
    read -r luna_avg luna_median luna_p95 luna_cycles <<< \
        "$(printf '%s\n' "${probe_output}" | summary_values luna)"
    read -r cpp_avg cpp_median cpp_p95 cpp_cycles <<< \
        "$(printf '%s\n' "${probe_output}" | summary_values cpp)"
    speedup="$(awk -v luna="${luna_median}" -v cpp="${cpp_median}" \
        'BEGIN { printf "%.2fx", luna / cpp }')"
    cycle_ratio="$(awk -v luna="${luna_cycles}" -v cpp="${cpp_cycles}" \
        'BEGIN { if (cpp == 0) printf "n/a"; else printf "%.2fx", luna / cpp }')"
    printf '  %-12s Luna avg/med/p95=%s/%s/%sms, C++23=%s/%s/%sms, median ratio=%s, cycles=%s\n' \
        "${workload}" "${luna_avg}" "${luna_median}" "${luna_p95}" \
        "${cpp_avg}" "${cpp_median}" "${cpp_p95}" "${speedup}" "${cycle_ratio}"
done
