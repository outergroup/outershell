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

stage_home_screen() {
    local arch="$1"
    local root="${STAGING_ROOT}/outer-shell-${arch}/OuterShell"
    mkdir -p "${root}/bin" "${root}/bundles"
    install -m 0755 "${PACKAGE_ROOT}/RemoteLinuxBinaries/${arch}/outershelld" "${root}/outershelld"
    install -m 0755 "${PACKAGE_ROOT}/RemoteLinuxBinaries/${arch}/OuterShellBackend" "${root}/OuterShellBackend"
    install -m 0755 "${PACKAGE_ROOT}/RemoteLinuxBinaries/${arch}/outerctl" "${root}/bin/outerctl"
    install -m 0644 "${REPO_ROOT}/app-icon.png" "${root}/app-icon.png"
    install -m 0644 "${RUN_ROOT}/bundles/OuterShell.bundle.macos-arm.aar" "${root}/bundles/OuterShell.bundle.macos-arm.aar"
    install -m 0644 "${RUN_ROOT}/bundles/OuterShell.bundle.macos-x86.aar" "${root}/bundles/OuterShell.bundle.macos-x86.aar"
    tar --format ustar --no-xattrs -C "${STAGING_ROOT}/outer-shell-${arch}" -czf "${OUTPUT_ROOT}/latest/outer-shell-${arch}.tar.gz" OuterShell
}

stage_home_screen_macos() {
    local root="${STAGING_ROOT}/outer-shell-macos/OuterShell"
    mkdir -p "${root}/bin" "${root}/bundles"
    ditto "${MACOS_BUILD_ROOT}/Outer Shell.app" "${root}/Outer Shell.app"
    install -m 0755 "${MACOS_BUILD_ROOT}/outershelld" "${root}/outershelld"
    install -m 0644 "${REPO_ROOT}/app-icon.png" "${root}/app-icon.png"
    install -m 0644 "${RUN_ROOT}/bundles/OuterShell.bundle.macos-arm.aar" "${root}/bundles/OuterShell.bundle.macos-arm.aar"
    install -m 0644 "${RUN_ROOT}/bundles/OuterShell.bundle.macos-x86.aar" "${root}/bundles/OuterShell.bundle.macos-x86.aar"
    if [[ -d "${RUN_ROOT}/bundled-apps" ]]; then
        ditto "${RUN_ROOT}/bundled-apps" "${root}/bundled-apps"
    fi
    clang++ -std=c++17 "${REPO_ROOT}/Resources/outerctl.cpp" \
        -o "${root}/bin/outerctl"
    chmod 0755 "${root}/bin/outerctl"
    tar --format ustar --no-xattrs -C "${STAGING_ROOT}/outer-shell-macos" -czf "${OUTPUT_ROOT}/latest/outer-shell-macos.tar.gz" OuterShell
}

stage_home_screen aarch64
stage_home_screen x86_64
stage_home_screen_macos

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

