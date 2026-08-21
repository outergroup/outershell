#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_ROOT="${BUILD_ROOT:-${SCRIPT_DIR}/build/macos}"
RUN_ROOT="${RUN_ROOT:-${SCRIPT_DIR}/build/run}"
CONFIGURATION="${CONFIGURATION:-Release}"
CLEAN_FRONTEND_BUILD="${CLEAN_FRONTEND_BUILD:-0}"

require_tool() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "error: required tool '$1' was not found on PATH" >&2
        exit 1
    fi
}

require_tool /usr/bin/xcodebuild
require_tool aa
require_tool dwarfdump
require_tool lipo

frontend_symbols_are_usable() {
    local dsym_path="$1"
    local dsym_statistics

    [[ -d "${dsym_path}" ]] || return 1
    dsym_statistics="$(dwarfdump --statistics "${dsym_path}" 2>/dev/null)" || return 1
    [[ "${dsym_statistics}" == *'"#functions":'* ]] &&
        [[ "${dsym_statistics}" != *'"#functions": 0'* ]] &&
        [[ "${dsym_statistics}" == *'"#bytes with line information":'* ]] &&
        [[ "${dsym_statistics}" != *'"#bytes with line information": 0'* ]]
}

frontend_symbols_match_bundle() {
    local bundle_binary="$1"
    local dsym_path="$2"
    local bundle_uuids
    local dsym_uuids

    bundle_uuids="$(dwarfdump --uuid "${bundle_binary}" | awk '{ print $2, $3 }' | LC_ALL=C sort)"
    dsym_uuids="$(dwarfdump --uuid "${dsym_path}" | awk '{ print $2, $3 }' | LC_ALL=C sort)"
    [[ -n "${bundle_uuids}" && "${bundle_uuids}" == "${dsym_uuids}" ]]
}

validate_frontend_symbols() {
    local bundle_path="$1"
    local dsym_path="$2"
    local bundle_binary="${bundle_path}/Contents/MacOS/Outer Shell"
    local bundle_uuids
    local dsym_uuids

    if [[ ! -f "${bundle_binary}" ]]; then
        echo "error: frontend executable not found at ${bundle_binary}" >&2
        exit 1
    fi
    if [[ ! -d "${dsym_path}" ]]; then
        echo "error: frontend dSYM not found at ${dsym_path}" >&2
        exit 1
    fi

    if ! frontend_symbols_are_usable "${dsym_path}"; then
        echo "error: frontend dSYM has no usable function or source-line information" >&2
        echo "       ${dsym_path}" >&2
        echo "       Run a clean frontend build before packaging." >&2
        exit 1
    fi

    if ! frontend_symbols_match_bundle "${bundle_binary}" "${dsym_path}"; then
        bundle_uuids="$(dwarfdump --uuid "${bundle_binary}" | awk '{ print $2, $3 }' | LC_ALL=C sort)"
        dsym_uuids="$(dwarfdump --uuid "${dsym_path}" | awk '{ print $2, $3 }' | LC_ALL=C sort)"
        echo "error: frontend executable and dSYM UUIDs do not match" >&2
        echo "Executable UUIDs:" >&2
        printf '%s\n' "${bundle_uuids}" >&2
        echo "dSYM UUIDs:" >&2
        printf '%s\n' "${dsym_uuids}" >&2
        exit 1
    fi
}

build_frontend() {
    /usr/bin/xcodebuild \
        -project "${SCRIPT_DIR}/outershell.xcodeproj" \
        -scheme "Outer Shell" \
        -configuration "${CONFIGURATION}" \
        SYMROOT="${BUILD_ROOT}" \
        ARCHS="arm64 x86_64" \
        ONLY_ACTIVE_ARCH=NO \
        CODE_SIGNING_ALLOWED=NO \
        CODE_SIGNING_REQUIRED=NO \
        "$@" \
        build
}

rm -rf "${RUN_ROOT}"
mkdir -p \
    "${BUILD_ROOT}" \
    "${RUN_ROOT}/bundles"

