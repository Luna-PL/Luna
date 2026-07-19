#!/usr/bin/env bash
# Lightweight, opt-in baseline. It intentionally reports different phases
# separately: JIT includes compilation and startup, AOT build is compilation
# only, and AOT execution is the closest host-runtime comparison.
set -euo pipefail

luna_driver="${1:-./build/luna}"
source_root="${2:-.}"
iterations="${LUNA_BASIC_BENCH_ITERATIONS:-5}"
optimization="${LUNA_BASIC_BENCH_OPT_LEVEL:--O3}"
cpp_compiler="${CXX:-clang++}"
source="${source_root}/benchmarks/luna_cpu_arithmetic.luna"
temp_dir="$(mktemp -d /tmp/luna-basic-bench.XXXXXX)"
cpp_binary="${temp_dir}/cpp23_arithmetic"
ir_path="${source}.ll"
aot_binary="${source%.luna}"

cleanup() {
    rm -f -- "${ir_path}" "${aot_binary}"
    rm -rf -- "${temp_dir}"
}
trap cleanup EXIT

if [[ ! -x "${luna_driver}" || ! -f "${source}" ]]; then
    echo "usage: $0 [path/to/luna] [source-root]" >&2
    exit 2
fi
if ! [[ "${iterations}" =~ ^[1-9][0-9]*$ ]]; then
    echo "LUNA_BASIC_BENCH_ITERATIONS must be a positive integer" >&2
    exit 2
fi
case "${optimization}" in
    -O0|-O2|-O3) ;;
    *) echo "LUNA_BASIC_BENCH_OPT_LEVEL must be -O0, -O2, or -O3" >&2; exit 2 ;;
esac
if ! command -v "${cpp_compiler}" >/dev/null; then
    echo "C++ compiler not found: ${cpp_compiler}" >&2
    exit 2
fi

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

elapsed_ms() {
    awk -v start="$1" -v end="$2" 'BEGIN { printf "%.3f", (end - start) / 1000000 }'
}

measure_repeated() {
    local label="$1"
    shift
    local start end run
    start="$(timestamp_ns)"
    for ((run = 0; run < iterations; ++run)); do
        "$@" >/dev/null
    done
    end="$(timestamp_ns)"
    printf '  %-24s total=%8sms average=%8sms iterations=%s\n' \
        "${label}" "$(elapsed_ms "${start}" "${end}")" \
        "$(awk -v start="${start}" -v end="${end}" -v count="${iterations}" \
            'BEGIN { printf "%.3f", (end - start) / 1000000 / count }')" "${iterations}"
}

"${cpp_compiler}" -std=c++23 "${optimization}" \
    "${source_root}/benchmarks/cpp23_cpu_suite.cpp" -o "${cpp_binary}"
expected="$(${cpp_binary} arithmetic | tail -n 1)"
luna_output="$(${luna_driver} run "${source}" "${optimization}")"
if [[ "${luna_output}" != *"${expected}"* ]]; then
    echo "checksum mismatch: C++23=${expected}, Luna output=${luna_output}" >&2
    exit 1
fi

echo "Luna basic CPU benchmark"
echo "  workload: arithmetic (20,000,000 iterations)"
echo "  optimization: ${optimization}"
echo "  checksum: ${expected}"
echo "  note: JIT includes compilation, startup, and execution; AOT execution excludes build time"
echo "  note: C++23 and Luna do not necessarily have identical safety/runtime abstractions"

measure_repeated "Luna JIT compile+run" "${luna_driver}" run "${source}" "${optimization}"

build_start="$(timestamp_ns)"
"${luna_driver}" build "${source}" "${optimization}" >/dev/null
build_end="$(timestamp_ns)"
printf '  %-24s total=%8sms\n' "Luna AOT build" "$(elapsed_ms "${build_start}" "${build_end}")"

measure_repeated "Luna AOT run" "${aot_binary}"
measure_repeated "C++23 run" "${cpp_binary}" arithmetic
