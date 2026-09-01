#!/usr/bin/env bash
# Gap-attribution analyzer for one Luna vs C++23 workload.
#
# Combines three signal families into one report:
#   1. static: optimized LLVM IR and assembly metrics (instruction count,
#      call sites, vector ops, memory ops) from the SAME LLVM 22 toolchain;
#   2. dynamic: process-level resources via tools/benchmark_probe.py
#      (user/sys time, max RSS, page faults, context switches);
#   3. startup decomposition: subtracts an empty-program baseline so the
#      pure workload time is visible next to the total wall time.
#
# Usage:
#   benchmark_analyze.sh <workload> <luna-source.luna> <cpp-source.cpp> \
#       <luna-driver> <source-root> [opt-level]
#
# Environment: CXX (default clang++), LUNA_ANALYZE_ITERATIONS (default 7),
# LUNA_ANALYZE_WARMUPS (default 2). Requires llvm-dis/opt/llc in PATH.
set -euo pipefail

workload="${1:?missing workload name}"
luna_source="${2:?missing Luna source}"
cpp_source="${3:?missing C++ source}"
luna_driver="${4:?missing Luna executable}"
source_root="${5:?missing source root}"
optimization="${6:--O3}"
iterations="${LUNA_ANALYZE_ITERATIONS:-7}"
warmups="${LUNA_ANALYZE_WARMUPS:-2}"
cpp_compiler="${CXX:-clang++}"

mca_mode=0
stripped=()
for arg in "$@"; do
    if [[ "${arg}" == "--mca" ]]; then
        mca_mode=1
    else
        stripped+=("${arg}")
    fi
done
set -- "${stripped[@]}"

mca_cpu="${LUNA_MCA_CPU:-}"
if [[ "${mca_mode}" == "1" && -z "${mca_cpu}" ]]; then
    if grep -qiE "zen4|7500F|7700X|7900X|7950X" /proc/cpuinfo; then
        mca_cpu="znver4"
    elif grep -qiE "zen3|5600X|5800X|5900X|5950X" /proc/cpuinfo; then
        mca_cpu="znver3"
    elif grep -qiE "zen2|3600X|3700X|3900X|3950X" /proc/cpuinfo; then
        mca_cpu="znver2"
    else
        mca_cpu="znver4"
    fi
fi

for tool in llvm-dis opt llc; do
    if ! command -v "${tool}" >/dev/null; then
        echo "missing ${tool} in PATH" >&2
        exit 2
    fi
done
if [[ "${mca_mode}" == "1" ]] && ! command -v llvm-mca >/dev/null; then
    echo "llvm-mca not found; disabling --mca" >&2
    mca_mode=0
fi

temp_dir="$(mktemp -d /tmp/luna-analyze.XXXXXX)"
cpp_bin="${temp_dir}/cpp_${workload}"
source "${source_root}/benchmarks/package_source.sh"
luna_aot=""

cleanup() {
    rm -rf -- "${temp_dir}"
}
trap cleanup EXIT

package_name="analyze_${workload//-/_}"
luna_benchmark_build_package "${luna_driver}" "${luna_source}" \
    "${temp_dir}/luna-package" "${package_name}" "${optimization}"
luna_aot="${LUNA_BENCHMARK_AOT}"
cp "${LUNA_BENCHMARK_IR}" "${temp_dir}/luna.raw.ll"

cpp_function_for() {
    case "$1" in
        arithmetic) echo arithmetic ;;
        branch) echo branchy ;;
        calls) echo calls ;;
        array) echo safe_array_reference ;;
        allocation) echo allocation ;;
        bitmix) echo bitmix ;;
        reduction) echo reduction ;;
        array-scan) echo array_scan ;;
        nested) echo nested ;;
        divmod) echo divmod ;;
        chase) echo chase ;;
        stream-read) echo stream_read ;;
        stream-write) echo stream_write ;;
        stream-copy) echo stream_copy ;;
        saxpy) echo saxpy ;;
        sort) echo sort ;;
        hash) echo hash ;;
        find) echo find ;;
        recursion) echo recursion ;;
        rotate) echo rotate ;;
        *) echo "${1//-/_}" ;;
    esac
}
cpp_workload="$(cpp_function_for "${workload}")"

