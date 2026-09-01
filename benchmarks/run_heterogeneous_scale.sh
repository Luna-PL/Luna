#!/usr/bin/env bash
# Heterogeneous-compute scale comparison: Luna AOT vs C++23/HIP across an
# 8 MiB .. 1 GiB size sweep, compute-intensity sweep (1x/4x/16x), transfer
# roundtrip and launch-overhead microbenchmarks. Also runs the Luna-only
# CPU simulator sweep at small sizes (the simulator executes one host thread
# per element, so 1 GiB is not meaningful there).
#
# Environment:
#   LUNA_GPU_BACKEND        rocm (default) or sim
#   LUNA_GPU_TARGET         rocm target, default gfx1101
#   LUNA_HETERO_SIZES       space-separated MiB list, default 8 16 32 64 128 256 512 1024
#   LUNA_HETERO_OPS         space-separated intensity list, default 1 4 16
#   LUNA_HETERO_ITERATIONS  measured runs per configuration, default 5
#   LUNA_HETERO_WARMUPS     warmups per configuration, default 2
#   LUNA_HETERO_SIM_MAX_MIB simulator size cap, default 64
#   LUNA_HETERO_OUT         optional TSV summary path
#
# Wall time includes process startup, HIP initialization, module loading and
# synchronization; LUNA_GPU_PROFILE=1 separates device-event kernel time.
set -euo pipefail

luna_driver="${1:?missing Luna executable}"
source_root="${2:?missing source root}"
backend="${LUNA_GPU_BACKEND:-rocm}"
gpu_target="${LUNA_GPU_TARGET:-gfx1101}"
sizes="${LUNA_HETERO_SIZES:-8 16 32 64 128 256 512 1024}"
ops_list="${LUNA_HETERO_OPS:-1 4 16}"
iterations="${LUNA_HETERO_ITERATIONS:-5}"
warmups="${LUNA_HETERO_WARMUPS:-2}"
sim_max_mib="${LUNA_HETERO_SIM_MAX_MIB:-64}"
out_file="${LUNA_HETERO_OUT:-}"

temp_dir="$(mktemp -d /tmp/luna-hetero-scale.XXXXXX)"
gen_dir="${temp_dir}/src"
luna_hip_bin="${temp_dir}/cpp23_hip_vector"
source "${source_root}/benchmarks/package_source.sh"

cleanup() {
    rm -rf -- "${temp_dir}"
}
trap cleanup EXIT

if [[ ! -x "${luna_driver}" ]]; then
    echo "Luna driver is not executable: ${luna_driver}" >&2
    exit 2
fi
if ! [[ "${iterations}" =~ ^[1-9][0-9]*$ ]] || ! [[ "${warmups}" =~ ^[0-9]+$ ]]; then
    echo "LUNA_HETERO_ITERATIONS must be positive, LUNA_HETERO_WARMUPS non-negative" >&2
    exit 2
fi
case "${backend}" in
    rocm|sim) ;;
    *) echo "LUNA_GPU_BACKEND must be rocm or sim" >&2; exit 2 ;;
esac

python3 "${source_root}/tools/gen_heterogeneous_scale.py" "${gen_dir}" \
    --sizes ${sizes} --ops ${ops_list} >/dev/null

if [[ "${backend}" == "rocm" ]]; then
    if ! command -v hipcc >/dev/null; then
        echo "hipcc not found; cannot build the C++23/HIP counterpart" >&2
        exit 2
    fi
    hipcc -std=c++23 -O3 "--offload-arch=${gpu_target}" \
        "${source_root}/benchmarks/cpp23_hip_vector.cpp" -o "${luna_hip_bin}"
fi

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

now_ns() { date +%s%N; }
elapsed_ms() {
    awk -v start="$1" -v end="$2" 'BEGIN { printf "%.3f", (end - start) / 1000000 }'
}
average_ms() {
    awk -v total="$1" -v count="$2" 'BEGIN { printf "%.3f", total / count }'
}

