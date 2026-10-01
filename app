#!/bin/bash
# Build and deploy Outer Shell directly to a target from target.env.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${ROOT}"

BUILD_DIR="${ROOT}/build/app-deploy"
RELEASE_DIR="${BUILD_DIR}/release"
SYMBOLS_DIR="${BUILD_DIR}/symbols"
BUILD_CONFIGURATION="${CONFIGURATION:-Release}"
TARGET_KIND=ssh
SSH_BASE=()
TARGET_OS=""
TARGET_ARCH=""
TARGET_LIBC=""
TARGET_UID=""

require_tool() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "error: required tool '$1' was not found" >&2
        exit 1
    }
}

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

prepare_linux_source_archives() {
    local curl_version="${OUTER_SHELL_CURL_VERSION:-8.20.0}"
    local curl_url="${OUTER_SHELL_CURL_SOURCE_URL:-https://curl.se/download/curl-${curl_version}.tar.gz}"
    local deps_root="${ROOT}/build/linux-deps-musl/${TARGET_ARCH}"
    local curl_prefix="${deps_root}/curl-${curl_version}-install"
    local curl_archive="${deps_root}/curl-${curl_version}.tar.gz"
    local curl_download="${curl_archive}.download"

    if [[ -f "${curl_prefix}/lib/libcurl.a" && -f "${curl_prefix}/include/curl/curl.h" ]]; then
        return
    fi
    if [[ ! -f "${curl_archive}" ]]; then
        require_tool curl
        mkdir -p "${deps_root}"
        echo "==> Downloading curl ${curl_version}"
        curl --fail --location --silent --show-error \
            --output "${curl_download}" "${curl_url}"
        mv "${curl_download}" "${curl_archive}"
    fi
}

ensure_internal_container_network() {
    local network="$1"
    if ! container network list --quiet | grep -Fxq "${network}"; then
        container network create --internal "${network}" >/dev/null
    fi
}

shell_quote() {
    local value="$1"
    printf "'%s'" "${value//\'/\'\\\'\'}"
}

load_target() {
    if [[ ! -f "${ROOT}/target.env" ]]; then
        echo "error: no deploy target configured" >&2
        echo "  ./app target \"ssh -p 22 you@server\"" >&2
        exit 1
    fi
    # shellcheck source=/dev/null
    source "${ROOT}/target.env"
    TARGET_KIND="${OUTER_TARGET_KIND:-ssh}"
    if [[ "${TARGET_KIND}" != ssh ]]; then
        echo "error: Outer Shell deployment currently requires an SSH target" >&2
        exit 1
    fi
    if [[ "$(declare -p OUTER_TARGET_SSH 2>/dev/null)" != declare\ -a* ]] ||
       [[ "${#OUTER_TARGET_SSH[@]}" -eq 0 ]]; then
        echo "error: target.env must define OUTER_TARGET_SSH as a non-empty array" >&2
        exit 1
    fi
    if [[ "$(basename "${OUTER_TARGET_SSH[0]}")" == ssh ]]; then
        mkdir -p "${HOME}/.ssh"
        SSH_BASE=(
            "${OUTER_TARGET_SSH[0]}"
            -o ControlMaster=auto
            -o "ControlPath=${HOME}/.ssh/app-%C"
            -o ControlPersist=2m
            "${OUTER_TARGET_SSH[@]:1}"
        )
    else
        SSH_BASE=("${OUTER_TARGET_SSH[@]}")
    fi
}

run_ssh() {
    "${SSH_BASE[@]}" "$@"
}

run_ssh_interactive() {
    if [[ "$(basename "${SSH_BASE[0]}")" == ssh ]]; then
        "${SSH_BASE[0]}" -t "${SSH_BASE[@]:1}" "$@"
    else
        "${SSH_BASE[@]}" -t "$@"
    fi
}

probe_target() {
    load_target
    TARGET_OS="$(run_ssh uname -s </dev/null)"
    TARGET_ARCH="$(run_ssh uname -m </dev/null)"
    TARGET_UID="$(run_ssh id -u </dev/null)"
    case "${TARGET_ARCH}" in
        aarch64|arm64) TARGET_ARCH=aarch64 ;;
        x86_64|amd64) TARGET_ARCH=x86_64 ;;
        *) echo "error: unsupported target architecture '${TARGET_ARCH}'" >&2; exit 1 ;;
    esac
    case "${TARGET_OS}" in
        Darwin) TARGET_LIBC=darwin ;;
        Linux)
            if run_ssh 'test -e "/lib/ld-musl-$(uname -m).so.1" || (command -v ldd >/dev/null 2>&1 && ldd --version 2>&1 | grep -qi musl)' </dev/null; then
                TARGET_LIBC=musl
            else
                TARGET_LIBC=glibc
            fi
            ;;
        *) echo "error: unsupported target operating system '${TARGET_OS}'" >&2; exit 1 ;;
    esac
}