"${cpp_compiler}" -std=c++23 "${optimization}" -DONLY_WORKLOAD="${cpp_workload}" \
    -S -emit-llvm \
    -Rpass=loop-vectorize -Rpass-missed=loop-vectorize \
    -Rpass-analysis=loop-vectorize \
    "${cpp_source}" -o "${temp_dir}/cpp.raw.ll" \
    2> "${temp_dir}/cpp.vec.remarks" || true
"${cpp_compiler}" -std=c++23 "${optimization}" -DONLY_WORKLOAD="${cpp_workload}" \
    "${cpp_source}" \
    "${source_root}/benchmarks/cpp23_allocation_support.cpp" \
    -o "${cpp_bin}"

opt "${optimization}" "${temp_dir}/luna.raw.ll" -o "${temp_dir}/luna.opt.bc"
opt "${optimization}" "${temp_dir}/cpp.raw.ll" -o "${temp_dir}/cpp.opt.bc"
llvm-dis "${temp_dir}/luna.opt.bc" -o "${temp_dir}/luna.opt.ll"
llvm-dis "${temp_dir}/cpp.opt.bc" -o "${temp_dir}/cpp.opt.ll"
llc "${optimization}" -o "${temp_dir}/luna.s" "${temp_dir}/luna.opt.bc"
llc "${optimization}" -o "${temp_dir}/cpp.s" "${temp_dir}/cpp.opt.bc"

opt -passes='loop-vectorize' \
    -pass-remarks=loop-vectorize -pass-remarks-missed=loop-vectorize \
    -pass-remarks-analysis=loop-vectorize \
    "${temp_dir}/luna.opt.bc" -o "${temp_dir}/luna.vec.bc" \
    2> "${temp_dir}/luna.vec.remarks" || true

ir_metrics() {
    local file="$1"
    awk '
        /^define / { functions++ }
        /^  / && !/^  ;/ && !/^  [a-zA-Z_.$][a-zA-Z0-9_.$]*:/ { instr++ }
        /^  / && / call / { calls++ }
        /^  / && / (load|store) / { memops++ }
        /^  / && / (br|switch) / { branches++ }
        /^  / && /<[0-9]+ x / { vector++ }
        END {
            printf "functions=%d instr=%d calls=%d memops=%d branches=%d vector=%d\n",
                functions, instr, calls, memops, branches, vector
        }' "$file"
}

asm_metrics() {
    local file="$1"
    awk '
        /^\t[a-z]/ { instr++ }
        /^\tcall/ { calls++ }
        /%[xyz]mm/ || /^\t[v][a-z]/ { simd++ }
        /^\tmov/ { movs++ }
        END {
            printf "instr=%d calls=%d simd=%d movs=%d\n", instr, calls, simd, movs
        }' "$file"
}

parse_metric() {
    local metrics="$1" key="$2"
    printf '%s' "${metrics}" | tr ' ' '\n' | awk -F= -v key="${key}" '$1 == key {print $2}'
}

luna_ir="$(ir_metrics "${temp_dir}/luna.opt.ll")"
cpp_ir="$(ir_metrics "${temp_dir}/cpp.opt.ll")"
luna_asm="$(asm_metrics "${temp_dir}/luna.s")"
cpp_asm="$(asm_metrics "${temp_dir}/cpp.s")"

luna_ir_instr="$(parse_metric "${luna_ir}" instr)"
cpp_ir_instr="$(parse_metric "${cpp_ir}" instr)"
luna_ir_calls="$(parse_metric "${luna_ir}" calls)"
cpp_ir_calls="$(parse_metric "${cpp_ir}" calls)"
luna_ir_vector="$(parse_metric "${luna_ir}" vector)"
cpp_ir_vector="$(parse_metric "${cpp_ir}" vector)"
luna_ir_memops="$(parse_metric "${luna_ir}" memops)"
cpp_ir_memops="$(parse_metric "${cpp_ir}" memops)"
luna_asm_instr="$(parse_metric "${luna_asm}" instr)"
cpp_asm_instr="$(parse_metric "${cpp_asm}" instr)"
luna_asm_calls="$(parse_metric "${luna_asm}" calls)"
cpp_asm_calls="$(parse_metric "${cpp_asm}" calls)"
luna_asm_simd="$(parse_metric "${luna_asm}" simd)"
cpp_asm_simd="$(parse_metric "${cpp_asm}" simd)"
luna_asm_movs="$(parse_metric "${luna_asm}" movs)"
cpp_asm_movs="$(parse_metric "${cpp_asm}" movs)"