echo "==> Building Outer Shell.bundle"
frontend_bundle_path="${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.bundle"
frontend_bundle_binary="${frontend_bundle_path}/Contents/MacOS/Outer Shell"
frontend_dsym_path="${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.bundle.dSYM"
frontend_object_root=""
frontend_dsym_backup_root=""
frontend_dsym_backup=""
if [[ "${CLEAN_FRONTEND_BUILD}" == 1 ]]; then
    frontend_object_root="$(mktemp -d "${TMPDIR:-/tmp}/outershell-frontend-build.XXXXXX")"
    trap 'rm -rf "${frontend_object_root}"' EXIT
    build_frontend "OBJROOT=${frontend_object_root}"
else
    if frontend_symbols_are_usable "${frontend_dsym_path}" &&
       frontend_symbols_match_bundle "${frontend_bundle_binary}" "${frontend_dsym_path}"; then
        frontend_dsym_backup_root="$(mktemp -d "${TMPDIR:-/tmp}/outershell-symbol-backup.XXXXXX")"
        frontend_dsym_backup="${frontend_dsym_backup_root}/Outer Shell.bundle.dSYM"
        trap 'rm -rf "${frontend_dsym_backup_root}"' EXIT
        ditto "${frontend_dsym_path}" "${frontend_dsym_backup}"
    fi
    build_frontend
    if ! frontend_symbols_are_usable "${frontend_dsym_path}" &&
       [[ -n "${frontend_dsym_backup}" ]] &&
       frontend_symbols_match_bundle "${frontend_bundle_binary}" "${frontend_dsym_backup}"; then
        echo "==> Restoring matching frontend dSYM after unchanged incremental build"
        ditto "${frontend_dsym_backup}" "${frontend_dsym_path}"
    fi
fi

validate_frontend_symbols \
    "${frontend_bundle_path}" \
    "${frontend_dsym_path}"
if [[ -n "${frontend_object_root}" ]]; then
    rm -rf "${frontend_object_root}"
    frontend_object_root=""
fi
if [[ -n "${frontend_dsym_backup_root}" ]]; then
    rm -rf "${frontend_dsym_backup_root}"
    frontend_dsym_backup_root=""
fi
trap - EXIT

echo "==> Building outershelld"
/usr/bin/xcodebuild \
    -project "${SCRIPT_DIR}/outershell.xcodeproj" \
    -target outershelld \
    -configuration "${CONFIGURATION}" \
    SYMROOT="${BUILD_ROOT}" \
    ARCHS="arm64 x86_64" \
    ONLY_ACTIVE_ARCH=NO \
    build

echo "==> Building Outer Shell agent"
/usr/bin/xcodebuild \
    -project "${SCRIPT_DIR}/outershell.xcodeproj" \
    -target OuterShellAgent \
    -configuration "${CONFIGURATION}" \
    SYMROOT="${BUILD_ROOT}" \
    ONLY_ACTIVE_ARCH=YES \
    build

echo "==> Archiving OuterShell bundles"
"${SCRIPT_DIR}/Scripts/archive_outershell_bundle.sh" \
    "${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.bundle" \
    "${RUN_ROOT}/bundles" \
    OuterShell.bundle
rm -rf "${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app/Contents/Resources/bundles"
mkdir -p "${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app/Contents/Resources/bundles"
cp "${RUN_ROOT}/bundles"/OuterShell.bundle.*.aar \
    "${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app/Contents/Resources/bundles/"
cp "${SCRIPT_DIR}/app-icon.png" \
    "${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app/Contents/Resources/app-icon.png"
cp "${SCRIPT_DIR}/OuterShell.icns" \
    "${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app/Contents/Resources/OuterShell.icns"
rm -rf "${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app/Contents/Resources/web"
cp -R "${SCRIPT_DIR}/Resources/OuterShellWeb" \
    "${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app/Contents/Resources/web"