if [[ -n "${out_file}" ]]; then
    printf 'backend\tsize_mib\tops\tmode\timpl\twall_ms\tkernel_ms\n' > "${out_file}"
fi

run_luna_aot() {
    local source="$1" backend_name="$2" target_flag="$3"
    local stem package_name package_root binary
    stem="${source##*/}"
    stem="${stem%.luna}"
    package_name="${stem//-/_}"
    package_name="${package_name,,}"
    package_root="${temp_dir}/packages/${backend_name}/${package_name}"
    luna_benchmark_build_package "${luna_driver}" "${source}" \
        "${package_root}" "${package_name}" -O2 "${target_flag}" || return
    binary="${LUNA_BENCHMARK_AOT}"
    local start end output wall kernel_ms
    start="$(now_ns)"
    output="$("${bench_prefix[@]}" env LUNA_GPU_BACKEND="${backend_name}" LUNA_GPU_PROFILE=1 "${binary}")"
    end="$(now_ns)"
    kernel_ms="$(printf '%s\n' "${output}" | sed -n 's/^Luna GPU profile: kernel_ms=//p')"
    checksum="$(printf '%s\n' "${output}" | grep -v '^Luna GPU profile:' | grep -v '^Program exited with code:' | tail -n 1)"
    printf '%s\n%s\n%s\n' "$(elapsed_ms "${start}" "${end}")" \
        "${kernel_ms:-N/A}" "${checksum}"
}

run_luna_jit() {
    local source="$1" backend_name="$2" target_flag="$3"
    local start end output wall kernel_ms
    start="$(now_ns)"
    output="$("${bench_prefix[@]}" env LUNA_GPU_BACKEND="${backend_name}" LUNA_GPU_PROFILE=1 \
        "${luna_driver}" run "${source}" -O2 "${target_flag}")"
    end="$(now_ns)"
    kernel_ms="$(printf '%s\n' "${output}" | sed -n 's/^Luna GPU profile: kernel_ms=//p')"
    checksum="$(printf '%s\n' "${output}" | grep -v '^Luna GPU profile:' | grep -v '^Program exited with code:' | tail -n 1)"
    printf '%s\n%s\n%s\n' "$(elapsed_ms "${start}" "${end}")" \
        "${kernel_ms:-N/A}" "${checksum}"
}

run_cpp() {
    local mode_name="$1"
    shift
    local start end output wall kernel_ms checksum_line
    start="$(now_ns)"
    output="$("${bench_prefix[@]}" "${luna_hip_bin}" "$@")"
    end="$(now_ns)"
    kernel_ms="$(printf '%s\n' "${output}" | sed -n 's/^kernel_ms=//p')"
    if [[ -z "${kernel_ms}" ]]; then
        kernel_ms="$(printf '%s\n' "${output}" | sed -n 's/^transfer_ms=//p')"
    fi
    checksum_line="$(printf '%s\n' "${output}" | sed -n 's/^checksum=//p')"
    printf '%s\n%s\n%s\n' "$(elapsed_ms "${start}" "${end}")" \
        "${kernel_ms:-N/A}" "${checksum_line}"
}

report_row() {
    local size="$1" ops="$2" label="$3" impl="$4" wall="$5" kernel="$6" checksum="$7"
    printf '  %-5s %-4s %-10s %-6s wall=%-9s kernel=%-9s checksum=%s\n' \
        "${size}MiB" "${ops}x" "${label}" "${impl}" "${wall}ms" "${kernel}ms" "${checksum}"
    if [[ -n "${out_file}" ]]; then
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "${backend}" "${size}" "${ops}" \
            "${label}" "${impl}" "${wall}" "${kernel}" >> "${out_file}"
    fi
}