package_variant() {
    if [[ "${TARGET_OS}" == Darwin ]]; then
        [[ "${TARGET_ARCH}" == aarch64 ]] && printf 'macos-arm64\n' || printf 'macos-x86_64\n'
    else
        printf 'linux-%s-musl\n' "${TARGET_ARCH}"
    fi
}

archive_name() {
    local variant
    variant="$(package_variant)"
    if [[ "${variant}" == macos-* ]]; then
        printf 'outer-shell-%s.zip\n' "${variant}"
    else
        printf 'outer-shell-%s.tar.gz\n' "${variant}"
    fi
}

cmd_build_frontend() {
    local clean_build="${1:-0}"
    [[ "$(uname -s)" == Darwin ]] || {
        echo "error: Outer Shell frontend and macOS agent must be built on macOS" >&2
        exit 1
    }
    echo "==> Building Outer Shell macOS and frontend resources (${BUILD_CONFIGURATION})"
    CLEAN_FRONTEND_BUILD="${clean_build}" \
    CONFIGURATION="${BUILD_CONFIGURATION}" \
        "${ROOT}/build_run.sh"
}

build_linux_target() {
    require_build_container_runtime
    local image="outer-shell-linux-toolchain-${TARGET_ARCH}"
    local network="${image}-internal"
    local source_root
    local platform
    [[ "${TARGET_ARCH}" == aarch64 ]] && platform=linux/arm64 || platform=linux/amd64
    prepare_linux_source_archives
    source_root="$(mktemp -d "${TMPDIR:-/tmp}/outershell-linux-source.XXXXXX")"
    cp -R \
        "${ROOT}/Backend" \
        "${ROOT}/outershelld" \
        "${ROOT}/Resources" \
        "${ROOT}/SocketBridge" \
        "${ROOT}/Scripts" \
        "${source_root}/"
    echo "==> Building Outer Shell for Linux/${TARGET_ARCH} with musl"
    if [[ "${BUILD_CONTAINER_RUNTIME}" == container ]]; then
        container build --progress plain --platform "${platform}" \
            --file "${ROOT}/Container/OuterShellLinux/Containerfile" \
            --tag "${image}" \
            "${ROOT}/Container/OuterShellLinux"
        ensure_internal_container_network "${network}"
        container run --rm --platform "${platform}" \
            --network "${network}" \
            --mount "type=bind,source=${source_root}/Backend,target=/work/Backend,readonly" \
            --mount "type=bind,source=${source_root}/outershelld,target=/work/outershelld,readonly" \
            --mount "type=bind,source=${source_root}/Resources,target=/work/Resources,readonly" \
            --mount "type=bind,source=${source_root}/SocketBridge,target=/work/SocketBridge,readonly" \
            --mount "type=bind,source=${source_root}/Scripts,target=/work/Scripts,readonly" \
            --mount "type=bind,source=${ROOT}/build,target=/work/build" \
            --workdir /work \
            "${image}" \
            bash -lc 'OUTER_SHELL_LINUX_LIBC=musl ./Scripts/build_linux_resources.sh'
    else
        docker build --platform "${platform}" \
            --file "${ROOT}/Container/OuterShellLinux/Containerfile" \
            --tag "${image}" \
            "${ROOT}/Container/OuterShellLinux"
        docker run --rm --platform "${platform}" \
            --network none \
            --mount "type=bind,source=${source_root}/Backend,target=/work/Backend,readonly" \
            --mount "type=bind,source=${source_root}/outershelld,target=/work/outershelld,readonly" \
            --mount "type=bind,source=${source_root}/Resources,target=/work/Resources,readonly" \
            --mount "type=bind,source=${source_root}/SocketBridge,target=/work/SocketBridge,readonly" \
            --mount "type=bind,source=${source_root}/Scripts,target=/work/Scripts,readonly" \
            --mount "type=bind,source=${ROOT}/build,target=/work/build" \
            --workdir /work \
            "${image}" \
            bash -lc 'OUTER_SHELL_LINUX_LIBC=musl ./Scripts/build_linux_resources.sh'
    fi
    rm -rf "${source_root}"
}

