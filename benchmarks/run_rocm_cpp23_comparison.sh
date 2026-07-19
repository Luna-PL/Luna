#!/usr/bin/env bash
# Compare Luna AOT end-to-end execution against a C++23/HIP implementation of
# the same 16 Mi-element, 10-pass vector workload. This is a hardware
# benchmark, never part of the default CPU-only CTest suite.
set -euo pipefail

luna_driver="${1:?missing Luna executable}"
source_root="${2:?missing source root}"
hipcc_bin="${HIPCC:-hipcc}"
architecture="${LUNA_AMDGPU_ARCH:-gfx1101}"
iterations="${LUNA_BENCH_ITERATIONS:-3}"
optimization="${LUNA_BENCH_OPT_LEVEL:--O2}"
source="${source_root}/benchmarks/luna_gpu_vector.luna"
cpp_source="${source_root}/benchmarks/cpp23_hip_vector.cpp"
luna_ir="${source}.ll"
luna_aot="${source_root}/benchmarks/luna_gpu_vector"
temp_dir="$(mktemp -d /tmp/luna-cpp23-rocm.XXXXXX)"
cpp_binary="${temp_dir}/cpp23_hip_vector"

cleanup() {
    rm -f -- "${luna_ir}" "${luna_aot}"
    rm -rf -- "${temp_dir}"
}
trap cleanup EXIT

if ! [[ "${iterations}" =~ ^[1-9][0-9]*$ ]]; then
    echo "LUNA_BENCH_ITERATIONS must be a positive integer" >&2
    exit 2
fi
case "${optimization}" in
    -O0|-O2|-O3) ;;
    *)
        echo "LUNA_BENCH_OPT_LEVEL must be -O0, -O2, or -O3" >&2
        exit 2
        ;;
esac
if ! command -v "${hipcc_bin}" >/dev/null; then
    echo "HIP compiler not found: ${hipcc_bin}" >&2
    exit 2
fi

"${hipcc_bin}" -std=c++23 -O3 "--offload-arch=${architecture}" \
    "${cpp_source}" -o "${cpp_binary}"
# Code-object generation is deliberately independent from device discovery:
# the build half can run on a compiler-only machine, while the produced AOT
# binary is still executed with ROCm below. This also makes ISA investigations
# possible without occupying a GPU.
env LUNA_GPU_BACKEND=sim LUNA_GPU_EMIT_AMDGPU=1 LUNA_AMDGPU_ARCH="${architecture}" \
    "${luna_driver}" build "${source}" "${optimization}" >/dev/null

elapsed_ms() {
    awk -v start="$1" -v end="$2" 'BEGIN { printf "%.3f", (end - start) / 1000000 }'
}
average_ms() {
    awk -v total="$1" -v count="$2" 'BEGIN { printf "%.3f", total / count }'
}

run_luna() {
    local start end output kernel_ms
    start="$(date +%s%N)"
    output="$(env LUNA_GPU_BACKEND=rocm LUNA_GPU_PROFILE=1 "${luna_aot}")"
    end="$(date +%s%N)"
    if [[ "${output}" != *"29524"* ]]; then
        echo "Luna result mismatch: ${output}" >&2
        exit 1
    fi
    kernel_ms="$(printf '%s\n' "${output}" | sed -n 's/^Luna GPU profile: kernel_ms=//p')"
    if [[ -z "${kernel_ms}" ]]; then
        echo "Luna runtime did not report GPU profiling output: ${output}" >&2
        exit 1
    fi
    printf '%s\n%s\n' "$(elapsed_ms "${start}" "${end}")" "${kernel_ms}"
}

run_cpp23() {
    local mode="$1" start end output kernel_ms
    start="$(date +%s%N)"
    if [[ "${mode}" == "awaited" ]]; then
        output="$("${cpp_binary}" --awaited)"
    else
        output="$("${cpp_binary}")"
    fi
    end="$(date +%s%N)"
    if [[ "${output}" != *"checksum=29524"* ]]; then
        echo "C++23/HIP result mismatch: ${output}" >&2
        exit 1
    fi
    kernel_ms="$(printf '%s\n' "${output}" | sed -n 's/^kernel_ms=//p')"
    if [[ -z "${kernel_ms}" ]]; then
        echo "C++23/HIP did not report kernel time: ${output}" >&2
        exit 1
    fi
    printf '%s\n%s\n' "$(elapsed_ms "${start}" "${end}")" "${kernel_ms}"
}

