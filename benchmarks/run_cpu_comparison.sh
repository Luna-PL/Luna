#!/usr/bin/env bash
# Compare common CPU workloads through Luna AOT and C++23. This is an opt-in
# microbenchmark, not a correctness test or an application-performance claim:
# process startup is included and some language abstractions are not identical.
set -euo pipefail

luna_driver="${1:?missing Luna executable}"
source_root="${2:?missing source root}"
iterations="${LUNA_CPU_ITERATIONS:-5}"
warmups="${LUNA_CPU_WARMUPS:-1}"
optimization="${LUNA_CPU_OPT_LEVEL:--O3}"
cpp_compiler="${CXX:-clang++}"
temp_dir="$(mktemp -d /tmp/luna-cpu-bench.XXXXXX)"
cpp_binary="${temp_dir}/cpp23_cpu_suite"
source "${source_root}/benchmarks/package_source.sh"
declare -A luna_binaries

workloads=(arithmetic branch calls array allocation bitmix reduction array-scan nested)

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

"${cpp_compiler}" -std=c++23 "${optimization}" \
    "${source_root}/benchmarks/cpp23_cpu_suite.cpp" \
    "${source_root}/benchmarks/cpp23_allocation_support.cpp" \
    -o "${cpp_binary}"

for workload in "${workloads[@]}"; do
    source="$(luna_source_for "${workload}")"
    package_name="${workload//-/_}"
    luna_benchmark_build_package "${luna_driver}" "${source}" \
        "${temp_dir}/packages/${package_name}" "${package_name}" "${optimization}"
    luna_binaries["${workload}"]="${LUNA_BENCHMARK_AOT}"
done

average_median_p95() {
    local input="$1" sorted count
    sorted="${input}.sorted"
    LC_ALL=C sort -n "${input}" > "${sorted}"
    count="$(wc -l < "${sorted}")"
    awk -v count="${count}" '{ values[NR] = $1; total += $1 }
        END {
            median = (count % 2) ? values[(count + 1) / 2] :
                (values[count / 2] + values[count / 2 + 1]) / 2;
            p = int(count * 0.95); if (p < count * 0.95) p++; if (p < 1) p = 1;
            printf "%.3f %.3f %.3f", total / count, median, values[p];
        }' "${sorted}"
    rm -f -- "${sorted}"
}

elapsed_ms() {
    awk -v start="$1" -v end="$2" 'BEGIN { printf "%.3f", (end - start) / 1000000 }'
}

timestamp_ns() {
    local value
    value="$(date +%s%N)"
    if [[ "${value}" != *%N* ]]; then
        printf '%s\n' "${value}"
        return
    fi
    if ! command -v python3 >/dev/null; then
        echo "this benchmark needs GNU date or python3 for nanosecond timing" >&2
        exit 2
    fi
    python3 -c 'import time; print(time.perf_counter_ns())'
}

echo "Luna CPU/C++23 performance comparison"
echo "  iterations: ${iterations}"
echo "  warmups per executable/workload: ${warmups}"
echo "  optimization: ${optimization}"
echo "  host: $(uname -srm)"
echo "  C++ compiler: $("${cpp_compiler}" --version | head -n 1)"
echo "  note: wall time includes process startup; array workloads include Luna safety semantics"
echo "  warning: idealized microbenchmarks cannot establish a real-world performance gap"

for workload in "${workloads[@]}"; do
    expected="$(${cpp_binary} "${workload}" | tail -n 1)"
    luna_binary="${luna_binaries[${workload}]}"
    luna_times="${temp_dir}/${workload}.luna"
    cpp_times="${temp_dir}/${workload}.cpp"
    : > "${luna_times}"
    : > "${cpp_times}"

    for ((run = 0; run < warmups; ++run)); do
        luna_output="$("${luna_binary}")"
        cpp_output="$(${cpp_binary} "${workload}")"
        if [[ "${luna_output}" != "${expected}" ||
              "${cpp_output}" != "${expected}" ]]; then
            echo "warmup checksum mismatch for ${workload}: expected ${expected}" >&2
            exit 1
        fi
    done

    for ((run = 1; run <= iterations; ++run)); do
        if ((run % 2 == 1)); then
            order=(luna cpp)
        else
            order=(cpp luna)
        fi
        for implementation in "${order[@]}"; do
            start="$(timestamp_ns)"
            if [[ "${implementation}" == "luna" ]]; then
                output="$("${luna_binary}")"
                timing_file="${luna_times}"
            else
                output="$(${cpp_binary} "${workload}")"
                timing_file="${cpp_times}"
            fi
            end="$(timestamp_ns)"
            if [[ "${output}" != "${expected}" ]]; then
                echo "${implementation} ${workload} checksum mismatch: expected ${expected}, output=${output}" >&2
                exit 1
            fi
            printf '%s\n' "$(elapsed_ms "${start}" "${end}")" >> "${timing_file}"
        done
    done

    read -r luna_avg luna_median luna_p95 <<< "$(average_median_p95 "${luna_times}")"
    read -r cpp_avg cpp_median cpp_p95 <<< "$(average_median_p95 "${cpp_times}")"
    speedup="$(awk -v luna="${luna_median}" -v cpp="${cpp_median}" \
        'BEGIN { printf "%.2fx", luna / cpp }')"
    printf '  %-10s Luna avg/med/p95=%s/%s/%sms, C++23=%s/%s/%sms, median ratio=%s\n' \
        "${workload}" "${luna_avg}" "${luna_median}" "${luna_p95}" \
        "${cpp_avg}" "${cpp_median}" "${cpp_p95}" "${speedup}"
done