index_frontend_symbols() {
    local dsym="$1"
    local uuid compact_uuid map_directory
    while read -r uuid; do
        compact_uuid="${uuid//-/}"
        map_directory="${SYMBOLS_DIR}/uuid-map/${compact_uuid:0:4}/${compact_uuid:4:4}/${compact_uuid:8:4}/${compact_uuid:12:4}/${compact_uuid:16:4}"
        mkdir -p "${map_directory}"
        ln -sfn "${dsym}/Contents/Resources/DWARF/Outer Shell" "${map_directory}/${compact_uuid:20:12}"
    done < <(dwarfdump --uuid "${dsym}" | awk '{ print $2 }')
}

archive_frontend_symbols() {
    local source_dsym="${ROOT}/build/macos/${BUILD_CONFIGURATION}/Outer Shell.bundle.dSYM"
    local arm64_uuid
    local destination_dsym

    arm64_uuid="$(dwarfdump --uuid "${source_dsym}" | awk '$3 == "(arm64)" { print $2; exit }')"
    if [[ -z "${arm64_uuid}" ]]; then
        echo "error: frontend dSYM does not contain an arm64 UUID" >&2
        exit 1
    fi

    destination_dsym="${SYMBOLS_DIR}/${arm64_uuid}/Outer Shell.bundle.dSYM"
    mkdir -p "$(dirname "${destination_dsym}")"
    ditto "${source_dsym}" "${destination_dsym}"
    index_frontend_symbols "${destination_dsym}"
    FRONTEND_SYMBOLS_PATH="${destination_dsym}"
}

cmd_build() {
    local clean_frontend="${1:-0}"
    probe_target
    cmd_build_frontend "${clean_frontend}"
    if [[ "${TARGET_OS}" == Linux ]]; then
        build_linux_target
    fi

    local variant public_base_url
    variant="$(package_variant)"
    public_base_url="${OUTER_TARGET_PUBLIC_BASE_URL:-https://outershell.org/outer-shell}"
    echo "==> Packaging ${variant}"
    OUTPUT_ROOT="${RELEASE_DIR}" \
    PUBLIC_BASE_URL="${public_base_url}" \
    MACOS_BUILD_ROOT="${ROOT}/build/macos/${BUILD_CONFIGURATION}" \
    OUTER_SHELL_PACKAGE_VARIANTS="${variant}" \
    OUTER_SHELL_CODESIGN_IDENTITY="${OUTER_SHELL_CODESIGN_IDENTITY:--}" \
        "${ROOT}/Scripts/package_release.sh"
    archive_frontend_symbols
    echo "==> Deployable archive: ${RELEASE_DIR}/latest/$(archive_name)"
    echo "==> Frontend profiling symbols: ${FRONTEND_SYMBOLS_PATH}"
}

cmd_deploy() {
    cmd_build 1
    local archive remote_dir install_command
    archive="$(archive_name)"
    remote_dir=".cache/outershell-deploy"
    echo "==> Uploading Outer Shell installer and ${archive}"
    COPYFILE_DISABLE=1 tar czf - \
        -C "${RELEASE_DIR}/latest" install.sh "${archive}" | run_ssh "
            set -e
            rm -rf \"\$HOME/${remote_dir}\"
            mkdir -p \"\$HOME/${remote_dir}\"
            tar xzf - -C \"\$HOME/${remote_dir}\"
        "
    echo "==> Installing Outer Shell"
    install_command="OUTERSHELL_INSTALL_ARCHIVE=\"\$HOME/${remote_dir}/${archive}\" OUTERSHELL_SKIP_SHARED_ROOT_REFRESH=1 sh \"\$HOME/${remote_dir}/install.sh\" install"
    if [[ -t 0 ]]; then
        run_ssh_interactive "${install_command}"
    else
        run_ssh "${install_command}"
    fi
    echo "Deployed Outer Shell."
    echo "Frontend profiling symbols: ${FRONTEND_SYMBOLS_PATH}"
    dwarfdump --uuid "${FRONTEND_SYMBOLS_PATH}"
}