python3 "${source_root}/tools/benchmark_probe.py" --iterations "${iterations}" \
    --warmups "${warmups}" --tag luna --perf -- "${luna_aot}" > "${temp_dir}/luna.probe.json"
python3 "${source_root}/tools/benchmark_probe.py" --iterations "${iterations}" \
    --warmups "${warmups}" --tag cpp --perf -- "${cpp_bin}" "${workload}" > "${temp_dir}/cpp.probe.json"

luna_summary="$(grep '"samples"' "${temp_dir}/luna.probe.json" | tail -n 1)"
cpp_summary="$(grep '"samples"' "${temp_dir}/cpp.probe.json" | tail -n 1)"

ratio() {
    awk -v a="$1" -v b="$2" 'BEGIN { if (b + 0 == 0) { print "inf"; exit } printf "%.2f", a / b }'
}

luna_wall="$(printf '%s' "${luna_summary}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["wall_ms_median"])')"
cpp_wall="$(printf '%s' "${cpp_summary}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["wall_ms_median"])')"
luna_rss="$(printf '%s' "${luna_summary}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["max_rss_kib_median"])')"
cpp_rss="$(printf '%s' "${cpp_summary}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["max_rss_kib_median"])')"

echo "=== gap analysis: ${workload} (${optimization}) ==="
echo "--- static (optimized LLVM IR, LLVM $(llvm-config --version)) ---"
echo "  Luna IR: ${luna_ir}   C++ IR: ${cpp_ir}"
echo "  IR instr ratio (Luna/C++): $(ratio "${luna_ir_instr}" "${cpp_ir_instr}")x"
echo "  IR call sites: Luna=${luna_ir_calls} C++=${cpp_ir_calls}"
echo "  IR vector ops: Luna=${luna_ir_vector} C++=${cpp_ir_vector}"
echo "  IR memory ops: Luna=${luna_ir_memops} C++=${cpp_ir_memops}"
echo "--- static (assembly, llc ${optimization}) ---"
echo "  asm instr: Luna=${luna_asm_instr} C++=${cpp_asm_instr} ratio=$(ratio "${luna_asm_instr}" "${cpp_asm_instr}")x"
echo "  asm calls: Luna=${luna_asm_calls} C++=${cpp_asm_calls}"
echo "  asm SIMD ops: Luna=${luna_asm_simd} C++=${cpp_asm_simd}"
echo "  asm movs: Luna=${luna_asm_movs} C++=${cpp_asm_movs}"
echo "--- vectorization diagnostics (loop-vectorize remarks) ---"
cpp_vec="$(grep -c 'loop vectorized' "${temp_dir}/cpp.vec.remarks" || true)"
cpp_missed="$(grep -c 'not vectorized' "${temp_dir}/cpp.vec.remarks" || true)"
luna_vec="$(grep -c 'loop vectorized' "${temp_dir}/luna.vec.remarks" || true)"
luna_missed="$(grep -c 'not vectorized' "${temp_dir}/luna.vec.remarks" || true)"
echo "  C++:  vectorized=${cpp_vec} not-vectorized=${cpp_missed}"
echo "  Luna: vectorized=${luna_vec} not-vectorized=${luna_missed}"
if [[ "${cpp_missed}" -gt 0 ]]; then
    echo "  C++ missed reasons (first 3):"
    grep 'not vectorized' "${temp_dir}/cpp.vec.remarks" | sed 's/^/    /' | head -n 3
fi
if [[ "${luna_missed}" -gt 0 ]]; then
    echo "  Luna missed reasons (first 3):"
    grep 'not vectorized' "${temp_dir}/luna.vec.remarks" | sed 's/^/    /' | head -n 3
