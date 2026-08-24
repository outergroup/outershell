#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
HELPER_SOURCE="${REPO_ROOT}/SocketBridge/outer-socket-bridge.c"
GLIBC_OUTPUT_DIR="${REPO_ROOT}/build/linux-package/RemoteLinuxBinaries"
MUSL_OUTPUT_DIR="${REPO_ROOT}/build/linux-package/RemoteLinuxBinariesMusl"
BUILD_CONTAINER_RUNTIME=""

apple_container_is_ready() {
    command -v container >/dev/null 2>&1 &&
        container system status >/dev/null 2>&1
}

docker_is_ready() {
    command -v docker >/dev/null 2>&1 &&
        docker info >/dev/null 2>&1
}

select_build_container_runtime() {
    case "${OUTER_BUILD_CONTAINER_RUNTIME:-auto}" in
        auto)
            if [[ "$(uname -s)" == Darwin ]] && apple_container_is_ready; then
                BUILD_CONTAINER_RUNTIME=container
            elif docker_is_ready; then
                BUILD_CONTAINER_RUNTIME=docker
            elif apple_container_is_ready; then
                BUILD_CONTAINER_RUNTIME=container
            else
                return 1
            fi
            ;;
        container)
            apple_container_is_ready || return 1
            BUILD_CONTAINER_RUNTIME=container
            ;;
        docker)
            docker_is_ready || return 1
            BUILD_CONTAINER_RUNTIME=docker
            ;;
        *)
            echo "error: OUTER_BUILD_CONTAINER_RUNTIME must be auto, container, or docker" >&2
            return 2
            ;;
    esac
}

require_build_container_runtime() {
    select_build_container_runtime && return 0
    local status=$?
    [[ "${status}" -ne 2 ]] || exit 1
    echo "error: no supported build container runtime is ready" >&2
    echo "       start Apple container or Docker, or set OUTER_BUILD_CONTAINER_RUNTIME" >&2
    exit 1
}

build_variant() {
    local libc="$1"
    local arch output output_dir

    if [[ "$(uname -s)" != "Linux" ]]; then
        echo "error: --build-variant must be run on Linux" >&2
        exit 1
    fi

    case "$(uname -m)" in
        aarch64|arm64)
            arch="aarch64"
            ;;
        x86_64|amd64)
            arch="x86_64"
            ;;
        *)
            echo "error: unsupported Linux architecture: $(uname -m)" >&2
            exit 1
            ;;
    esac

    case "${libc}" in
        glibc|musl)
            ;;
        *)
            echo "error: unsupported Linux libc: ${libc}" >&2
            exit 1
            ;;
    esac

    output_dir="${OUTER_SOCKET_BRIDGE_OUTPUT_DIR:-/out}"
    mkdir -p "${output_dir}"
    if [[ ! -s "${HELPER_SOURCE}" ]]; then
        echo "error: could not find helper source at ${HELPER_SOURCE}" >&2
        exit 1
    fi

    output="${output_dir}/outer-socket-bridge-linux-${arch}-${libc}"
    cc -x c -std=gnu17 -O2 -Wall -Wextra -o "${output}" "${HELPER_SOURCE}"
    strip --strip-unneeded "${output}"
    "${output}" --version

    if ! readelf -l "${output}" | grep -F "Requesting program interpreter" >/dev/null; then
        echo "error: ${output} is not dynamically linked" >&2
        exit 1
    fi

    echo "Built outer-socket-bridge Linux helper for ${arch}/${libc}"
}

matrix_is_current() {
    local libc arch output_dir output
    for libc in glibc musl; do
        if [[ "${libc}" == glibc ]]; then
            output_dir="${GLIBC_OUTPUT_DIR}"
        else
            output_dir="${MUSL_OUTPUT_DIR}"
        fi
        for arch in aarch64 x86_64; do
            output="${output_dir}/${arch}/outer-socket-bridge"
            if [[ ! -x "${output}" || "${HELPER_SOURCE}" -nt "${output}" ]]; then
                return 1
            fi
        done
    done
    return 0
}

build_matrix() {
    local libc arch image platform output_name output_dir staging_dir build_input_dir

    if matrix_is_current; then
        echo "==> Outer Shell socket bridge matrix is current"
        return
    fi

    require_build_container_runtime
    mkdir -p "${REPO_ROOT}/build/linux-package"
    staging_dir="$(mktemp -d "${REPO_ROOT}/build/linux-package/.outer-socket-bridge-build.XXXXXX")"
    build_input_dir="$(mktemp -d "${TMPDIR:-/tmp}/outer-socket-bridge-input.XXXXXX")"
    trap 'rm -rf "${staging_dir:-}" "${build_input_dir:-}"' EXIT
    mkdir -p "${build_input_dir}/SocketBridge" "${build_input_dir}/Scripts"
    cp "${HELPER_SOURCE}" "${build_input_dir}/SocketBridge/"
    cp "${BASH_SOURCE[0]}" "${build_input_dir}/Scripts/"

    for libc in glibc musl; do
        for arch in aarch64 x86_64; do
            if [[ "${libc}" == "musl" ]]; then
                image="quay.io/pypa/musllinux_1_2_${arch}"
            else
                image="quay.io/pypa/manylinux2014_${arch}"
            fi
            if [[ "${arch}" == "aarch64" ]]; then
                platform="linux/arm64"
            else
                platform="linux/amd64"
            fi

            echo "==> Building outer-socket-bridge for Linux/${arch}/${libc}"
            if [[ "${BUILD_CONTAINER_RUNTIME}" == container ]]; then
                container run --rm --platform "${platform}" \
                    --mount "type=bind,source=${build_input_dir},target=/work,readonly" \
                    --mount "type=bind,source=${staging_dir},target=/out" \
                    --workdir /work \
                    --env OUTER_SOCKET_BRIDGE_OUTPUT_DIR=/out \
                    "${image}" \
                    bash ./Scripts/build_socket_bridge_helpers.sh --build-variant "${libc}"
            else
                docker run --rm --platform "${platform}" \
                    --network none \
                    --mount "type=bind,source=${build_input_dir},target=/work,readonly" \
                    --mount "type=bind,source=${staging_dir},target=/out" \
                    --workdir /work \
                    --env OUTER_SOCKET_BRIDGE_OUTPUT_DIR=/out \
                    "${image}" \
                    bash ./Scripts/build_socket_bridge_helpers.sh --build-variant "${libc}"
            fi
        done
    done

    for libc in glibc musl; do
        if [[ "${libc}" == glibc ]]; then
            output_dir="${GLIBC_OUTPUT_DIR}"
        else
            output_dir="${MUSL_OUTPUT_DIR}"
        fi
        for arch in aarch64 x86_64; do
            output_name="outer-socket-bridge-linux-${arch}-${libc}"
            mkdir -p "${output_dir}/${arch}"
            mv "${staging_dir}/${output_name}" "${output_dir}/${arch}/outer-socket-bridge"
        done
    done
    rm -rf "${staging_dir}"
    rm -rf "${build_input_dir}"
    trap - EXIT
    echo "==> Outer Shell socket bridge Linux matrix is ready"
}

case "${1:-}" in
    "")
        build_matrix
        ;;
    --build-variant)
        if [[ "$#" -ne 2 ]]; then
            echo "usage: $0 --build-variant {glibc|musl}" >&2
            exit 1
        fi
        build_variant "$2"
        ;;
    *)
        echo "usage: $0 [--build-variant {glibc|musl}]" >&2
        exit 1
        ;;
esac
