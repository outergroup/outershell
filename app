#!/bin/bash
# Build and deploy Outer Shell directly to a target from target.env.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${ROOT}"

BUILD_DIR="${ROOT}/build/app-deploy"
RELEASE_DIR="${BUILD_DIR}/release"
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
    [[ "$(uname -s)" == Darwin ]] || {
        echo "error: Outer Shell frontend and macOS agent must be built on macOS" >&2
        exit 1
    }
    echo "==> Building Outer Shell macOS and frontend resources"
    "${ROOT}/build_run.sh"
}

build_linux_target() {
    require_tool docker
    local image="alpine:3.20" platform
    [[ "${TARGET_ARCH}" == aarch64 ]] && platform=linux/arm64 || platform=linux/amd64
    echo "==> Building Outer Shell for Linux/${TARGET_ARCH} with musl"
    docker run --rm --platform "${platform}" \
        -v "${ROOT}:/work" \
        -w /work \
        "${image}" \
        sh -lc 'apk add --no-cache bash build-base openssl-dev openssl-libs-static zlib-dev zlib-static wget && ln -sf /lib/libz.a /usr/lib/libz.a && OUTER_SHELL_LINUX_LIBC=musl bash ./Scripts/build_linux_resources.sh'
}

cmd_build() {
    probe_target
    cmd_build_frontend
    if [[ "${TARGET_OS}" == Linux ]]; then
        build_linux_target
    fi

    local variant public_base_url
    variant="$(package_variant)"
    public_base_url="${OUTER_TARGET_PUBLIC_BASE_URL:-https://outershell.org/outer-shell}"
    echo "==> Packaging ${variant}"
    OUTPUT_ROOT="${RELEASE_DIR}" \
    PUBLIC_BASE_URL="${public_base_url}" \
    OUTER_SHELL_PACKAGE_VARIANTS="${variant}" \
    OUTER_SHELL_CODESIGN_IDENTITY="${OUTER_SHELL_CODESIGN_IDENTITY:--}" \
        "${ROOT}/Scripts/package_release.sh"
    echo "==> Deployable archive: ${RELEASE_DIR}/latest/$(archive_name)"
}

cmd_deploy() {
    cmd_build
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
    install_command="OUTERSHELL_INSTALL_ARCHIVE=\"\$HOME/${remote_dir}/${archive}\" sh \"\$HOME/${remote_dir}/install.sh\" install"
    if [[ -t 0 ]]; then
        run_ssh_interactive "${install_command}"
    else
        run_ssh "${install_command}"
    fi
    echo "Deployed Outer Shell."
}

cmd_push_frontend() {
    probe_target
    cmd_build_frontend
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
    echo "Pushed Outer Shell frontend bundles. Reload Outer Shell in Outer Loop."
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
