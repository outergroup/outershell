#!/bin/bash

set -euo pipefail
export COPYFILE_DISABLE=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTPUT_ROOT="${OUTPUT_ROOT:-${REPO_ROOT}/build/release/outer-shell}"
RUN_ROOT="${RUN_ROOT:-${REPO_ROOT}/build/run}"
PACKAGE_ROOT="${PACKAGE_ROOT:-${REPO_ROOT}/build/linux-package}"
MACOS_BUILD_ROOT="${MACOS_BUILD_ROOT:-${REPO_ROOT}/build/macos/Release}"
PUBLIC_BASE_URL="${PUBLIC_BASE_URL:-}"
APP_CATALOG_PATH="${APP_CATALOG_PATH:-}"
OUTER_SHELL_VERSION="${OUTER_SHELL_VERSION:-0.0.0.DEV}"
OUTER_SHELL_CODESIGN_IDENTITY="${OUTER_SHELL_CODESIGN_IDENTITY:-${CODE_SIGN_IDENTITY:--}}"
OUTER_SHELL_NOTARY_PROFILE="${OUTER_SHELL_NOTARY_PROFILE:-}"

if [[ -z "${PUBLIC_BASE_URL}" ]]; then
    echo "error: set PUBLIC_BASE_URL to the public Outer Shell asset base URL" >&2
    exit 1
fi
PUBLIC_BASE_URL="${PUBLIC_BASE_URL%/}"

require_file() {
    if [[ ! -f "$1" ]]; then
        echo "error: missing $1" >&2
        exit 1
    fi
}

require_file "${PACKAGE_ROOT}/RemoteLinuxBinaries/aarch64/outershelld"
require_file "${PACKAGE_ROOT}/RemoteLinuxBinaries/x86_64/outershelld"
require_file "${PACKAGE_ROOT}/RemoteLinuxBinaries/aarch64/OuterShellBackend"
require_file "${PACKAGE_ROOT}/RemoteLinuxBinaries/x86_64/OuterShellBackend"
require_file "${PACKAGE_ROOT}/RemoteLinuxBinaries/aarch64/outerctl"
require_file "${PACKAGE_ROOT}/RemoteLinuxBinaries/x86_64/outerctl"
require_file "${RUN_ROOT}/bundles/OuterShell.bundle.macos-arm.aar"
require_file "${RUN_ROOT}/bundles/OuterShell.bundle.macos-x86.aar"
require_file "${MACOS_BUILD_ROOT}/Outer Shell.app/Contents/MacOS/Outer Shell"
require_file "${MACOS_BUILD_ROOT}/outershelld"
require_file "${REPO_ROOT}/app-icon.png"
if [[ -n "${APP_CATALOG_PATH}" ]]; then
    require_file "${APP_CATALOG_PATH}"
fi

rm -rf "${OUTPUT_ROOT}"
mkdir -p "${OUTPUT_ROOT}/latest"
if [[ -n "${APP_CATALOG_PATH}" ]]; then
    install -m 0644 "${APP_CATALOG_PATH}" "${OUTPUT_ROOT}/app-catalog.json"
fi

STAGING_ROOT="$(mktemp -d)"
trap 'rm -rf "${STAGING_ROOT}"' EXIT

macos_codesign_args() {
    if [[ "${OUTER_SHELL_CODESIGN_IDENTITY}" == "-" ]]; then
        printf '%s\n' --force --sign - --timestamp=none
    else
        printf '%s\n' --force --options runtime --timestamp --sign "${OUTER_SHELL_CODESIGN_IDENTITY}"
    fi
}

sign_macos_code() {
    local path="$1"
    local identifier="${2:-}"
    if ! command -v /usr/bin/codesign >/dev/null 2>&1; then
        return 0
    fi
    local args=()
    while IFS= read -r arg; do
        args+=("$arg")
    done < <(macos_codesign_args)
    if [[ -n "${identifier}" ]]; then
        args+=(--identifier "${identifier}")
    fi
    /usr/bin/codesign "${args[@]}" "$path"
}

thin_macho() {
    local input="$1"
    local arch="$2"
    local output="$3"
    /usr/bin/lipo "$input" -thin "$arch" -output "$output"
    chmod 0755 "$output"
}

notarize_macos_archive_if_requested() {
    local archive="$1"
    if [[ -z "${OUTER_SHELL_NOTARY_PROFILE}" ]]; then
        return 0
    fi
    if [[ "${OUTER_SHELL_CODESIGN_IDENTITY}" == "-" ]]; then
        echo "error: OUTER_SHELL_NOTARY_PROFILE requires OUTER_SHELL_CODESIGN_IDENTITY" >&2
        exit 1
    fi
    xcrun notarytool submit "$archive" \
        --keychain-profile "${OUTER_SHELL_NOTARY_PROFILE}" \
        --wait
}

stage_home_screen() {
    local arch="$1"
    local package_name="outer-shell-linux-${arch}"
    local root="${STAGING_ROOT}/${package_name}/OuterShell"
    local app_root="${root}/apps/org.outershell.OuterShell"
    mkdir -p "${root}/tools" "${app_root}/bundles"
    install -m 0755 "${PACKAGE_ROOT}/RemoteLinuxBinaries/${arch}/outershelld" "${root}/tools/outershelld"
    install -m 0755 "${PACKAGE_ROOT}/RemoteLinuxBinaries/${arch}/outerctl" "${root}/tools/outerctl"
    install -m 0755 "${PACKAGE_ROOT}/RemoteLinuxBinaries/${arch}/OuterShellBackend" "${app_root}/OuterShellBackend"
    install -m 0644 "${REPO_ROOT}/app-icon.png" "${app_root}/app-icon.png"
    install -m 0644 "${RUN_ROOT}/bundles/OuterShell.bundle.macos-arm.aar" "${app_root}/bundles/OuterShell.bundle.macos-arm.aar"
    install -m 0644 "${RUN_ROOT}/bundles/OuterShell.bundle.macos-x86.aar" "${app_root}/bundles/OuterShell.bundle.macos-x86.aar"
    tar --format ustar --no-xattrs -C "${STAGING_ROOT}/${package_name}" -czf "${OUTPUT_ROOT}/latest/${package_name}.tar.gz" OuterShell
}

