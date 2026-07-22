#!/usr/bin/env bash
# Compare common CPU workloads through Luna AOT and C++23 -O3. This is an
# opt-in benchmark, not a correctness test: process startup is included and
# the safe-array workload intentionally exposes bounds-checking overhead.
set -euo pipefail

luna_driver="${1:?missing Luna executable}"
source_root="${2:?missing source root}"
iterations="${LUNA_CPU_ITERATIONS:-5}"
optimization="${LUNA_CPU_OPT_LEVEL:--O3}"
cpp_compiler="${CXX:-clang++}"
temp_dir="$(mktemp -d /tmp/luna-cpu-bench.XXXXXX)"
cpp_binary="${temp_dir}/cpp23_cpu_suite"

workloads=(arithmetic branch calls array allocation)
declare -A luna_sources=(
    [arithmetic]="${source_root}/benchmarks/luna_cpu_arithmetic.luna"
    [branch]="${source_root}/benchmarks/luna_cpu_branch.luna"
    [calls]="${source_root}/benchmarks/luna_cpu_calls.luna"
    [array]="${source_root}/benchmarks/luna_cpu_array.luna"
    [allocation]="${source_root}/benchmarks/luna_cpu_allocation.luna"
)
declare -A luna_binaries

cleanup() {
    for source in "${luna_sources[@]}"; do
        rm -f -- "${source}.ll" "${source%.luna}"
    done
    rm -rf -- "${temp_dir}"
}
trap cleanup EXIT

if ! [[ "${iterations}" =~ ^[1-9][0-9]*$ ]]; then
    echo "LUNA_CPU_ITERATIONS must be a positive integer" >&2
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
    "${source_root}/benchmarks/cpp23_cpu_suite.cpp" -o "${cpp_binary}"

for workload in "${workloads[@]}"; do
    source="${luna_sources[${workload}]}"
    "${luna_driver}" build "${source}" "${optimization}" >/dev/null
    luna_binaries[${workload}]="${source%.luna}"
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

echo "Luna CPU/C++23 performance comparison"
echo "  iterations: ${iterations}"
echo "  optimization: ${optimization}"
echo "  note: wall time includes process startup; array includes Luna bounds checks"

for workload in "${workloads[@]}"; do
    expected="$(${cpp_binary} "${workload}" | tail -n 1)"
    luna_times="${temp_dir}/${workload}.luna"
    cpp_times="${temp_dir}/${workload}.cpp"
    : > "${luna_times}"
    : > "${cpp_times}"

    for ((run = 1; run <= iterations; ++run)); do
        start="$(date +%s%N)"
        luna_output="$("${luna_binaries[${workload}]}")"
        end="$(date +%s%N)"
        if [[ "${luna_output}" != *"${expected}"* ]]; then
            echo "Luna ${workload} checksum mismatch: expected ${expected}, output=${luna_output}" >&2
            exit 1
        fi
        printf '%s\n' "$(elapsed_ms "${start}" "${end}")" >> "${luna_times}"

        start="$(date +%s%N)"
        cpp_output="$(${cpp_binary} "${workload}")"
        end="$(date +%s%N)"
        if [[ "${cpp_output}" != *"${expected}"* ]]; then
            echo "C++23 ${workload} checksum mismatch: expected ${expected}, output=${cpp_output}" >&2
            exit 1
        fi
        printf '%s\n' "$(elapsed_ms "${start}" "${end}")" >> "${cpp_times}"
    done

    read -r luna_avg luna_median luna_p95 <<< "$(average_median_p95 "${luna_times}")"
    read -r cpp_avg cpp_median cpp_p95 <<< "$(average_median_p95 "${cpp_times}")"
    speedup="$(awk -v luna="${luna_median}" -v cpp="${cpp_median}" \
        'BEGIN { printf "%.2fx", luna / cpp }')"
    printf '  %-10s Luna avg/med/p95=%s/%s/%sms, C++23=%s/%s/%sms, median ratio=%s\n' \
        "${workload}" "${luna_avg}" "${luna_median}" "${luna_p95}" \
        "${cpp_avg}" "${cpp_median}" "${cpp_p95}" "${speedup}"
done