fi
if [[ "${cpp_missed}" -eq 0 && "${luna_missed}" -eq 0 ]]; then
    echo "  (no missed-vectorization remarks emitted)"
fi

echo "--- hardware counters (perf stat, if available) ---"
luna_perf="$(grep '"perf"' "${temp_dir}/luna.probe.json" | tail -n 1)"
cpp_perf="$(grep '"perf"' "${temp_dir}/cpp.probe.json" | tail -n 1)"
luna_perf_value="$(printf '%s' "${luna_perf}" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("perf", "missing"))' 2>/dev/null || echo missing)"
cpp_perf_value="$(printf '%s' "${cpp_perf}" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("perf", "missing"))' 2>/dev/null || echo missing)"
if [[ "${luna_perf_value}" == *"not installed"* ]]; then
    echo "  perf is not installed; install linux-tools to get instruction/branch/cache counters"
elif [[ "${luna_perf_value}" != "missing" && "${luna_perf_value}" != "{}" ]]; then
    echo "  Luna: ${luna_perf_value}"
    echo "  C++:  ${cpp_perf_value}"
    luna_instr="$(printf '%s' "${luna_perf_value}" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("instructions", 0))')"
    cpp_instr="$(printf '%s' "${cpp_perf_value}" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("instructions", 0))')"
    echo "  instructions ratio (Luna/C++): $(ratio "${luna_instr}" "${cpp_instr}")x"
else
    echo "  perf present but did not count events (may need permissions)"
fi

echo "--- dynamic (probe, median of ${iterations}) ---"
echo "  wall: Luna=${luna_wall}ms C++=${cpp_wall}ms ratio=$(ratio "${luna_wall}" "${cpp_wall}")x"
echo "  max RSS: Luna=${luna_rss}KiB C++=${cpp_rss}KiB ratio=$(ratio "${luna_rss}" "${cpp_rss}")x"

signals=""
if awk -v l="${luna_wall}" -v c="${cpp_wall}" 'BEGIN { exit !(l > c * 1.3) }'; then
    if awk -v l="${luna_asm_simd}" -v c="${cpp_asm_simd}" \
        'BEGIN { exit !(c > 8 && l < c / 4) }'; then
        signals="${signals} [C++ 自动向量化而 Luna 为标量循环]"
    fi
    if awk -v l="${luna_ir_instr}" -v c="${cpp_ir_instr}" \
        'BEGIN { exit !(l > c * 1.3) }'; then
        signals="${signals} [Luna IR 指令量高 ${luna_ir_instr} vs ${cpp_ir_instr}]"
    fi
    if awk -v l="${luna_asm_calls}" -v c="${cpp_asm_calls}" \
        'BEGIN { exit !(l > c + 5) }'; then
        signals="${signals} [热路径内联不足: asm call Luna=${luna_asm_calls} C++=${cpp_asm_calls}]"
    fi
    if awk -v l="${luna_rss}" -v c="${cpp_rss}" 'BEGIN { exit !(l > c * 1.5) }'; then
        signals="${signals} [内存占用高: RSS ${luna_rss} vs ${cpp_rss} KiB]"
    fi
    if [[ -z "${signals}" ]]; then
        signals=" [未命中已知信号,需 perf/llvm-mca 级分析]"
    fi
else
    signals=" [Luna 不慢于 C++ 1.3x, 无需归因]"
fi
echo "--- attribution ---"
luna_rt_calls="$(grep -c 'call .*@rt_' "${temp_dir}/luna.opt.ll" || true)"
rt_signals=""
if [[ "${luna_rt_calls}" -gt 0 ]]; then
    rt_signals=" [Luna 热路径调用运行时守卫 ${luna_rt_calls} 处, 如 rt_array_index_or_abort 边界检查]"