cmd_push_frontend() {
    probe_target
    cmd_build_frontend
    archive_frontend_symbols
    local remote_dir
    if [[ "${TARGET_OS}" == Darwin ]]; then
        remote_dir="${HOME}/Library/Application Support/outershell/apps/org.outershell.OuterShell/Outer Shell.app/Contents/Resources/bundles"
        # Use the target's home, not the build host's home.
        remote_dir='${HOME}/Library/Application Support/outershell/apps/org.outershell.OuterShell/Outer Shell.app/Contents/Resources/bundles'
    elif [[ "${TARGET_UID}" == 0 ]]; then
        remote_dir="/var/lib/outershell/outer-shell/bundles"
    else
        remote_dir='${XDG_STATE_HOME:-$HOME/.local/state}/outershell/outer-shell/bundles'
    fi
    COPYFILE_DISABLE=1 tar czf - -C "${ROOT}/build/run/bundles" \
        OuterShell.bundle.macos-arm.aar OuterShell.bundle.macos-x86.aar | run_ssh "
            set -e
            remote_dir=\"${remote_dir}\"
            mkdir -p \"\$remote_dir\"
            tar xzf - -C \"\$remote_dir\"
        "
    echo "Pushed Outer Shell frontend bundles (${BUILD_CONFIGURATION}). Reload Outer Shell in Outer Loop."
    echo "Frontend profiling symbols: ${FRONTEND_SYMBOLS_PATH}"
    dwarfdump --uuid "${FRONTEND_SYMBOLS_PATH}"
}

cmd_uninstall() {
    cmd_build
    run_ssh "sh -s -- uninstall" < "${RELEASE_DIR}/latest/install.sh"
}

cmd_target() {
    if [[ $# -ne 1 ]]; then
        echo "usage: ./app target \"ssh -p 22 you@server\"" >&2
        exit 1
    fi
    read -r -a words <<< "$1"
    {
        printf 'OUTER_TARGET_KIND=ssh\nOUTER_TARGET_SSH=(\n'
        local word
        for word in "${words[@]}"; do
            printf '    '
            shell_quote "${word}"
            printf '\n'
        done
        printf ')\nOUTER_TARGET_PUBLIC_BASE_URL=https://outershell.org/outer-shell\n'
    } > "${ROOT}/target.env"
    echo "Wrote target.env"
}

cmd_status() {
    probe_target
    if [[ "${TARGET_OS}" == Darwin ]]; then
        run_ssh "launchctl print gui/\$(id -u)/org.outershell.OuterShell" || true
    elif [[ "${TARGET_UID}" == 0 ]]; then
        run_ssh "systemctl --system status org.outershell.OuterShell.socket --no-pager" || true
    else
        run_ssh "systemctl --user status org.outershell.OuterShell.socket --no-pager" || true
    fi
}

cmd_logs() {
    probe_target
    if [[ "${TARGET_OS}" == Darwin ]]; then
        run_ssh_interactive 'tail -f "$HOME/Library/Logs/org.outershell.OuterShell/output.log"'
    elif [[ "${TARGET_UID}" == 0 ]]; then
        run_ssh_interactive 'tail -f /var/log/outershell/org.outershell.OuterShell.log'
    else
        run_ssh_interactive 'tail -f "${XDG_STATE_HOME:-$HOME/.local/state}/outershell/outer-shell/logs/OuterShellBackend.log"'
    fi
}

cmd_clean() {
    rm -rf "${BUILD_DIR}"
}

cmd_help() {
    cat <<'EOF'
Outer Shell development tasks

  ./app target "ssh ..."  write target.env
  ./app build             build and package for target.env
  ./app deploy            build, upload, and install Outer Shell
  ./app push-frontend     update only the target frontend bundles
  ./app status            show target service status
  ./app logs              follow the target log
  ./app ssh [command]     open a shell or run a target command
  ./app uninstall         uninstall Outer Shell from the target
  ./app clean             remove direct-deploy build products

Builds auto-select Apple container on macOS and Docker elsewhere.
Set OUTER_BUILD_CONTAINER_RUNTIME=container|docker to override.
EOF
}

command="${1:-help}"
shift $(( $# > 0 ? 1 : 0 ))
case "${command}" in
    target) cmd_target "$@" ;;
    build) cmd_build ;;
    build-frontend) cmd_build_frontend ;;
    deploy) cmd_deploy ;;
    push-frontend) cmd_push_frontend ;;
    status) cmd_status ;;
    logs) cmd_logs ;;
    ssh) load_target; run_ssh "$@" ;;
    uninstall) cmd_uninstall ;;
    clean) cmd_clean ;;
    help|--help|-h) cmd_help ;;
    *) echo "error: unknown command '${command}'" >&2; cmd_help >&2; exit 1 ;;
esac