bootstrap_resource="${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app/Contents/Resources/container-bootstrap"
bootstrap_run_resource="${RUN_ROOT}/container-bootstrap"
rm -rf "${bootstrap_resource}" "${bootstrap_run_resource}"
for libc in glibc musl; do
    if [[ "${libc}" == glibc ]]; then
        linux_root="${SCRIPT_DIR}/build/linux-package/RemoteLinuxBinaries"
    else
        linux_root="${SCRIPT_DIR}/build/linux-package/RemoteLinuxBinariesMusl"
    fi
    for architecture in aarch64 x86_64; do
        for tool in outershelld outerctl; do
            if [[ ! -x "${linux_root}/${architecture}/${tool}" ]]; then
                echo "Missing container bootstrap tool: ${linux_root}/${architecture}/${tool}" >&2
                exit 1
            fi
        done
        socket_bridge="${SCRIPT_DIR}/../outerloop/OuterLoop/Resources/LinuxHelpers/outer-socket-bridge-linux-${architecture}-${libc}"
        if [[ ! -x "${socket_bridge}" ]]; then
            echo "Missing container socket bridge: ${socket_bridge}" >&2
            exit 1
        fi
        destination="${bootstrap_resource}/bin/${libc}/${architecture}"
        run_destination="${bootstrap_run_resource}/bin/${libc}/${architecture}"
        mkdir -p "${destination}" "${run_destination}"
        cp "${linux_root}/${architecture}/outershelld" "${linux_root}/${architecture}/outerctl" \
            "${destination}/"
        cp "${linux_root}/${architecture}/outershelld" "${linux_root}/${architecture}/outerctl" \
            "${run_destination}/"
        cp "${socket_bridge}" "${destination}/outer-socket-bridge"
        cp "${socket_bridge}" "${run_destination}/outer-socket-bridge"
    done
done
rm -rf "${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app/Contents/Resources/bundled-apps"
agentdiy_payload="${AGENTDIY_PAYLOAD_DIR:-${SCRIPT_DIR}/../AgentDIY/build/payload}"
if [[ -x "${agentdiy_payload}/AgentDIYBackend" ]]; then
    agentdiy_file_info="$(/usr/bin/file "${agentdiy_payload}/AgentDIYBackend")"
    if [[ "${agentdiy_file_info}" == *"aarch64"* || "${agentdiy_file_info}" == *"arm64"* ]]; then
        agentdiy_platform="linux-aarch64"
    elif [[ "${agentdiy_file_info}" == *"x86-64"* || "${agentdiy_file_info}" == *"x86_64"* ]]; then
        agentdiy_platform="linux-x86_64"
    else
        echo "Unsupported Container Agent backend architecture: ${agentdiy_file_info}" >&2
        exit 1
    fi
    agentdiy_resource="${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app/Contents/Resources/bundled-apps/AgentDIY/${agentdiy_platform}"
    mkdir -p "${agentdiy_resource}"
    cp -R "${agentdiy_payload}/." "${agentdiy_resource}/"
else
    echo "Container Agent payload is missing; build Container Agent before Outer Shell." >&2
    exit 1
fi
/usr/bin/codesign --force --sign - \
    --entitlements "${SCRIPT_DIR}/Agent/OuterShellAgent.entitlements" \
    "${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app"

echo "Built:"
echo "  ${BUILD_ROOT}/${CONFIGURATION}/outershelld"
echo "  ${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app"
echo "  ${RUN_ROOT}/bundles"
echo
echo "Run:"
echo "  API_SOCKET=\"$(getconf DARWIN_USER_TEMP_DIR)outershelld-api\""
echo "  \"${BUILD_ROOT}/${CONFIGURATION}/outershelld\" --api-socket-path \"\$API_SOCKET\" &"
echo "  clang -std=gnu17 -DOUTER_SHELL_BACKEND_STANDALONE=1 Backend/OuterShellBuffer.c Backend/OuterShellAPI.c Backend/OuterShellPlatform.c Backend/OuterShellDownloaderApple.m Backend/OuterShellBackend.c -framework Foundation -o /tmp/OuterShellBackend && /tmp/OuterShellBackend --port 7354 --api-socket-path \"\$API_SOCKET\" --bundles-dir \"${RUN_ROOT}/bundles\" --web-root \"${SCRIPT_DIR}/Resources/OuterShellWeb\" --native-app-template-dir \"${SCRIPT_DIR}/Resources/NativeAppTemplate\""
echo "  \"${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app/Contents/MacOS/Outer Shell\" --socket-path \"$(getconf DARWIN_USER_TEMP_DIR)org.outershell.OuterShell\" --app-base-url \"https://outershell.org/outer-shell/apps\""