echo "Luna heterogeneous scale comparison"
echo "  backend: ${backend}  target: ${gpu_target}"
echo "  sizes: ${sizes} MiB   ops: ${ops_list}   iterations: ${iterations}  warmups: ${warmups}"
echo "  host: $(uname -srm)"
if command -v lscpu >/dev/null; then
    echo "  cpu: $(lscpu | awk -F'[:：] +' '/Model name|型号名称/ {print $2; exit}')"
fi
echo "  Luna: $("${luna_driver}" --version 2>&1 | head -n 1)"
if [[ "${backend}" == "rocm" ]]; then
    echo "  HIP: $(hipcc --version | sed -n '3p')"
fi
echo "  note: wall includes startup/HIP init/module load/sync; kernel is device-event time"

if [[ "${backend}" == "rocm" ]]; then
    echo "--- vector sweep (10 passes, initialization + readback) ---"
    for size in ${sizes}; do
        for ops in ${ops_list}; do
            source="${gen_dir}/vector_${size}MiB_ops${ops}.luna"
            target_flag="--gpu-target=rocm:${gpu_target}"
            # Warmups
            for ((w = 0; w < warmups; ++w)); do
                run_luna_aot "${source}" rocm "${target_flag}" >/dev/null
                run_cpp "cpp" --elements="$((size * 1024 * 1024 / 4))" --passes=10 --ops="${ops}" >/dev/null
                run_cpp "cpp" --elements="$((size * 1024 * 1024 / 4))" --passes=10 --ops="${ops}" --awaited >/dev/null
            done
            for ((r = 1; r <= iterations; ++r)); do
                if ((r % 2 == 1)); then
                    luna_first=1
                else
                    luna_first=0
                fi
                if [[ "${luna_first}" == "1" ]]; then
                    luna_report="$(run_luna_aot "${source}" rocm "${target_flag}")"
                    cpp_report="$(run_cpp "cpp" --elements="$((size * 1024 * 1024 / 4))" --passes=10 --ops="${ops}")"
                    cpp_awaited_report="$(run_cpp "cpp" --elements="$((size * 1024 * 1024 / 4))" --passes=10 --ops="${ops}" --awaited)"
                else
                    cpp_report="$(run_cpp "cpp" --elements="$((size * 1024 * 1024 / 4))" --passes=10 --ops="${ops}")"
                    cpp_awaited_report="$(run_cpp "cpp" --elements="$((size * 1024 * 1024 / 4))" --passes=10 --ops="${ops}" --awaited)"
                    luna_report="$(run_luna_aot "${source}" rocm "${target_flag}")"
                fi
                luna_checksum="$(printf '%s\n' "${luna_report}" | sed -n '3p')"
                cpp_checksum="$(printf '%s\n' "${cpp_report}" | sed -n '3p')"
                if [[ "${luna_checksum}" != "${cpp_checksum}" ]]; then
                    echo "checksum mismatch at ${size}MiB ops${ops}: luna=${luna_checksum} cpp=${cpp_checksum}" >&2
                    exit 1
                fi
                printf '  %-5s %-4s run %d: Luna wall=%sms kernel=%sms | C++ stream wall=%sms kernel=%sms | C++ awaited wall=%sms kernel=%sms | checksum=%s\n' \
                    "${size}MiB" "${ops}x" "${r}" \
                    "$(printf '%s\n' "${luna_report}" | sed -n '1p')" "$(printf '%s\n' "${luna_report}" | sed -n '2p')" \
                    "$(printf '%s\n' "${cpp_report}" | sed -n '1p')" "$(printf '%s\n' "${cpp_report}" | sed -n '2p')" \
                    "$(printf '%s\n' "${cpp_awaited_report}" | sed -n '1p')" "$(printf '%s\n' "${cpp_awaited_report}" | sed -n '2p')" \
                    "${luna_checksum}"
            done
            report_row "${size}" "${ops}" "vector" "luna-aot" \
                "$(printf '%s\n' "${luna_report}" | sed -n '1p')" \
                "$(printf '%s\n' "${luna_report}" | sed -n '2p')" "${luna_checksum}"
            report_row "${size}" "${ops}" "vector" "cpp-stream" \
                "$(printf '%s\n' "${cpp_report}" | sed -n '1p')" \
                "$(printf '%s\n' "${cpp_report}" | sed -n '2p')" "${cpp_checksum}"
            report_row "${size}" "${ops}" "vector" "cpp-awaited" \
                "$(printf '%s\n' "${cpp_awaited_report}" | sed -n '1p')" \
                "$(printf '%s\n' "${cpp_awaited_report}" | sed -n '2p')" \
                "$(printf '%s\n' "${cpp_awaited_report}" | sed -n '3p')"
        done
    done

    echo "--- transfer roundtrip (H2D + D2H, no compute) ---"
    for size in ${sizes}; do
        source="${gen_dir}/transfer_${size}MiB.luna"
        target_flag="--gpu-target=rocm:${gpu_target}"
        for ((w = 0; w < warmups; ++w)); do
            run_luna_aot "${source}" rocm "${target_flag}" >/dev/null
            run_cpp "cpp" --mode=transfer --elements="$((size * 1024 * 1024 / 4))" >/dev/null
        done
        for ((r = 1; r <= iterations; ++r)); do
            if ((r % 2 == 1)); then
                luna_report="$(run_luna_aot "${source}" rocm "${target_flag}")"
                cpp_report="$(run_cpp "cpp" --mode=transfer --elements="$((size * 1024 * 1024 / 4))")"
            else
                cpp_report="$(run_cpp "cpp" --mode=transfer --elements="$((size * 1024 * 1024 / 4))")"
                luna_report="$(run_luna_aot "${source}" rocm "${target_flag}")"
            fi
            luna_checksum="$(printf '%s\n' "${luna_report}" | sed -n '3p')"
            cpp_checksum="$(printf '%s\n' "${cpp_report}" | sed -n '3p')"
            if [[ "${luna_checksum}" != "${cpp_checksum}" ]]; then
                echo "transfer checksum mismatch at ${size}MiB: luna=${luna_checksum} cpp=${cpp_checksum}" >&2
                exit 1
            fi
            printf '  %-5s run %d: Luna wall=%sms | C++ wall=%sms | checksum=%s\n' \
                "${size}MiB" "${r}" \
                "$(printf '%s\n' "${luna_report}" | sed -n '1p')" \
                "$(printf '%s\n' "${cpp_report}" | sed -n '1p')" \
                "${luna_checksum}"
        done
        report_row "${size}" "1" "transfer" "luna-aot" \
            "$(printf '%s\n' "${luna_report}" | sed -n '1p')" N/A "${luna_checksum}"
        report_row "${size}" "1" "transfer" "cpp" \
            "$(printf '%s\n' "${cpp_report}" | sed -n '1p')" \
            "$(printf '%s\n' "${cpp_report}" | sed -n '2p')" "${cpp_checksum}"
    done

    echo "--- launch overhead (1000 sequential launch/await, 8 threads) ---"
    source="${gen_dir}/launch_1000.luna"
    target_flag="--gpu-target=rocm:${gpu_target}"
    for ((w = 0; w < warmups; ++w)); do
        run_luna_aot "${source}" rocm "${target_flag}" >/dev/null
        run_cpp "cpp" --mode=launch --launches=1000 >/dev/null
    done
    for ((r = 1; r <= iterations; ++r)); do
        if ((r % 2 == 1)); then
            luna_report="$(run_luna_aot "${source}" rocm "${target_flag}")"
            cpp_report="$(run_cpp "cpp" --mode=launch --launches=1000)"
        else
            cpp_report="$(run_cpp "cpp" --mode=launch --launches=1000)"
            luna_report="$(run_luna_aot "${source}" rocm "${target_flag}")"
        fi
        luna_checksum="$(printf '%s\n' "${luna_report}" | sed -n '3p')"
        cpp_checksum="$(printf '%s\n' "${cpp_report}" | sed -n '3p')"
        if [[ "${luna_checksum}" != "${cpp_checksum}" ]]; then
            echo "launch checksum mismatch: luna=${luna_checksum} cpp=${cpp_checksum}" >&2
            exit 1
        fi
        printf '  run %d: Luna wall=%sms kernel=%sms | C++ awaited wall=%sms kernel=%sms | checksum=%s\n' \
            "${r}" \
            "$(printf '%s\n' "${luna_report}" | sed -n '1p')" "$(printf '%s\n' "${luna_report}" | sed -n '2p')" \
            "$(printf '%s\n' "${cpp_report}" | sed -n '1p')" "$(printf '%s\n' "${cpp_report}" | sed -n '2p')" \
            "${luna_checksum}"
    done
    report_row "1" "1" "launch1000" "luna-aot" \
        "$(printf '%s\n' "${luna_report}" | sed -n '1p')" \
        "$(printf '%s\n' "${luna_report}" | sed -n '2p')" "${luna_checksum}"
    report_row "1" "1" "launch1000" "cpp-awaited" \
        "$(printf '%s\n' "${cpp_report}" | sed -n '1p')" \
        "$(printf '%s\n' "${cpp_report}" | sed -n '2p')" "${cpp_checksum}"