stage_home_screen_macos() {
    local package_arch="$1"
    local macho_arch="$2"
    local package_name="outer-shell-macos-${package_arch}"
    local root="${STAGING_ROOT}/${package_name}/OuterShell"
    local app_root="${root}/apps/org.outershell.OuterShell"
    local app_bundle="${app_root}/Outer Shell.app"
    mkdir -p "${root}/tools" "${app_root}"
    ditto "${MACOS_BUILD_ROOT}/Outer Shell.app" "${app_bundle}"
    rm -rf "${app_bundle}/Contents/Resources/bundled-apps"
    rm -rf "${app_bundle}/Contents/Resources/bundles"
    mkdir -p "${app_bundle}/Contents/Resources/bundles"
    install -m 0644 "${REPO_ROOT}/app-icon.png" "${app_bundle}/Contents/Resources/app-icon.png"
    install -m 0644 "${RUN_ROOT}/bundles/OuterShell.bundle.macos-arm.aar" "${app_bundle}/Contents/Resources/bundles/OuterShell.bundle.macos-arm.aar"
    install -m 0644 "${RUN_ROOT}/bundles/OuterShell.bundle.macos-x86.aar" "${app_bundle}/Contents/Resources/bundles/OuterShell.bundle.macos-x86.aar"
    thin_macho "${MACOS_BUILD_ROOT}/Outer Shell.app/Contents/MacOS/Outer Shell" "${macho_arch}" "${app_bundle}/Contents/MacOS/Outer Shell"
    thin_macho "${MACOS_BUILD_ROOT}/outershelld" "${macho_arch}" "${root}/tools/outershelld"
    clang++ -std=c++17 -arch "${macho_arch}" "${REPO_ROOT}/Resources/outerctl.cpp" \
        -o "${root}/tools/outerctl"
    chmod 0755 "${root}/tools/outerctl"
    sign_macos_code "${root}/tools/outershelld" "org.outershell.outershelld"
    sign_macos_code "${root}/tools/outerctl" "org.outershell.outerctl"
    sign_macos_code "${app_bundle}"
    (cd "${STAGING_ROOT}/${package_name}" && ditto -c -k --norsrc --keepParent OuterShell "${OUTPUT_ROOT}/latest/${package_name}.zip")
    notarize_macos_archive_if_requested "${OUTPUT_ROOT}/latest/${package_name}.zip"
}

stage_home_screen aarch64
stage_home_screen x86_64
stage_home_screen_macos arm64 arm64
stage_home_screen_macos x86_64 x86_64

ASSET_VERSION="$(date -u +%Y%m%d%H%M%S)"
printf '%s\n' "${OUTER_SHELL_VERSION}" > "${OUTPUT_ROOT}/latest/version.txt"
cat > "${OUTPUT_ROOT}/latest/install.sh" <<'INSTALL_SH'
#!/bin/sh
set -eu

command="${1:-install}"
case "$command" in
    install|update|uninstall) ;;
    *) echo "Unsupported Outer Shell installer command: $command" >&2; exit 2 ;;
esac

machine_arch="$(uname -m)"
case "$machine_arch" in
    aarch64|arm64) arch="aarch64" ;;
    x86_64|amd64) arch="x86_64" ;;
    *) echo "Unsupported architecture: $(uname -m)" >&2; exit 1 ;;
esac

download() {
    url="$1"
    out="$2"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL -o "$out" "$url"
    elif command -v wget >/dev/null 2>&1; then
        wget -qO "$out" "$url"
    else
        echo "curl or wget is required to install Outer Shell." >&2
        exit 127
    fi
}

stage_archive() {
    url="$1"
    out="$2"
    if [ -n "${OUTERSHELL_INSTALL_ARCHIVE:-}" ]; then
        cp "$OUTERSHELL_INSTALL_ARCHIVE" "$out"
    else
        download "$url" "$out"
    fi
}

timestamp() {
    date -u '+%Y-%m-%dT%H:%M:%SZ'
}

xml_escape() {
    printf '%s' "$1" | sed \
        -e 's/&/\&amp;/g' \
        -e 's/</\&lt;/g' \
        -e 's/>/\&gt;/g' \
        -e 's/"/\&quot;/g'
}

runtime_dir_for_allowlist_scope() {
    scope="$1"
    if [ "$scope" = "system" ]; then
        if [ "$os_name" = "Darwin" ]; then
            printf '%s\n' "/var/run"
        else
            printf '%s\n' "/run"
        fi
        return
    fi
    if [ "$os_name" = "Darwin" ]; then
        getconf DARWIN_USER_TEMP_DIR | sed 's:/*$::'
    else
        printf '%s\n' "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" | sed 's:/*$::'
    fi
}

allowlist_path_for_scope() {
    scope="$1"
    if [ "$os_name" = "Darwin" ]; then
        if [ "$scope" = "system" ]; then
            printf '%s\n' "/Library/Application Support/dev.outergroup.OuterLoop/http-unix.allow"
        else
            printf '%s\n' "$HOME/Library/Application Support/dev.outergroup.OuterLoop/http-unix.allow"
        fi
    else
        if [ "$scope" = "system" ]; then
            printf '%s\n' "/etc/outerloop/http-unix.allow"
        else
            printf '%s\n' "${XDG_CONFIG_HOME:-$HOME/.config}/outerloop/http-unix.allow"
        fi
    fi
}