luna_total=0
luna_kernel_total=0
cpp_stream_total=0
cpp_stream_kernel_total=0
cpp_awaited_total=0
cpp_awaited_kernel_total=0
echo "ROCm C++23/HIP comparison"
echo "  elements:   16777216 (64 MiB)"
echo "  passes:     10 transform passes + initialization"
echo "  architecture: ${architecture}"
echo "  iterations: ${iterations}"
echo "  Luna optimization: ${optimization} (device codegen: aggressive)"

for ((iteration = 1; iteration <= iterations; ++iteration)); do
    luna_report="$(run_luna)"
    luna_ms="$(printf '%s\n' "${luna_report}" | sed -n '1p')"
    luna_kernel_ms="$(printf '%s\n' "${luna_report}" | sed -n '2p')"
    cpp_stream_report="$(run_cpp23 stream)"
    cpp_stream_ms="$(printf '%s\n' "${cpp_stream_report}" | sed -n '1p')"
    cpp_stream_kernel_ms="$(printf '%s\n' "${cpp_stream_report}" | sed -n '2p')"
    cpp_awaited_report="$(run_cpp23 awaited)"
    cpp_awaited_ms="$(printf '%s\n' "${cpp_awaited_report}" | sed -n '1p')"
    cpp_awaited_kernel_ms="$(printf '%s\n' "${cpp_awaited_report}" | sed -n '2p')"
    luna_total="$(awk -v total="${luna_total}" -v value="${luna_ms}" 'BEGIN { print total + value }')"
    luna_kernel_total="$(awk -v total="${luna_kernel_total}" -v value="${luna_kernel_ms}" 'BEGIN { print total + value }')"
    cpp_stream_total="$(awk -v total="${cpp_stream_total}" -v value="${cpp_stream_ms}" 'BEGIN { print total + value }')"
    cpp_stream_kernel_total="$(awk -v total="${cpp_stream_kernel_total}" -v value="${cpp_stream_kernel_ms}" 'BEGIN { print total + value }')"
    cpp_awaited_total="$(awk -v total="${cpp_awaited_total}" -v value="${cpp_awaited_ms}" 'BEGIN { print total + value }')"
    cpp_awaited_kernel_total="$(awk -v total="${cpp_awaited_kernel_total}" -v value="${cpp_awaited_kernel_ms}" 'BEGIN { print total + value }')"
    printf '  run %d: Luna AOT wall=%sms, Luna kernel=%sms, C++23 stream wall=%sms, kernel=%sms, C++23 awaited wall=%sms, kernel=%sms\n' \
        "${iteration}" "${luna_ms}" "${luna_kernel_ms}" \
        "${cpp_stream_ms}" "${cpp_stream_kernel_ms}" \
        "${cpp_awaited_ms}" "${cpp_awaited_kernel_ms}"
done

printf 'average: Luna AOT wall=%sms, Luna kernel=%sms, C++23 stream wall=%sms, kernel=%sms, C++23 awaited wall=%sms, kernel=%sms\n' \
    "$(average_ms "${luna_total}" "${iterations}")" \
    "$(average_ms "${luna_kernel_total}" "${iterations}")" \
    "$(average_ms "${cpp_stream_total}" "${iterations}")" \
    "$(average_ms "${cpp_stream_kernel_total}" "${iterations}")" \
    "$(average_ms "${cpp_awaited_total}" "${iterations}")" \
    "$(average_ms "${cpp_awaited_kernel_total}" "${iterations}")"
echo "Note: Luna AOT wall time includes process startup, HIP initialization, module loading,"
echo "event handling, and one scalar readback. C++ stream is the unconstrained throughput reference;"
echo "C++ awaited creates, records, synchronizes, and destroys events after every launch to match Luna await."