fi

echo "--- simulator sweep (Luna-only; CPU threads = elements) ---"
sim_sizes=""
for size in ${sizes}; do
    if ((size <= sim_max_mib)); then
        sim_sizes="${sim_sizes} ${size}"
    fi
done
for size in ${sim_sizes}; do
    for ops in ${ops_list}; do
        source="${gen_dir}/vector_${size}MiB_ops${ops}.luna"
        target_flag="--gpu-target=sim"
        for ((w = 0; w < warmups; ++w)); do
            run_luna_jit "${source}" sim "${target_flag}" >/dev/null
            run_luna_aot "${source}" sim "${target_flag}" >/dev/null
        done
        for ((r = 1; r <= iterations; ++r)); do
            if ((r % 2 == 1)); then
                jit_report="$(run_luna_jit "${source}" sim "${target_flag}")"
                aot_report="$(run_luna_aot "${source}" sim "${target_flag}")"
            else
                aot_report="$(run_luna_aot "${source}" sim "${target_flag}")"
                jit_report="$(run_luna_jit "${source}" sim "${target_flag}")"
            fi
            jit_checksum="$(printf '%s\n' "${jit_report}" | sed -n '3p')"
            aot_checksum="$(printf '%s\n' "${aot_report}" | sed -n '3p')"
            if [[ "${jit_checksum}" != "${aot_checksum}" ]]; then
                echo "sim checksum mismatch at ${size}MiB ops${ops}: jit=${jit_checksum} aot=${aot_checksum}" >&2
                exit 1
            fi
            printf '  %-5s %-4s run %d: JIT wall=%sms kernel=%sms | AOT wall=%sms kernel=%sms | checksum=%s\n' \
                "${size}MiB" "${ops}x" "${r}" \
                "$(printf '%s\n' "${jit_report}" | sed -n '1p')" "$(printf '%s\n' "${jit_report}" | sed -n '2p')" \
                "$(printf '%s\n' "${aot_report}" | sed -n '1p')" "$(printf '%s\n' "${aot_report}" | sed -n '2p')" \
                "${jit_checksum}"
        done
        report_row "${size}" "${ops}" "sim-vector" "luna-jit" \
            "$(printf '%s\n' "${jit_report}" | sed -n '1p')" \
            "$(printf '%s\n' "${jit_report}" | sed -n '2p')" "${jit_checksum}"
        report_row "${size}" "${ops}" "sim-vector" "luna-aot" \
            "$(printf '%s\n' "${aot_report}" | sed -n '1p')" \
            "$(printf '%s\n' "${aot_report}" | sed -n '2p')" "${aot_checksum}"
    done
done

echo "done"