case "$(uname -m)" in
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
    install_root="$outershell_home/outer-shell"
    outerctl_path="$outershell_home/bin/outerctl"
    launch_agent_dir="$HOME/Library/LaunchAgents"
    plist_path="$launch_agent_dir/org.outershell.OuterShell.plist"
    socket_path="$(getconf DARWIN_USER_TEMP_DIR)org.outershell.OuterShell"
    api_socket_path="$(getconf DARWIN_USER_TEMP_DIR)outershelld-api"
    log_dir="$HOME/Library/Logs/org.outershell.OuterShell"
    log_path="$log_dir/output.log"
    service_id="org.outershell.OuterShell"
    display_name="Outer Shell"

    unload_outer_shell() {
        launchctl bootout "gui/$(id -u)/$service_id" >/dev/null 2>&1 || launchctl remove "$service_id" >/dev/null 2>&1 || true
        attempts=20
        while [ "$attempts" -gt 0 ]; do
            if ! launchctl print "gui/$(id -u)/$service_id" >/dev/null 2>&1; then
                return 0
            fi
            sleep 0.1
            attempts=$((attempts - 1))
        done
        return 0
    }

    cleanup_legacy_home_screen() {
        legacy_service_id="dev.outergroup.HomeScreen"
        legacy_plist_path="$launch_agent_dir/dev.outergroup.HomeScreen.plist"
        legacy_socket_path="$(getconf DARWIN_USER_TEMP_DIR)dev.outergroup.HomeScreen"
        legacy_install_root="$outershell_home/home-screen"
        launchctl bootout "gui/$(id -u)/$legacy_service_id" >/dev/null 2>&1 || launchctl remove "$legacy_service_id" >/dev/null 2>&1 || true
        rm -f "$legacy_plist_path" "$legacy_socket_path"
        rm -rf "$legacy_install_root"
        if [ -x "$outerctl_path" ]; then
            OUTERSHELL_HOME="$outershell_home" "$outerctl_path" app clear --backend "$legacy_service_id" >/dev/null 2>&1 || true
            OUTERSHELL_HOME="$outershell_home" "$outerctl_path" log clear --backend "$legacy_service_id" >/dev/null 2>&1 || true
            OUTERSHELL_HOME="$outershell_home" "$outerctl_path" launchd clear --backend "$legacy_service_id" >/dev/null 2>&1 || true
            OUTERSHELL_HOME="$outershell_home" "$outerctl_path" backend remove --backend "$legacy_service_id" >/dev/null 2>&1 || true
        fi
    }

    bootstrap_outer_shell() {
        attempts=20
        last_error=""
        while [ "$attempts" -gt 0 ]; do
            if last_error="$(launchctl bootstrap "gui/$(id -u)" "$plist_path" 2>&1)"; then
                return 0
            fi
            unload_outer_shell
            rm -f "$socket_path" "$api_socket_path"
            sleep 0.1
            attempts=$((attempts - 1))
        done
        if [ -n "$last_error" ]; then
            printf '%s\n' "$last_error" >&2
        fi
        return 1
    }

    if [ "$command" = "uninstall" ]; then
        unload_outer_shell
        rm -f "$plist_path" "$socket_path"
        if [ -x "$outerctl_path" ]; then
            OUTERSHELL_HOME="$outershell_home" "$outerctl_path" app clear --backend "$service_id" >/dev/null 2>&1 || true
            OUTERSHELL_HOME="$outershell_home" "$outerctl_path" log clear --backend "$service_id" >/dev/null 2>&1 || true
            OUTERSHELL_HOME="$outershell_home" "$outerctl_path" launchd clear --backend "$service_id" >/dev/null 2>&1 || true
            OUTERSHELL_HOME="$outershell_home" "$outerctl_path" backend remove --backend "$service_id" >/dev/null 2>&1 || true
        fi
        rm -rf "$install_root"
        cleanup_legacy_home_screen
        printf 'Outer Shell has been uninstalled. Outer Loop will offer to install it again the next time it connects.\n'
        exit 0
    fi

    mkdir -p "$install_root" "$outershell_home/bin" "$launch_agent_dir" "$log_dir"
    archive_path="$(mktemp)"
    stage_archive "${public_base_url%/}/latest/outer-shell-macos.tar.gz?v=__ASSET_VERSION__" "$archive_path"
    rm -rf "$install_root"
    mkdir -p "$install_root" "$outershell_home/bin" "$log_dir"
    tar -xzf "$archive_path" -C "$install_root" --strip-components=1
    rm -f "$archive_path"
    chmod 0755 "$install_root/Outer Shell.app/Contents/MacOS/Outer Shell"
    chmod 0755 "$install_root/outershelld"
    chmod 0755 "$install_root/bin/outerctl"
    install -m 0755 "$install_root/bin/outerctl" "$outerctl_path"
    printf '%s\n' "__OUTER_SHELL_VERSION__" > "$install_root/version"
    touch "$log_path"

    unload_outer_shell
    rm -f "$socket_path" "$api_socket_path"

    app_executable="$install_root/Outer Shell.app/Contents/MacOS/Outer Shell"
    bundles_dir="$install_root/bundles"
    bundled_apps_dir="$install_root/bundled-apps"
    app_executable_xml="$(xml_escape "$app_executable")"
    socket_path_xml="$(xml_escape "$socket_path")"
    bundles_dir_xml="$(xml_escape "$bundles_dir")"
    bundled_apps_dir_xml="$(xml_escape "$bundled_apps_dir")"
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
    <string>--bundles-dir</string>
    <string>$bundles_dir_xml</string>
    <string>--bundled-apps-dir</string>
    <string>$bundled_apps_dir_xml</string>
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

    cleanup_legacy_home_screen
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
    OUTERSHELL_HOME="$outershell_home" "$outerctl_path" app add --backend "$service_id" --socket-path "$socket_path" --name "$display_name" --url "/" --icon-path "$install_root/app-icon.png"
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

