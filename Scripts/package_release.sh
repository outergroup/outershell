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
require_file "${RUN_ROOT}/bundles/BackendsContent.bundle.macos-arm.aar"
require_file "${RUN_ROOT}/bundles/BackendsContent.bundle.macos-x86.aar"
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
    install -m 0644 "${RUN_ROOT}/bundles/BackendsContent.bundle.macos-arm.aar" "${root}/bundles/BackendsContent.bundle.macos-arm.aar"
    install -m 0644 "${RUN_ROOT}/bundles/BackendsContent.bundle.macos-x86.aar" "${root}/bundles/BackendsContent.bundle.macos-x86.aar"
    tar --format ustar --no-xattrs -C "${STAGING_ROOT}/outer-shell-${arch}" -czf "${OUTPUT_ROOT}/latest/outer-shell-${arch}.tar.gz" OuterShell
}

stage_home_screen_macos() {
    local root="${STAGING_ROOT}/outer-shell-macos/OuterShell"
    mkdir -p "${root}/bin" "${root}/bundles"
    ditto "${MACOS_BUILD_ROOT}/Outer Shell.app" "${root}/Outer Shell.app"
    install -m 0755 "${MACOS_BUILD_ROOT}/outershelld" "${root}/outershelld"
    install -m 0644 "${REPO_ROOT}/app-icon.png" "${root}/app-icon.png"
    install -m 0644 "${RUN_ROOT}/bundles/BackendsContent.bundle.macos-arm.aar" "${root}/bundles/BackendsContent.bundle.macos-arm.aar"
    install -m 0644 "${RUN_ROOT}/bundles/BackendsContent.bundle.macos-x86.aar" "${root}/bundles/BackendsContent.bundle.macos-x86.aar"
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
        if launchctl bootstrap "gui/$(id -u)" "$plist_path"; then
            return 0
        fi
        first_error="$(launchctl bootstrap "gui/$(id -u)" "$plist_path" 2>&1 >/dev/null || true)"
        unload_outer_shell
        rm -f "$socket_path"
        if launchctl bootstrap "gui/$(id -u)" "$plist_path"; then
            return 0
        fi
        second_error="$(launchctl bootstrap "gui/$(id -u)" "$plist_path" 2>&1 >/dev/null || true)"
        if [ -n "$first_error" ]; then
            printf '%s\n' "$first_error" >&2
        fi
        if [ -n "$second_error" ]; then
            printf '%s\n' "$second_error" >&2
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
    download "${public_base_url%/}/latest/outer-shell-macos.tar.gz?v=__ASSET_VERSION__" "$archive_path"
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
    rm -f "$socket_path"

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
    OUTERSHELL_HOME="$outershell_home" "$outerctl_path" backend upsert --backend "$service_id" --name "$display_name" --launchd-plist "$plist_path" --owns-plist true
    OUTERSHELL_HOME="$outershell_home" "$outerctl_path" app clear --backend "$service_id"
    OUTERSHELL_HOME="$outershell_home" "$outerctl_path" app add --backend "$service_id" --socket-path "$socket_path" --name "$display_name" --url "$socket_path" --icon-path "$install_root/app-icon.png"
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

runtime_dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
state_home="${XDG_STATE_HOME:-$HOME/.local/state}"
outershell_home="${OUTERSHELL_HOME:-$state_home/outershell}"
install_root="$outershell_home/outer-shell"
outerctl_path="$outershell_home/bin/outerctl"
unit_dir="$HOME/.config/systemd/user"
socket_path="$runtime_dir/org.outershell.OuterShell"
log_dir="$install_root/logs"
log_path="$log_dir/OuterShellBackend.log"
broker_log_path="$log_dir/outershelld.log"
runner_path="$install_root/run-outer-shell.sh"
broker_runner_path="$install_root/run-outershelld.sh"

cleanup_legacy_home_screen() {
    legacy_install_root="$outershell_home/home-screen"
    legacy_socket_path="$runtime_dir/dev.outergroup.HomeScreen"
    legacy_service_id="dev.outergroup.HomeScreen"
    systemctl --user disable --now dev.outergroup.HomeScreen.socket dev.outergroup.HomeScreen.service >/dev/null 2>&1 || true
    rm -f "$unit_dir/dev.outergroup.HomeScreen.service" "$unit_dir/dev.outergroup.HomeScreen.socket" "$legacy_socket_path"
    systemctl --user daemon-reload >/dev/null 2>&1 || true
    rm -rf "$legacy_install_root"
    if [ -x "$outerctl_path" ]; then
        OUTERSHELL_HOME="$outershell_home" "$outerctl_path" app clear --backend "$legacy_service_id" >/dev/null 2>&1 || true
        OUTERSHELL_HOME="$outershell_home" "$outerctl_path" log clear --backend "$legacy_service_id" >/dev/null 2>&1 || true
        OUTERSHELL_HOME="$outershell_home" "$outerctl_path" systemd clear --backend "$legacy_service_id" >/dev/null 2>&1 || true
        OUTERSHELL_HOME="$outershell_home" "$outerctl_path" backend remove --backend "$legacy_service_id" >/dev/null 2>&1 || true
    fi
}

if [ "$command" = "uninstall" ]; then
    systemctl --user disable org.outershell.OuterShell.socket outershelld.socket outershelld.service >/dev/null 2>&1 || true
    if [ -x "$outerctl_path" ]; then
        OUTERSHELL_HOME="$outershell_home" "$outerctl_path" app clear --backend org.outershell.OuterShell >/dev/null 2>&1 || true
        OUTERSHELL_HOME="$outershell_home" "$outerctl_path" log clear --backend org.outershell.OuterShell >/dev/null 2>&1 || true
        OUTERSHELL_HOME="$outershell_home" "$outerctl_path" systemd clear --backend org.outershell.OuterShell >/dev/null 2>&1 || true
        OUTERSHELL_HOME="$outershell_home" "$outerctl_path" backend remove --backend org.outershell.OuterShell >/dev/null 2>&1 || true
    fi
    cleanup_script="$(mktemp)"
    cat > "$cleanup_script" <<EOF
#!/bin/sh
sleep 0.25
    systemctl --user stop org.outershell.OuterShell.socket outershelld.socket org.outershell.OuterShell.service outershelld.service >/dev/null 2>&1 || true
    rm -f "$unit_dir/org.outershell.OuterShell.service" "$unit_dir/outershelld.service" "$unit_dir/outershelld.socket" "$unit_dir/org.outershell.OuterShell.socket" "$socket_path" "$runtime_dir/outershelld-api"
systemctl --user daemon-reload >/dev/null 2>&1 || true
rm -rf "$install_root"
rm -f "$cleanup_script"
EOF
    chmod 0755 "$cleanup_script"
    if command -v systemd-run >/dev/null 2>&1; then
        systemd-run --user --unit=org.outershell.OuterShell-uninstall --collect "$cleanup_script" >/dev/null 2>&1 || (nohup "$cleanup_script" >/dev/null 2>&1 &)
    else
        nohup "$cleanup_script" >/dev/null 2>&1 &
    fi
    cleanup_legacy_home_screen
    printf 'Outer Shell has been uninstalled. Outer Loop will offer to install it again the next time it connects.\n'
    exit 0
fi

mkdir -p "$install_root" "$outershell_home/bin" "$unit_dir" "$log_dir"
cleanup_legacy_home_screen
archive_path="$(mktemp)"
download "${public_base_url%/}/latest/outer-shell-${arch}.tar.gz?v=__ASSET_VERSION__" "$archive_path"
rm -f "$install_root/outershelld" "$install_root/OuterShellBackend"
tar -xzf "$archive_path" -C "$install_root" --strip-components=1
rm -f "$archive_path"
chmod 0755 "$install_root/outershelld"
chmod 0755 "$install_root/OuterShellBackend"
chmod 0755 "$install_root/bin/outerctl"
install -m 0755 "$install_root/bin/outerctl" "$outerctl_path"
printf '%s\n' "__OUTER_SHELL_VERSION__" > "$install_root/version"
touch "$log_path" "$broker_log_path"

cat > "$runner_path" <<EOF
#!/bin/sh
exec "$install_root/OuterShellBackend" --socket-path "\${1:-$socket_path}" --api-socket-path "$runtime_dir/outershelld-api" --bundles-dir "$install_root/bundles" --bundled-apps-dir "$install_root/bundled-apps" --app-base-url "$app_base_url" --public-base-url "$public_base_url" >> "$log_path" 2>&1
EOF
chmod 0755 "$runner_path"

cat > "$broker_runner_path" <<EOF
#!/bin/sh
exec "$install_root/outershelld" --api-socket-path "\${1:-$runtime_dir/outershelld-api}" --bundled-apps-dir "$install_root/bundled-apps" --app-base-url "$app_base_url" --public-base-url "$public_base_url" >> "$broker_log_path" 2>&1
EOF
chmod 0755 "$broker_runner_path"

cat > "$unit_dir/org.outershell.OuterShell.service" <<EOF
[Unit]
Description=Outer Group Outer Shell
After=outershelld.socket
Wants=outershelld.socket

[Service]
Environment=OUTERSHELL_HOME=$outershell_home
ExecStart=$runner_path %t/org.outershell.OuterShell
Restart=no
EOF

cat > "$unit_dir/org.outershell.OuterShell.socket" <<EOF
[Unit]
Description=Outer Group Outer Shell Socket

[Socket]
ListenStream=%t/org.outershell.OuterShell
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
ListenStream=%t/outershelld-api
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
ExecStart=$broker_runner_path %t/outershelld-api
Restart=no
EOF

printf '[%s] %s Outer Shell package %s from %s.\n' "$(timestamp)" "$command" "__OUTER_SHELL_VERSION__" "$public_base_url" >> "$log_path"

if [ "$command" = "install" ]; then
    systemctl --user stop org.outershell.OuterShell.service >/dev/null 2>&1 || true
fi
systemctl --user daemon-reload
systemctl --user enable org.outershell.OuterShell.socket outershelld.socket
systemctl --user stop org.outershell.OuterShell.service outershelld.service org.outershell.OuterShell.socket outershelld.socket >/dev/null 2>&1 || true
rm -f "$socket_path" "$runtime_dir/outershelld-api"
systemctl --user start org.outershell.OuterShell.socket outershelld.socket

OUTERSHELL_HOME="$outershell_home" "$outerctl_path" backend upsert --backend org.outershell.OuterShell --name "Outer Shell" --systemd-unit org.outershell.OuterShell.service
OUTERSHELL_HOME="$outershell_home" "$outerctl_path" app clear --backend org.outershell.OuterShell
OUTERSHELL_HOME="$outershell_home" "$outerctl_path" app add --backend org.outershell.OuterShell --socket-path "$socket_path" --name "Outer Shell" --url "$socket_path" --icon-path "$install_root/app-icon.png"
OUTERSHELL_HOME="$outershell_home" "$outerctl_path" log clear --backend org.outershell.OuterShell
OUTERSHELL_HOME="$outershell_home" "$outerctl_path" log add --backend org.outershell.OuterShell --path "$log_path"

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
