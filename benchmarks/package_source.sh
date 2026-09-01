#!/usr/bin/env bash

# Build one benchmark source through the Luna 0.3 application-package boundary.
# The caller owns package_root and is expected to place it under a temporary
# directory. Results are returned in LUNA_BENCHMARK_AOT and LUNA_BENCHMARK_IR.
luna_benchmark_build_package() {
    local luna_driver="$1"
    local source="$2"
    local package_root="$3"
    local package_name="$4"
    local optimization="$5"
    shift 5

    if ! [[ "${package_name}" =~ ^[a-z][a-z0-9_]*$ ]]; then
        echo "invalid benchmark package name: ${package_name}" >&2
        return 2
    fi

    mkdir -p "${package_root}/src"
    cp -- "${source}" "${package_root}/src/main.luna"
    printf '[package]\nid = "org.luna.benchmark.%s"\nversion = "0.3.0"\nkind = "application"\nsources = ["src"]\n' \
        "${package_name}" > "${package_root}/luna.package"
    "${luna_driver}" build "${package_root}" "${optimization}" "$@" >/dev/null

    local executable_suffix=""
    if [[ "${OS:-}" == "Windows_NT" ]]; then
        executable_suffix=".exe"
    fi
    LUNA_BENCHMARK_AOT="${package_root}/build/native/${package_name}${executable_suffix}"
    LUNA_BENCHMARK_IR="${package_root}/build/native/${package_name}${executable_suffix}.ll"
    if [[ ! -x "${LUNA_BENCHMARK_AOT}" || ! -f "${LUNA_BENCHMARK_IR}" ]]; then
        echo "Luna benchmark package did not produce expected native artifacts" >&2
        return 1
    fi
}