if [ "$root_install" = true ]; then
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
    runtime_dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
    state_home="${XDG_STATE_HOME:-$HOME/.local/state}"
    outershell_home="${OUTERSHELL_HOME:-$state_home/outershell}"
    install_root="$outershell_home/outer-shell"
    daemon_root="$outershell_home/outershelld"
    outershelld_path="$daemon_root/outershelld"
    daemon_version_path="$daemon_root/version"
    app_version_path="$install_root/version"
    outerctl_path="$outershell_home/bin/outerctl"
    unit_dir="$HOME/.config/systemd/user"
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

cleanup_legacy_home_screen() {
    legacy_install_root="$outershell_home/home-screen"
    legacy_socket_path="$runtime_dir/dev.outergroup.HomeScreen"
    legacy_service_id="dev.outergroup.HomeScreen"
    systemctl $systemctl_scope disable --now dev.outergroup.HomeScreen.socket dev.outergroup.HomeScreen.service >/dev/null 2>&1 || true
    rm -f "$unit_dir/dev.outergroup.HomeScreen.service" "$unit_dir/dev.outergroup.HomeScreen.socket" "$legacy_socket_path"
    systemctl $systemctl_scope daemon-reload >/dev/null 2>&1 || true
    rm -rf "$legacy_install_root"
    if [ -x "$outerctl_path" ]; then
        run_outerctl app clear --backend "$legacy_service_id" >/dev/null 2>&1 || true
        run_outerctl log clear --backend "$legacy_service_id" >/dev/null 2>&1 || true
        run_outerctl systemd clear --backend "$legacy_service_id" >/dev/null 2>&1 || true
        run_outerctl backend remove --backend "$legacy_service_id" >/dev/null 2>&1 || true
    fi
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
rm -f "$cleanup_script"
EOF
    chmod 0755 "$cleanup_script"
    if [ "$root_install" = false ] && command -v systemd-run >/dev/null 2>&1; then
        systemd-run $systemctl_scope --unit=org.outershell.OuterShell-uninstall --collect "$cleanup_script" >/dev/null 2>&1 || (nohup "$cleanup_script" >/dev/null 2>&1 &)
    else
        nohup "$cleanup_script" >/dev/null 2>&1 &
    fi
    cleanup_legacy_home_screen
    printf 'Outer Shell has been uninstalled. Outer Loop will offer to install it again the next time it connects.\n'
    exit 0
fi

mkdir -p "$install_root" "$daemon_root" "$outershell_home/bin" "$unit_dir" "$app_log_dir" "$daemon_log_dir"
cleanup_legacy_home_screen
archive_path="$(mktemp)"
payload_outerctl_path="$(mktemp)"
stage_archive "${public_base_url%/}/latest/outer-shell-${arch}.tar.gz?v=__ASSET_VERSION__" "$archive_path"
rm -f "$outershelld_path" "$outerctl_path" "$install_root/OuterShellBackend" "$install_root/outershelld" "$install_root/bin/outerctl" "$install_root/run-outer-shell.sh"
rm -rf "$install_root/bin"
tar -xzf "$archive_path" -C "$install_root" --strip-components=1
rm -f "$archive_path"
mv "$install_root/outershelld" "$outershelld_path"
mv "$install_root/bin/outerctl" "$payload_outerctl_path"
rmdir "$install_root/bin" >/dev/null 2>&1 || true
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
systemctl $systemctl_scope stop org.outershell.OuterShell.service outershelld.service org.outershell.OuterShell.socket outershelld.socket >/dev/null 2>&1 || true
rm -f "$socket_path" "$api_socket_path"
systemctl $systemctl_scope start org.outershell.OuterShell.socket outershelld.socket

run_outerctl backend upsert --backend org.outershell.OuterShell --name "Outer Shell" --systemd-unit org.outershell.OuterShell.service
run_outerctl app clear --backend org.outershell.OuterShell
run_outerctl app add --backend org.outershell.OuterShell --socket-path "$socket_path" --name "Outer Shell" --url "/" --icon-path "$install_root/app-icon.png"
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