fi
echo "  ${rt_signals}${signals}"
if [[ "${mca_mode}" == "1" ]]; then
    echo "--- llvm-mca (${mca_cpu}) largest function ---"
    extract_hot_fn() {
        local asm_file="$1"
        awk '
            /^[a-zA-Z_.$][a-zA-Z0-9_.$]*:/ { if (name != "") print count, name; name = $0; count = 0; next }
            /^\t[a-z]/ { count++ }
            END { if (name != "") print count, name }
        ' "${asm_file}" | sort -k1 -n | tail -n 1 | cut -d' ' -f2-
    }
    fn_of() {
        local asm_file="$1" fn_name="$2" in_fn=0
        awk -v fn="${fn_name}" '
            index($0, fn) == 1 { in_fn = 1; next }
            in_fn && /^[a-zA-Z_.$][a-zA-Z0-9_.$]*:/ { exit }
            in_fn && /^\t[a-z]/ { print }
        ' "${asm_file}"
    }
    read -r luna_fn luna_size <<< "$(extract_hot_fn "${temp_dir}/luna.s")"
    read -r cpp_fn cpp_size <<< "$(extract_hot_fn "${temp_dir}/cpp.s")"
    fn_of "${temp_dir}/luna.s" "${luna_fn}" > "${temp_dir}/luna.hot.s"
    fn_of "${temp_dir}/cpp.s" "${cpp_fn}" > "${temp_dir}/cpp.hot.s"
    llvm-mca --mcpu="${mca_cpu}" --iterations=100 "${temp_dir}/luna.hot.s"         2>/dev/null | grep -E "^Total Cycles:|^IPC:|^Block RThroughput"         | sed 's/^/  Luna '"${luna_fn}"' /' || echo "  Luna: mca failed"
    llvm-mca --mcpu="${mca_cpu}" --iterations=100 "${temp_dir}/cpp.hot.s"         2>/dev/null | grep -E "^Total Cycles:|^IPC:|^Block RThroughput"         | sed 's/^/  C++ '"${cpp_fn}"' /' || echo "  C++: mca failed"
fi

echo "--- startup decomposition (empty-program baseline) ---"

luna_empty="${temp_dir}/luna_empty.luna"
cpp_empty="${temp_dir}/cpp_empty.cpp"
printf 'fn main() -> i32 {\n    print(0);\n    return 0;\n}\n' > "${luna_empty}"
printf '#include <cstdio>\nint main() { std::printf("0\\n"); return 0; }\n' > "${cpp_empty}"
luna_benchmark_build_package "${luna_driver}" "${luna_empty}" \
    "${temp_dir}/empty-package" empty_baseline "${optimization}" >/dev/null 2>&1
"${cpp_compiler}" -std=c++23 "${optimization}" "${cpp_empty}" -o "${temp_dir}/cpp_empty"
luna_baseline_bin="${LUNA_BENCHMARK_AOT}"

python3 "${source_root}/tools/benchmark_probe.py" --iterations 5 --warmups 1 \
    --tag luna-baseline -- "${luna_baseline_bin}" > "${temp_dir}/luna_base.json" 2>/dev/null
python3 "${source_root}/tools/benchmark_probe.py" --iterations 5 --warmups 1 \
    --tag cpp-baseline -- "${temp_dir}/cpp_empty" > "${temp_dir}/cpp_base.json" 2>/dev/null

luna_base="$(tail -n 1 "${temp_dir}/luna_base.json" | python3 -c 'import json,sys; print(json.load(sys.stdin)["wall_ms_median"])')"
cpp_base="$(tail -n 1 "${temp_dir}/cpp_base.json" | python3 -c 'import json,sys; print(json.load(sys.stdin)["wall_ms_median"])')"
echo "  empty-program baseline: Luna=${luna_base}ms C++=${cpp_base}ms"
echo "  workload-only (wall - baseline): Luna=$(awk -v w="${luna_wall}" -v b="${luna_base}" 'BEGIN { printf "%.3f", w - b }')ms"
echo "  workload-only (wall - baseline): C++=$(awk -v w="${cpp_wall}" -v b="${cpp_base}" 'BEGIN { printf "%.3f", w - b }')ms"
echo "  workload-only ratio: $(awk -v l="${luna_wall}" -v lc="${luna_base}" -v c="${cpp_wall}" -v cc="${cpp_base}" 'BEGIN { printf "%.2f", (l - lc) / (c - cc) }')x"