allowlist_entry_for_socket_path() {
    scope="$1"
    socket="$2"
    runtime_dir="$(runtime_dir_for_allowlist_scope "$scope")"
    case "$socket" in
        "$runtime_dir"/*)
            suffix="${socket#"$runtime_dir"/}"
            if [ "$scope" = "system" ]; then
                printf '%%T/%s\n' "$suffix"
            else
                printf '%%t/%s\n' "$suffix"
            fi
            ;;
        *)
            printf '%s\n' "$socket"
            ;;
    esac
}

append_outerloop_http_unix_allowlist_entry() {
    scope="$1"
    socket="$2"
    [ -n "$socket" ] || return 0
    allowlist_path="$(allowlist_path_for_scope "$scope")"
    allowlist_dir="$(dirname "$allowlist_path")"
    entry="$(allowlist_entry_for_socket_path "$scope" "$socket")"
    mkdir -p "$allowlist_dir"
    if [ "$scope" = "system" ]; then
        chmod 0755 "$allowlist_dir"
    else
        chmod 0700 "$allowlist_dir"
    fi
    if [ -L "$allowlist_path" ] || { [ -e "$allowlist_path" ] && [ ! -f "$allowlist_path" ]; }; then
        echo "$allowlist_path is not a regular file" >&2
        exit 1
    fi
    touch "$allowlist_path"
    if [ "$os_name" = "Darwin" ]; then
        owner_uid="$(stat -f %u "$allowlist_path")"
    else
        owner_uid="$(stat -c %u "$allowlist_path")"
    fi
    if [ "$scope" = "system" ]; then
        expected_uid=0
    else
        expected_uid="$(id -u)"
    fi
    if [ "$owner_uid" != "$expected_uid" ]; then
        echo "$allowlist_path is owned by uid $owner_uid, expected uid $expected_uid" >&2
        exit 1
    fi
    chmod 0644 "$allowlist_path"
    if ! grep -Fx -- "$entry" "$allowlist_path" >/dev/null 2>&1; then
        printf '%s\n' "$entry" >> "$allowlist_path"
    fi
}

systemd_quote_arg() {
    printf '"'
    printf '%s' "$1" | sed \
        -e 's/\\/\\\\/g' \
        -e 's/"/\\"/g' \
        -e 's/%/%%/g'
    printf '"'
}

os_name="$(uname -s)"
public_base_url="__PUBLIC_BASE_URL__"
app_base_url="${public_base_url%/}/apps"

if [ "$os_name" = "Darwin" ]; then
    outershell_home="${OUTERSHELL_HOME:-$HOME/Library/Application Support/outershell}"
    legacy_install_root="$outershell_home/outer-shell"
    app_install_root="$outershell_home/apps/org.outershell.OuterShell"
    tools_install_root="$outershell_home/outershelld"
    outershelld_path="$tools_install_root/outershelld"
    outerctl_path="$outershell_home/bin/outerctl"
    outer_shell_cache_root="$HOME/Library/Caches/outershell/outer-shell"
    outershell_cache_root="$HOME/Library/Caches/outershell"
    launch_agent_dir="$HOME/Library/LaunchAgents"
    plist_path="$launch_agent_dir/org.outershell.OuterShell.plist"
    socket_path="$(getconf DARWIN_USER_TEMP_DIR)org.outershell.OuterShell"
    api_socket_path="$(getconf DARWIN_USER_TEMP_DIR)outershelld-api"
    log_dir="$HOME/Library/Logs/org.outershell.OuterShell"
    log_path="$log_dir/output.log"
    service_id="org.outershell.OuterShell"
    display_name="Outer Shell"
    case "$machine_arch" in
        arm64|aarch64) package_arch="macos-arm64" ;;
        x86_64|amd64) package_arch="macos-x86_64" ;;
        *) echo "Unsupported macOS architecture: $machine_arch" >&2; exit 1 ;;
    esac

    unload_outer_shell() {
        launchctl bootout "gui/$(id -u)/$service_id" >/dev/null 2>&1 || launchctl remove "$service_id" >/dev/null 2>&1 || true
        attempts=60
        while [ "$attempts" -gt 0 ]; do
            if ! launchctl print "gui/$(id -u)/$service_id" >/dev/null 2>&1; then
                return 0
            fi
            sleep 0.25
            attempts=$((attempts - 1))
        done
        return 0
    }

    bootstrap_outer_shell() {
        attempts=60
        last_error=""
        while [ "$attempts" -gt 0 ]; do
            if last_error="$(launchctl bootstrap "gui/$(id -u)" "$plist_path" 2>&1)"; then
                return 0
            fi
            unload_outer_shell
            rm -f "$socket_path" "$api_socket_path"
            sleep 0.25
            attempts=$((attempts - 1))
        done
        if [ -n "$last_error" ]; then
            printf '%s\n' "$last_error" >&2
        fi
        return 1
    }

    if [ "$command" = "uninstall" ]; then
        unload_outer_shell
        rm -f "$plist_path" "$socket_path" "$api_socket_path"
        if [ -x "$outerctl_path" ]; then
            OUTERSHELL_HOME="$outershell_home" "$outerctl_path" app clear --backend "$service_id" >/dev/null 2>&1 || true
            OUTERSHELL_HOME="$outershell_home" "$outerctl_path" log clear --backend "$service_id" >/dev/null 2>&1 || true
            OUTERSHELL_HOME="$outershell_home" "$outerctl_path" launchd clear --backend "$service_id" >/dev/null 2>&1 || true
            OUTERSHELL_HOME="$outershell_home" "$outerctl_path" backend remove --backend "$service_id" >/dev/null 2>&1 || true
        fi
        rm -rf "$app_install_root" "$legacy_install_root"
        if [ "${OUTERSHELL_UNINSTALL_REMOVE_USER_STATE:-0}" = "1" ]; then
            rm -f "$outershell_home/registry.orwa" "$outershell_home/registry.orwa.lock" "$outerctl_path"
            rm -rf "$tools_install_root"
            rmdir "$outershell_home/apps" "$outershell_home/bin" "$outershell_home" >/dev/null 2>&1 || true
            rm -rf "$outer_shell_cache_root"
            system_root="/Library/Application Support/outershell"
            cleanup_root_script="$(mktemp)"
            cat > "$cleanup_root_script" <<EOF
#!/bin/sh
set -eu
root='$system_root'
apps="\$root/apps"
if [ ! -d "\$apps" ] || ! find "\$apps" -mindepth 1 -print -quit 2>/dev/null | grep -q .; then
    rm -f "\$root/registry.orwa" "\$root/registry.orwa.lock"
    rm -f /usr/local/libexec/outershelld-root-tool /usr/local/libexec/outershelld-root-helper
    rm -f "\$root/bin/outerctl"
    rm -rf "\$root/outershelld"
    rmdir "\$apps" "\$root/bin" "\$root" >/dev/null 2>&1 || true
fi
EOF
            chmod 0700 "$cleanup_root_script"
            sudo -n sh "$cleanup_root_script" >/dev/null 2>&1 || true
            rm -f "$cleanup_root_script"
        else
            rm -rf "$outer_shell_cache_root/install"
        fi
        rmdir "$outer_shell_cache_root" "$outershell_cache_root" >/dev/null 2>&1 || true
        printf 'Outer Shell has been uninstalled. Outer Loop will offer to install it again the next time it connects.\n'
        exit 0
    fi

    mkdir -p "$app_install_root" "$tools_install_root" "$outershell_home/bin" "$launch_agent_dir" "$log_dir"
    archive_path="$(mktemp)"
    payload_root="$(mktemp -d)"
    stage_archive "${public_base_url%/}/latest/outer-shell-${package_arch}.zip?v=__ASSET_VERSION__" "$archive_path"
    ditto -x -k "$archive_path" "$payload_root"
    rm -f "$archive_path"
    payload="$payload_root/OuterShell"
    rm -rf "$app_install_root" "$legacy_install_root"
    mkdir -p "$app_install_root" "$tools_install_root" "$outershell_home/bin" "$log_dir"
    ditto "$payload/apps/$service_id/Outer Shell.app" "$app_install_root/Outer Shell.app"
    install -m 0755 "$payload/tools/outershelld" "$outershelld_path"
    install -m 0755 "$payload/tools/outerctl" "$outerctl_path"
    rm -rf "$payload_root"
    chmod 0755 "$app_install_root/Outer Shell.app/Contents/MacOS/Outer Shell"
    chmod 0755 "$outershelld_path"
    chmod 0755 "$outerctl_path"
    printf '%s\n' "__OUTER_SHELL_VERSION__" > "$app_install_root/version"
    printf '%s\n' "__OUTER_SHELL_VERSION__" > "$tools_install_root/version"
    touch "$log_path"

    unload_outer_shell
    rm -f "$socket_path" "$api_socket_path"

    app_executable="$app_install_root/Outer Shell.app/Contents/MacOS/Outer Shell"
    icon_path="$app_install_root/Outer Shell.app/Contents/Resources/app-icon.png"
    app_executable_xml="$(xml_escape "$app_executable")"
    socket_path_xml="$(xml_escape "$socket_path")"
    app_base_url_xml="$(xml_escape "$app_base_url")"
    public_base_url_xml="$(xml_escape "$public_base_url")"
    log_path_xml="$(xml_escape "$log_path")"
    cat > "$plist_path" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>org.outershell.OuterShell</string>
  <key>ProgramArguments</key>
  <array>
    <string>$app_executable_xml</string>
    <string>--socket-path</string>
    <string>$socket_path_xml</string>
    <string>--launchd-socket-name</string>
    <string>Listener</string>
    <string>--app-base-url</string>
    <string>$app_base_url_xml</string>
    <string>--public-base-url</string>
    <string>$public_base_url_xml</string>
  </array>
  <key>Sockets</key>
  <dict>
    <key>Listener</key>
    <dict>
      <key>SockPathName</key>
      <string>$socket_path_xml</string>
      <key>SockPathMode</key>
      <integer>384</integer>
    </dict>
  </dict>
  <key>StandardOutPath</key>
  <string>$log_path_xml</string>
  <key>StandardErrorPath</key>
  <string>$log_path_xml</string>
</dict>
</plist>
EOF

    printf '[%s] %s Outer Shell package %s from %s.\n' "$(timestamp)" "$command" "__OUTER_SHELL_VERSION__" "$public_base_url" >> "$log_path"

    bootstrap_outer_shell
    launchctl kickstart -k "gui/$(id -u)/$service_id" >/dev/null 2>&1 || true
    attempts=50
    while [ "$attempts" -gt 0 ]; do
        if [ -S "$api_socket_path" ]; then
            break
        fi
        sleep 0.1
        attempts=$((attempts - 1))
    done
    OUTERSHELL_HOME="$outershell_home" "$outerctl_path" backend upsert --backend "$service_id" --name "$display_name" --launchd-plist "$plist_path" --owns-plist true
    OUTERSHELL_HOME="$outershell_home" "$outerctl_path" app clear --backend "$service_id"
    OUTERSHELL_HOME="$outershell_home" "$outerctl_path" app add --backend "$service_id" --socket-path "$socket_path" --name "$display_name" --url "/" --icon-path "$icon_path"
    append_outerloop_http_unix_allowlist_entry user "$socket_path"
    OUTERSHELL_HOME="$outershell_home" "$outerctl_path" log clear --backend "$service_id"
    OUTERSHELL_HOME="$outershell_home" "$outerctl_path" log add --backend "$service_id" --path "$log_path"
    if [ "$command" = "update" ]; then
        printf 'Outer Shell updated to %s. The new version will run the next time Outer Shell starts.\n' "__OUTER_SHELL_VERSION__"
        exit 0
    fi

    attempts=50
    while [ "$attempts" -gt 0 ]; do
        if [ -S "$socket_path" ]; then
            printf '%s\n' "$socket_path"
            exit 0
        fi
        sleep 0.1
        attempts=$((attempts - 1))
    done

    echo "Outer Shell installed, but $socket_path did not appear." >&2
    exit 1
fi

root_install=false
if [ "$(id -u)" -eq 0 ]; then
    root_install=true
fi

system_outershell_home="/var/lib/outershell"
system_daemon_root="$system_outershell_home/outershelld"
system_outershelld_path="$system_daemon_root/outershelld"
system_outerctl_path="$system_outershell_home/bin/outerctl"
system_version_path="$system_daemon_root/version"
system_binary_users_dir="$system_outershell_home/system-binary-users"
system_binary_user_marker="$system_binary_users_dir/uid-$(id -u)"
home_dir="${HOME:-}"
if [ -z "$home_dir" ]; then
    if command -v getent >/dev/null 2>&1; then
        home_dir="$(getent passwd "$(id -u)" | awk -F: '{print $6}')"
    fi
    if [ -z "$home_dir" ]; then
        if [ "$(id -u)" -eq 0 ]; then
            home_dir="/root"
        else
            home_dir="/home/$(id -un)"
        fi
    fi
fi

if [ "$root_install" = true ]; then
    cache_home="/var/cache"
    outershell_cache_home="$cache_home/outershell"
    outer_shell_cache_root="$outershell_cache_home/outer-shell"
    outer_shell_install_cache="$outer_shell_cache_root/install"
    runtime_dir="/run"
    outershell_home="${OUTERSHELL_HOME:-$system_outershell_home}"
    install_root="$outershell_home/outer-shell"
    daemon_root="$outershell_home/outershelld"
    outershelld_path="$daemon_root/outershelld"
    daemon_version_path="$daemon_root/version"
    app_version_path="$install_root/version"
    outerctl_path="$outershell_home/bin/outerctl"
    unit_dir="/etc/systemd/system"
    socket_path="/run/org.outershell.OuterShell"
    api_socket_path="/run/outershelld-api"
    app_log_dir="/var/log/outergroup"
    daemon_log_dir="/var/log/outergroup"
    log_path="$app_log_dir/org.outershell.OuterShell.log"
    broker_log_path="$daemon_log_dir/outershelld.log"
    systemctl_scope="--system"
    service_wanted_by="multi-user.target"
    api_listen_stream="$api_socket_path"
else
    cache_home="${XDG_CACHE_HOME:-$home_dir/.cache}"
    outershell_cache_home="$cache_home/outershell"
    outer_shell_cache_root="$outershell_cache_home/outer-shell"
    outer_shell_install_cache="$outer_shell_cache_root/install"
    runtime_dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
    state_home="${XDG_STATE_HOME:-$home_dir/.local/state}"
    outershell_home="${OUTERSHELL_HOME:-$state_home/outershell}"
    install_root="$outershell_home/outer-shell"
    daemon_root="$outershell_home/outershelld"
    outershelld_path="$daemon_root/outershelld"
    daemon_version_path="$daemon_root/version"
    app_version_path="$install_root/version"
    outerctl_path="$outershell_home/bin/outerctl"
    unit_dir="$home_dir/.config/systemd/user"
    socket_path="$runtime_dir/org.outershell.OuterShell"
    api_socket_path="$runtime_dir/outershelld-api"
    app_log_dir="$install_root/logs"
    daemon_log_dir="$daemon_root/logs"
    log_path="$app_log_dir/OuterShellBackend.log"
    broker_log_path="$daemon_log_dir/outershelld.log"
    systemctl_scope="--user"
    service_wanted_by="default.target"
    api_listen_stream="%t/outershelld-api"
fi

root_binaries_match=false
if [ "$root_install" = false ] &&
   [ -x "$system_outershelld_path" ] &&
   [ -x "$system_outerctl_path" ] &&
   [ -f "$system_version_path" ] &&
   [ -f "$system_binary_user_marker" ] &&
   [ "$(stat -c %u "$system_binary_user_marker" 2>/dev/null || echo invalid)" = "$(id -u)" ] &&
   [ "$(cat "$system_version_path" 2>/dev/null || true)" = "__OUTER_SHELL_VERSION__" ]; then
    root_binaries_match=true
fi

run_outerctl() {
    OUTERSHELL_HOME="$outershell_home" OUTERSHELLD_API_SOCKET="$api_socket_path" "$outerctl_path" "$@"
}

cleanup_legacy_outeragent_user_unit() {
    [ "$root_install" = false ] || return 0
    systemctl --user disable --now outeragent.service >/dev/null 2>&1 || true
    rm -f "$unit_dir/outeragent.service"
    systemctl --user daemon-reload >/dev/null 2>&1 || true
    systemctl --user reset-failed outeragent.service >/dev/null 2>&1 || true
}

cleanup_legacy_outeragent_system_unit() {
    [ "$root_install" = true ] || return 0
    systemctl --system disable --now outerloop-rootd.service >/dev/null 2>&1 || true
    rm -f /etc/systemd/system/outerloop-rootd.service
    systemctl --system daemon-reload >/dev/null 2>&1 || true
    systemctl --system reset-failed outerloop-rootd.service >/dev/null 2>&1 || true
}

system_binary_users_empty() {
    if [ ! -d "$system_binary_users_dir" ]; then
        return 0
    fi
    for marker in "$system_binary_users_dir"/*; do
        [ -e "$marker" ] || continue
        name="$(basename "$marker")"
        case "$name" in
            root-apps)
                [ "$(stat -c %u "$marker" 2>/dev/null || echo invalid)" = "0" ] && return 1
                ;;
            uid-*)
                uid="${name#uid-}"
                case "$uid" in
                    *[!0-9]*|'') continue ;;
                esac
                [ "$(stat -c %u "$marker" 2>/dev/null || echo invalid)" = "$uid" ] && return 1
                ;;
        esac
    done
    return 0
}

remove_system_binaries_if_unused() {
    [ "$(id -u)" -eq 0 ] || return 0
    system_binary_users_empty || return 0
    systemctl --system disable --now outershelld.socket outershelld.service >/dev/null 2>&1 || true
    rm -f /etc/systemd/system/outershelld.service /etc/systemd/system/outershelld.socket /run/outershelld-api
    systemctl --system daemon-reload >/dev/null 2>&1 || true
    rm -f "$system_outershelld_path" "$system_outerctl_path" "$system_version_path"
    rm -f /var/log/outergroup/outershelld.log /var/log/outergroup/org.outershell.OuterShell.log
    if ! find "$system_outershell_home/apps" -mindepth 1 -print -quit 2>/dev/null | grep -q . &&
       ! find /opt/outergroup -mindepth 1 -print -quit 2>/dev/null | grep -q .; then
        rm -f "$system_outershell_home/registry.orwa" "$system_outershell_home/registry.orwa.lock"
        rmdir "$system_outershell_home/apps" /opt/outergroup >/dev/null 2>&1 || true
    fi
    rmdir "$system_daemon_root" "$system_outershell_home/bin" "$system_binary_users_dir" "$system_outershell_home" /var/log/outergroup >/dev/null 2>&1 || true
}

system_binary_cleanup_shell_functions() {
    cat <<'OUTERSHELL_SYSTEM_BINARY_CLEANUP_SH'
system_binary_users_empty() {
    if [ ! -d "$system_binary_users_dir" ]; then
        return 0
    fi
    for marker in "$system_binary_users_dir"/*; do
        [ -e "$marker" ] || continue
        name="$(basename "$marker")"
        case "$name" in
            root-apps)
                [ "$(stat -c %u "$marker" 2>/dev/null || echo invalid)" = "0" ] && return 1
                ;;
            uid-*)
                uid="${name#uid-}"
                case "$uid" in
                    *[!0-9]*|'') continue ;;
                esac
                [ "$(stat -c %u "$marker" 2>/dev/null || echo invalid)" = "$uid" ] && return 1
                ;;
        esac
    done
    return 0
}

remove_system_binaries_if_unused() {
    [ "$(id -u)" -eq 0 ] || return 0
    system_binary_users_empty || return 0
    systemctl --system disable --now outershelld.socket outershelld.service >/dev/null 2>&1 || true
    rm -f /etc/systemd/system/outershelld.service /etc/systemd/system/outershelld.socket /run/outershelld-api
    systemctl --system daemon-reload >/dev/null 2>&1 || true
    rm -f "$system_outershelld_path" "$system_outerctl_path" "$system_version_path"
    rm -f /var/log/outergroup/outershelld.log /var/log/outergroup/org.outershell.OuterShell.log
    if ! find "$system_outershell_home/apps" -mindepth 1 -print -quit 2>/dev/null | grep -q . &&
       ! find /opt/outergroup -mindepth 1 -print -quit 2>/dev/null | grep -q .; then
        rm -f "$system_outershell_home/registry.orwa" "$system_outershell_home/registry.orwa.lock"
        rmdir "$system_outershell_home/apps" /opt/outergroup >/dev/null 2>&1 || true
    fi
    rmdir "$system_daemon_root" "$system_outershell_home/bin" "$system_binary_users_dir" "$system_outershell_home" /var/log/outergroup >/dev/null 2>&1 || true
}
OUTERSHELL_SYSTEM_BINARY_CLEANUP_SH
}

remove_system_binary_user_marker_with_sudo() {
    [ "$root_install" = false ] || return 0
    [ -e "$system_binary_user_marker" ] || return 0
    if rm -f "$system_binary_user_marker" 2>/dev/null && ! system_binary_users_empty; then
        return 0
    fi

    cleanup_root_script="$(mktemp)"
    cat > "$cleanup_root_script" <<EOF
#!/bin/sh
set -eu
system_outershell_home="$system_outershell_home"
system_daemon_root="$system_daemon_root"
system_outershelld_path="$system_outershelld_path"
system_outerctl_path="$system_outerctl_path"
system_version_path="$system_version_path"
system_binary_users_dir="$system_binary_users_dir"
marker="$system_binary_user_marker"
expected_uid="$(id -u)"
if [ -e "\$marker" ] && [ "\$(stat -c %u "\$marker" 2>/dev/null || echo invalid)" = "\$expected_uid" ]; then
    rm -f "\$marker"
fi
$(system_binary_cleanup_shell_functions)
remove_system_binaries_if_unused
EOF
    chmod 0700 "$cleanup_root_script"
    if sudo -n sh "$cleanup_root_script" >/dev/null 2>&1; then
        rm -f "$cleanup_root_script"
        return 0
    fi
    printf 'Administrator password required to remove shared Outer Shell root support for this user.\n' >&2
    sudo sh "$cleanup_root_script"
    rm -f "$cleanup_root_script"
}

if [ "$command" = "uninstall" ]; then
    if [ "$root_install" = true ]; then
        systemctl --system disable org.outershell.OuterShell.socket >/dev/null 2>&1 || true
    else
        systemctl --user disable org.outershell.OuterShell.socket outershelld.socket outershelld.service >/dev/null 2>&1 || true
    fi
    if [ -x "$outerctl_path" ]; then
        run_outerctl app clear --backend org.outershell.OuterShell >/dev/null 2>&1 || true
        run_outerctl log clear --backend org.outershell.OuterShell >/dev/null 2>&1 || true
        run_outerctl systemd clear --backend org.outershell.OuterShell >/dev/null 2>&1 || true
        run_outerctl backend remove --backend org.outershell.OuterShell >/dev/null 2>&1 || true
    fi
    remove_system_binary_user_marker_with_sudo
    cleanup_script="$(mktemp)"
    cat > "$cleanup_script" <<EOF
#!/bin/sh
sleep 0.25
    if [ "$root_install" = true ]; then
        systemctl --system stop org.outershell.OuterShell.socket org.outershell.OuterShell.service >/dev/null 2>&1 || true
    else
        systemctl --user stop org.outershell.OuterShell.socket outershelld.socket org.outershell.OuterShell.service outershelld.service >/dev/null 2>&1 || true
    fi
    if [ "$root_install" = true ]; then
        rm -f "$unit_dir/org.outershell.OuterShell.service" "$unit_dir/org.outershell.OuterShell.socket" "$socket_path"
    else
        rm -f "$unit_dir/org.outershell.OuterShell.service" "$unit_dir/outershelld.service" "$unit_dir/outershelld.socket" "$unit_dir/org.outershell.OuterShell.socket" "$socket_path" "$api_socket_path"
    fi
systemctl $systemctl_scope daemon-reload >/dev/null 2>&1 || true
if [ "$root_install" = true ]; then
    rm -rf "$install_root"
    rm -f "$log_path"
else
    rm -rf "$install_root"
    rm -rf "$daemon_root"
    rm -f "$outerctl_path"
fi
rm -f "$system_binary_user_marker"
$(system_binary_cleanup_shell_functions)
system_outershell_home="$system_outershell_home"
system_daemon_root="$system_daemon_root"
system_outershelld_path="$system_outershelld_path"
system_outerctl_path="$system_outerctl_path"
system_version_path="$system_version_path"
system_binary_users_dir="$system_binary_users_dir"
remove_system_binaries_if_unused
if [ "$root_install" = false ] && [ "${OUTERSHELL_UNINSTALL_REMOVE_USER_STATE:-0}" = "1" ]; then
    rm -f "$outershell_home/registry.orwa" "$outershell_home/registry.orwa.lock"
    rmdir "$outershell_home/apps" >/dev/null 2>&1 || true
    rmdir "$outershell_home/bin" "$daemon_root" "$install_root" "$outershell_home" >/dev/null 2>&1 || true
    rm -rf "$outer_shell_cache_root"
else
    rm -rf "$outer_shell_install_cache"
fi
rmdir "$outer_shell_cache_root" "$outershell_cache_home" >/dev/null 2>&1 || true
rm -f "$cleanup_script"
EOF
    chmod 0755 "$cleanup_script"
    if command -v systemd-run >/dev/null 2>&1; then
        systemd-run $systemctl_scope --unit=org.outershell.OuterShell-uninstall --collect "$cleanup_script" >/dev/null 2>&1 || (nohup "$cleanup_script" >/dev/null 2>&1 &)
    else
        nohup "$cleanup_script" >/dev/null 2>&1 &
    fi
    printf 'Outer Shell has been uninstalled. Outer Loop will offer to install it again the next time it connects.\n'
    exit 0
fi

mkdir -p "$install_root" "$daemon_root" "$outershell_home/bin" "$unit_dir" "$app_log_dir" "$daemon_log_dir"
cleanup_legacy_outeragent_user_unit
cleanup_legacy_outeragent_system_unit
archive_path="$(mktemp)"
payload_outerctl_path="$(mktemp)"
payload_root="$(mktemp -d)"
stage_archive "${public_base_url%/}/latest/outer-shell-linux-${arch}.tar.gz?v=__ASSET_VERSION__" "$archive_path"
tar -xzf "$archive_path" -C "$payload_root"
rm -f "$archive_path"
payload="$payload_root/OuterShell"
app_payload="$payload/apps/org.outershell.OuterShell"
rm -rf "$install_root"
mkdir -p "$install_root/bundles" "$daemon_root" "$outershell_home/bin" "$unit_dir" "$app_log_dir" "$daemon_log_dir"
install -m 0755 "$payload/tools/outershelld" "$outershelld_path"
install -m 0755 "$payload/tools/outerctl" "$payload_outerctl_path"
install -m 0755 "$app_payload/OuterShellBackend" "$install_root/OuterShellBackend"
install -m 0644 "$app_payload/app-icon.png" "$install_root/app-icon.png"
install -m 0644 "$app_payload/bundles/OuterShell.bundle.macos-arm.aar" "$install_root/bundles/OuterShell.bundle.macos-arm.aar"
install -m 0644 "$app_payload/bundles/OuterShell.bundle.macos-x86.aar" "$install_root/bundles/OuterShell.bundle.macos-x86.aar"
rm -rf "$payload_root"
chmod 0755 "$outershelld_path"
chmod 0755 "$install_root/OuterShellBackend"
chmod 0755 "$payload_outerctl_path"
if [ "$root_install" = true ]; then
    mkdir -p "$system_binary_users_dir"
    chmod 1777 "$system_binary_users_dir"
    touch "$system_binary_user_marker"
    chown 0:0 "$system_binary_user_marker" 2>/dev/null || true
    install -m 0755 "$payload_outerctl_path" "$outerctl_path"
elif [ "$root_binaries_match" = true ]; then
    rm -f "$outershelld_path" "$outerctl_path"
    ln -s "$system_outershelld_path" "$outershelld_path"
    ln -s "$system_outerctl_path" "$outerctl_path"
else
    install -m 0755 "$payload_outerctl_path" "$outerctl_path"
fi
rm -f "$payload_outerctl_path"
printf '%s\n' "__OUTER_SHELL_VERSION__" > "$app_version_path"
printf '%s\n' "__OUTER_SHELL_VERSION__" > "$daemon_version_path"
touch "$log_path" "$broker_log_path"

outer_shell_exec="$(systemd_quote_arg "$install_root/OuterShellBackend") --socket-path $(systemd_quote_arg "$socket_path") --api-socket-path $(systemd_quote_arg "$api_socket_path") --bundles-dir $(systemd_quote_arg "$install_root/bundles") --bundled-apps-dir $(systemd_quote_arg "$install_root/bundled-apps") --app-base-url $(systemd_quote_arg "$app_base_url") --public-base-url $(systemd_quote_arg "$public_base_url")"

cat > "$unit_dir/org.outershell.OuterShell.service" <<EOF
[Unit]
Description=Outer Group Outer Shell
After=outershelld.socket
Wants=outershelld.socket

[Service]
Environment=OUTERSHELL_HOME=$outershell_home
ExecStart=$outer_shell_exec
Restart=no
StandardOutput=append:$log_path
StandardError=append:$log_path

[Install]
WantedBy=$service_wanted_by
EOF

cat > "$unit_dir/org.outershell.OuterShell.socket" <<EOF
[Unit]
Description=Outer Group Outer Shell Socket

[Socket]
ListenStream=$socket_path
FileDescriptorName=http
SocketMode=0600
Service=org.outershell.OuterShell.service

[Install]
WantedBy=sockets.target
EOF

cat > "$unit_dir/outershelld.socket" <<EOF
[Unit]
Description=Outer Shell API Socket

[Socket]
ListenStream=$api_listen_stream
FileDescriptorName=api
SocketMode=0600
Service=outershelld.service

[Install]
WantedBy=sockets.target
EOF

cat > "$unit_dir/outershelld.service" <<EOF
[Unit]
Description=Outer Shell daemon

[Service]
Environment=OUTERSHELL_HOME=$outershell_home
Environment=OUTER_SHELL_PUBLIC_BASE_URL=$public_base_url
ExecStart=$outershelld_path
Restart=no
StandardOutput=append:$broker_log_path
StandardError=append:$broker_log_path

[Install]
WantedBy=$service_wanted_by
EOF

printf '[%s] %s Outer Shell package %s from %s.\n' "$(timestamp)" "$command" "__OUTER_SHELL_VERSION__" "$public_base_url" >> "$log_path"

if [ "$command" = "install" ]; then
    systemctl $systemctl_scope stop org.outershell.OuterShell.service >/dev/null 2>&1 || true
fi
systemctl $systemctl_scope daemon-reload
systemctl $systemctl_scope enable org.outershell.OuterShell.socket outershelld.socket
systemctl $systemctl_scope stop org.outershell.OuterShell.socket outershelld.socket >/dev/null 2>&1 || true
systemctl $systemctl_scope stop org.outershell.OuterShell.service outershelld.service >/dev/null 2>&1 || true
systemctl $systemctl_scope reset-failed org.outershell.OuterShell.socket outershelld.socket org.outershell.OuterShell.service outershelld.service >/dev/null 2>&1 || true
rm -f "$socket_path" "$api_socket_path"
systemctl $systemctl_scope start outershelld.socket
systemctl $systemctl_scope start org.outershell.OuterShell.socket

run_outerctl backend upsert --backend org.outershell.OuterShell --name "Outer Shell" --systemd-unit org.outershell.OuterShell.service
run_outerctl app clear --backend org.outershell.OuterShell
run_outerctl app add --backend org.outershell.OuterShell --socket-path "$socket_path" --name "Outer Shell" --url "/" --icon-path "$install_root/app-icon.png"
if [ "$root_install" = true ]; then
    append_outerloop_http_unix_allowlist_entry system "$socket_path"
else
    append_outerloop_http_unix_allowlist_entry user "$socket_path"
fi
run_outerctl log clear --backend org.outershell.OuterShell
run_outerctl log add --backend org.outershell.OuterShell --path "$log_path"

if [ "$command" = "update" ]; then
    printf 'Outer Shell updated to %s. The new version will run the next time Outer Shell starts.\n' "__OUTER_SHELL_VERSION__"
    exit 0
fi

attempts=50
while [ "$attempts" -gt 0 ]; do
    if [ -S "$socket_path" ]; then
        printf '%s\n' "$socket_path"
        exit 0
    fi
    sleep 0.1
    attempts=$((attempts - 1))
done

echo "Outer Shell installed, but $socket_path did not appear." >&2
exit 1
INSTALL_SH
python3 - "${OUTPUT_ROOT}/latest/install.sh" "${ASSET_VERSION}" "${PUBLIC_BASE_URL}" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text()
text = text.replace("__ASSET_VERSION__", sys.argv[2])
text = text.replace("__PUBLIC_BASE_URL__", sys.argv[3])
text = text.replace("__OUTER_SHELL_VERSION__", "__OUTER_SHELL_VERSION_REPLACE__")
path.write_text(text)
PY
python3 - "${OUTPUT_ROOT}/latest/install.sh" "${OUTER_SHELL_VERSION}" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text()
text = text.replace("__OUTER_SHELL_VERSION_REPLACE__", sys.argv[2])
path.write_text(text)
PY
chmod 0755 "${OUTPUT_ROOT}/latest/install.sh"

echo "Packaged assets under ${OUTPUT_ROOT}"
