#!/bin/bash

set -euo pipefail
export COPYFILE_DISABLE=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTPUT_ROOT="${OUTPUT_ROOT:-${REPO_ROOT}/build/release/home-screen}"
RUN_ROOT="${RUN_ROOT:-${REPO_ROOT}/build/run}"
PACKAGE_ROOT="${PACKAGE_ROOT:-${REPO_ROOT}/build/linux-package}"
MACOS_BUILD_ROOT="${MACOS_BUILD_ROOT:-${REPO_ROOT}/build/macos/Release}"
PUBLIC_BASE_URL="${PUBLIC_BASE_URL:-}"
APP_CATALOG_PATH="${APP_CATALOG_PATH:-}"
HOME_SCREEN_VERSION="${HOME_SCREEN_VERSION:-0.0.0.DEV}"

if [[ -z "${PUBLIC_BASE_URL}" ]]; then
    echo "error: set PUBLIC_BASE_URL to the public Home Screen asset base URL" >&2
    exit 1
fi
PUBLIC_BASE_URL="${PUBLIC_BASE_URL%/}"

require_file() {
    if [[ ! -f "$1" ]]; then
        echo "error: missing $1" >&2
        exit 1
    fi
}

require_file "${PACKAGE_ROOT}/RemoteLinuxBinaries/aarch64/HomeScreenBackend"
require_file "${PACKAGE_ROOT}/RemoteLinuxBinaries/x86_64/HomeScreenBackend"
require_file "${PACKAGE_ROOT}/RemoteLinuxBinaries/aarch64/outerctl"
require_file "${PACKAGE_ROOT}/RemoteLinuxBinaries/x86_64/outerctl"
require_file "${RUN_ROOT}/bundles/BackendsContent.bundle.macos-arm.aar"
require_file "${RUN_ROOT}/bundles/BackendsContent.bundle.macos-x86.aar"
require_file "${MACOS_BUILD_ROOT}/Home Screen.app/Contents/MacOS/Home Screen"
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
    local root="${STAGING_ROOT}/home-screen-${arch}/HomeScreen"
    mkdir -p "${root}/bin" "${root}/bundles"
    install -m 0755 "${PACKAGE_ROOT}/RemoteLinuxBinaries/${arch}/HomeScreenBackend" "${root}/HomeScreenBackend"
    install -m 0755 "${PACKAGE_ROOT}/RemoteLinuxBinaries/${arch}/outerctl" "${root}/bin/outerctl"
    install -m 0644 "${REPO_ROOT}/app-icon.png" "${root}/app-icon.png"
    install -m 0644 "${RUN_ROOT}/bundles/BackendsContent.bundle.macos-arm.aar" "${root}/bundles/BackendsContent.bundle.macos-arm.aar"
    install -m 0644 "${RUN_ROOT}/bundles/BackendsContent.bundle.macos-x86.aar" "${root}/bundles/BackendsContent.bundle.macos-x86.aar"
    tar --format ustar --no-xattrs -C "${STAGING_ROOT}/home-screen-${arch}" -czf "${OUTPUT_ROOT}/latest/home-screen-${arch}.tar.gz" HomeScreen
}

stage_home_screen_macos() {
    local root="${STAGING_ROOT}/home-screen-macos/HomeScreen"
    mkdir -p "${root}/bin" "${root}/bundles"
    ditto "${MACOS_BUILD_ROOT}/Home Screen.app" "${root}/Home Screen.app"
    install -m 0644 "${REPO_ROOT}/app-icon.png" "${root}/app-icon.png"
    install -m 0644 "${RUN_ROOT}/bundles/BackendsContent.bundle.macos-arm.aar" "${root}/bundles/BackendsContent.bundle.macos-arm.aar"
    install -m 0644 "${RUN_ROOT}/bundles/BackendsContent.bundle.macos-x86.aar" "${root}/bundles/BackendsContent.bundle.macos-x86.aar"
    if [[ -d "${RUN_ROOT}/bundled-apps" ]]; then
        ditto "${RUN_ROOT}/bundled-apps" "${root}/bundled-apps"
    fi
    clang++ -std=c++17 "${REPO_ROOT}/Resources/outerctl.cpp" \
        -lsqlite3 -framework CoreFoundation -o "${root}/bin/outerctl"
    chmod 0755 "${root}/bin/outerctl"
    tar --format ustar --no-xattrs -C "${STAGING_ROOT}/home-screen-macos" -czf "${OUTPUT_ROOT}/latest/home-screen-macos.tar.gz" HomeScreen
}

stage_home_screen aarch64
stage_home_screen x86_64
stage_home_screen_macos

ASSET_VERSION="$(date -u +%Y%m%d%H%M%S)"
printf '%s\n' "${HOME_SCREEN_VERSION}" > "${OUTPUT_ROOT}/latest/version.txt"
cat > "${OUTPUT_ROOT}/latest/install.sh" <<'INSTALL_SH'
#!/bin/sh
set -eu

command="${1:-install}"
case "$command" in
    install|update|uninstall) ;;
    *) echo "Unsupported Home Screen installer command: $command" >&2; exit 2 ;;
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
        echo "curl or wget is required to install Home Screen." >&2
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
    outerwebapps_home="${OUTERWEBAPPS_HOME:-$HOME/Library/Application Support/outerwebapps}"
    install_root="$outerwebapps_home/home-screen"
    outerctl_path="$outerwebapps_home/bin/outerctl"
    launch_agent_dir="$HOME/Library/LaunchAgents"
    plist_path="$launch_agent_dir/dev.outergroup.HomeScreen.plist"
    socket_path="$(getconf DARWIN_USER_TEMP_DIR)dev.outergroup.HomeScreen"
    log_dir="$HOME/Library/Logs/dev.outergroup.HomeScreen"
    log_path="$log_dir/output.log"
    service_id="dev.outergroup.HomeScreen"
    display_name="Home Screen"

    unload_home_screen() {
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

    bootstrap_home_screen() {
        if launchctl bootstrap "gui/$(id -u)" "$plist_path"; then
            return 0
        fi
        first_error="$(launchctl bootstrap "gui/$(id -u)" "$plist_path" 2>&1 >/dev/null || true)"
        unload_home_screen
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
        unload_home_screen
        rm -f "$plist_path" "$socket_path"
        if [ -x "$outerctl_path" ]; then
            OUTERWEBAPPS_HOME="$outerwebapps_home" "$outerctl_path" app clear --backend "$service_id" >/dev/null 2>&1 || true
            OUTERWEBAPPS_HOME="$outerwebapps_home" "$outerctl_path" log clear --backend "$service_id" >/dev/null 2>&1 || true
            OUTERWEBAPPS_HOME="$outerwebapps_home" "$outerctl_path" launchd clear --backend "$service_id" >/dev/null 2>&1 || true
            OUTERWEBAPPS_HOME="$outerwebapps_home" "$outerctl_path" backend remove --backend "$service_id" >/dev/null 2>&1 || true
        fi
        rm -rf "$install_root"
        printf 'Home Screen has been uninstalled. Outer Loop will offer to install it again the next time it connects.\n'
        exit 0
    fi

    mkdir -p "$install_root" "$outerwebapps_home/bin" "$launch_agent_dir" "$log_dir"
    archive_path="$(mktemp)"
    download "${public_base_url%/}/latest/home-screen-macos.tar.gz?v=__ASSET_VERSION__" "$archive_path"
    rm -rf "$install_root"
    mkdir -p "$install_root" "$outerwebapps_home/bin" "$log_dir"
    tar -xzf "$archive_path" -C "$install_root" --strip-components=1
    rm -f "$archive_path"
    chmod 0755 "$install_root/Home Screen.app/Contents/MacOS/Home Screen"
    chmod 0755 "$install_root/bin/outerctl"
    install -m 0755 "$install_root/bin/outerctl" "$outerctl_path"
    printf '%s\n' "__HOME_SCREEN_VERSION__" > "$install_root/version"
    touch "$log_path"

    unload_home_screen
    rm -f "$socket_path"

    app_executable="$install_root/Home Screen.app/Contents/MacOS/Home Screen"
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
  <string>dev.outergroup.HomeScreen</string>
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

    OUTERWEBAPPS_HOME="$outerwebapps_home" "$outerctl_path" backend upsert --backend "$service_id" --name "$display_name" --icon-path "$install_root/app-icon.png"
    OUTERWEBAPPS_HOME="$outerwebapps_home" "$outerctl_path" launchd set --backend "$service_id" --plist "$plist_path" --owns-plist true
    OUTERWEBAPPS_HOME="$outerwebapps_home" "$outerctl_path" app clear --backend "$service_id"
    OUTERWEBAPPS_HOME="$outerwebapps_home" "$outerctl_path" app add --backend "$service_id" --socket-path "$socket_path" --name "$display_name" --url "$socket_path" --icon-path "$install_root/app-icon.png"
    OUTERWEBAPPS_HOME="$outerwebapps_home" "$outerctl_path" log clear --backend "$service_id"
    OUTERWEBAPPS_HOME="$outerwebapps_home" "$outerctl_path" log add --backend "$service_id" --path "$log_path"
    printf '[%s] %s Home Screen package %s from %s.\n' "$(timestamp)" "$command" "__HOME_SCREEN_VERSION__" "$public_base_url" >> "$log_path"

    bootstrap_home_screen
    if [ "$command" = "update" ]; then
        printf 'Home Screen updated to %s. The new version will run the next time Home Screen starts.\n' "__HOME_SCREEN_VERSION__"
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

    echo "Home Screen installed, but $socket_path did not appear." >&2
    exit 1
fi

runtime_dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
state_home="${XDG_STATE_HOME:-$HOME/.local/state}"
outerwebapps_home="${OUTERWEBAPPS_HOME:-$state_home/outerwebapps}"
install_root="$outerwebapps_home/home-screen"
outerctl_path="$outerwebapps_home/bin/outerctl"
unit_dir="$HOME/.config/systemd/user"
socket_path="$runtime_dir/dev.outergroup.HomeScreen"
log_dir="$install_root/logs"
log_path="$log_dir/HomeScreenBackend.log"
runner_path="$install_root/run-home-screen.sh"

if [ "$command" = "uninstall" ]; then
    systemctl --user disable dev.outergroup.HomeScreen.socket >/dev/null 2>&1 || true
    if command -v python3 >/dev/null 2>&1; then
        HOME_SCREEN_REGISTRY="$outerwebapps_home/registry.sqlite3" \
        HOME_SCREEN_SERVICE_ID="dev.outergroup.HomeScreen" \
        python3 - <<'PY' || true
import os
import sqlite3

database_path = os.environ["HOME_SCREEN_REGISTRY"]
service_id = os.environ["HOME_SCREEN_SERVICE_ID"]
try:
    database = sqlite3.connect(database_path)
except sqlite3.Error:
    raise SystemExit(0)
with database:
    for table in ("frontends", "log_files", "systemd_backends", "backends"):
        try:
            database.execute(f"DELETE FROM {table} WHERE service_id = ?", (service_id,))
        except sqlite3.Error:
            pass
database.close()
PY
    fi
    cleanup_script="$(mktemp)"
    cat > "$cleanup_script" <<EOF
#!/bin/sh
sleep 0.25
systemctl --user stop dev.outergroup.HomeScreen.socket dev.outergroup.HomeScreen.service >/dev/null 2>&1 || true
rm -f "$unit_dir/dev.outergroup.HomeScreen.service" "$unit_dir/dev.outergroup.HomeScreen.socket" "$socket_path"
systemctl --user daemon-reload >/dev/null 2>&1 || true
rm -rf "$install_root"
rm -f "$cleanup_script"
EOF
    chmod 0755 "$cleanup_script"
    if command -v systemd-run >/dev/null 2>&1; then
        systemd-run --user --unit=dev.outergroup.HomeScreen-uninstall --collect "$cleanup_script" >/dev/null 2>&1 || (nohup "$cleanup_script" >/dev/null 2>&1 &)
    else
        nohup "$cleanup_script" >/dev/null 2>&1 &
    fi
    printf 'Home Screen has been uninstalled. Outer Loop will offer to install it again the next time it connects.\n'
    exit 0
fi

mkdir -p "$install_root" "$outerwebapps_home/bin" "$unit_dir" "$log_dir"
archive_path="$(mktemp)"
download "${public_base_url%/}/latest/home-screen-${arch}.tar.gz?v=__ASSET_VERSION__" "$archive_path"
tar -xzf "$archive_path" -C "$install_root" --strip-components=1
rm -f "$archive_path"
chmod 0755 "$install_root/HomeScreenBackend"
chmod 0755 "$install_root/bin/outerctl"
install -m 0755 "$install_root/bin/outerctl" "$outerctl_path"
printf '%s\n' "__HOME_SCREEN_VERSION__" > "$install_root/version"
touch "$log_path"

cat > "$runner_path" <<EOF
#!/bin/sh
exec "$install_root/HomeScreenBackend" --socket-path "\${1:-$socket_path}" --bundles-dir "$install_root/bundles" --bundled-apps-dir "$install_root/bundled-apps" --app-base-url "$app_base_url" --public-base-url "$public_base_url" >> "$log_path" 2>&1
EOF
chmod 0755 "$runner_path"

cat > "$unit_dir/dev.outergroup.HomeScreen.service" <<EOF
[Unit]
Description=Outer Group Home Screen

[Service]
Environment=OUTERWEBAPPS_HOME=$outerwebapps_home
ExecStart=$runner_path %t/dev.outergroup.HomeScreen
Restart=no
EOF

cat > "$unit_dir/dev.outergroup.HomeScreen.socket" <<EOF
[Unit]
Description=Outer Group Home Screen Socket

[Socket]
ListenStream=%t/dev.outergroup.HomeScreen
SocketMode=0600

[Install]
WantedBy=sockets.target
EOF

printf '[%s] %s Home Screen package %s from %s.\n' "$(timestamp)" "$command" "__HOME_SCREEN_VERSION__" "$public_base_url" >> "$log_path"

if command -v python3 >/dev/null 2>&1; then
    HOME_SCREEN_REGISTRY="$outerwebapps_home/registry.sqlite3" \
    HOME_SCREEN_SERVICE_ID="dev.outergroup.HomeScreen" \
    HOME_SCREEN_DISPLAY_NAME="Home Screen" \
    HOME_SCREEN_UNIT_NAME="dev.outergroup.HomeScreen.service" \
    HOME_SCREEN_SOCKET_PATH="$socket_path" \
    HOME_SCREEN_LOG_PATH="$log_path" \
    HOME_SCREEN_ICON_PATH="$install_root/app-icon.png" \
    python3 - <<'PY'
import base64
import os
import sqlite3

database_path = os.environ["HOME_SCREEN_REGISTRY"]
service_id = os.environ["HOME_SCREEN_SERVICE_ID"]
display_name = os.environ["HOME_SCREEN_DISPLAY_NAME"]
unit_name = os.environ["HOME_SCREEN_UNIT_NAME"]
socket_path = os.environ["HOME_SCREEN_SOCKET_PATH"]
log_path = os.environ["HOME_SCREEN_LOG_PATH"]
icon_path = os.environ["HOME_SCREEN_ICON_PATH"]

def registry_icon_value(path):
    try:
        with open(path, "rb") as file:
            data = file.read(1024 * 1024 + 1)
    except OSError:
        return path
    if not data or len(data) > 1024 * 1024:
        return path
    return "data:image/png;base64," + base64.b64encode(data).decode("ascii")

os.makedirs(os.path.dirname(database_path), exist_ok=True)
database = sqlite3.connect(database_path)
database.executescript("""
CREATE TABLE IF NOT EXISTS backends (
service_id TEXT PRIMARY KEY,
display_name TEXT NOT NULL DEFAULT '',
icon TEXT,
service_unit TEXT
);
CREATE TABLE IF NOT EXISTS frontends (
url TEXT PRIMARY KEY,
service_id TEXT,
display_name TEXT NOT NULL DEFAULT '',
port INTEGER NOT NULL DEFAULT 0,
socket_path TEXT NOT NULL DEFAULT '',
icon TEXT,
is_home_screen INTEGER NOT NULL DEFAULT 0,
list TEXT
);
CREATE INDEX IF NOT EXISTS frontends_service_id_idx ON frontends(service_id);
CREATE TABLE IF NOT EXISTS log_files (
path TEXT PRIMARY KEY,
service_id TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS log_files_service_id_idx ON log_files(service_id);
CREATE TABLE IF NOT EXISTS systemd_backends (
service_id TEXT PRIMARY KEY,
unit_name TEXT NOT NULL,
scope TEXT NOT NULL DEFAULT 'user'
);
""")

def ensure_column(table, name, definition):
    columns = {row[1] for row in database.execute(f"PRAGMA table_info({table})")}
    if name not in columns:
        database.execute(f"ALTER TABLE {table} ADD COLUMN {name} {definition}")

ensure_column("backends", "icon", "TEXT")
ensure_column("backends", "service_unit", "TEXT")
ensure_column("frontends", "display_name", "TEXT NOT NULL DEFAULT ''")
ensure_column("frontends", "icon", "TEXT")
ensure_column("frontends", "is_home_screen", "INTEGER NOT NULL DEFAULT 0")
ensure_column("frontends", "list", "TEXT")
frontend_columns = {row[1] for row in database.execute("PRAGMA table_info(frontends)")}
if "name" in frontend_columns:
    database.execute("UPDATE frontends SET display_name = name WHERE display_name = ''")
    database.executescript("""
DROP INDEX IF EXISTS frontends_service_id_idx;
CREATE TABLE frontends_new (
url TEXT PRIMARY KEY,
service_id TEXT,
display_name TEXT NOT NULL DEFAULT '',
port INTEGER NOT NULL DEFAULT 0,
socket_path TEXT NOT NULL DEFAULT '',
icon TEXT,
is_home_screen INTEGER NOT NULL DEFAULT 0,
list TEXT
);
INSERT OR REPLACE INTO frontends_new(url, service_id, display_name, port, socket_path, icon, is_home_screen, list)
SELECT url, service_id, COALESCE(NULLIF(display_name, ''), name, ''), COALESCE(port, 0), COALESCE(socket_path, ''), icon, COALESCE(is_home_screen, 0), list FROM frontends;
DROP TABLE frontends;
ALTER TABLE frontends_new RENAME TO frontends;
CREATE INDEX IF NOT EXISTS frontends_service_id_idx ON frontends(service_id);
""")

icon_value = registry_icon_value(icon_path)
with database:
    database.execute(
        "INSERT INTO backends(service_id, display_name, icon, service_unit) VALUES(?, ?, ?, ?) "
        "ON CONFLICT(service_id) DO UPDATE SET display_name=excluded.display_name, icon=excluded.icon, service_unit=excluded.service_unit",
        (service_id, display_name, icon_value, unit_name),
    )
    database.execute(
        "INSERT INTO systemd_backends(service_id, unit_name, scope) VALUES(?, ?, 'user') "
        "ON CONFLICT(service_id) DO UPDATE SET unit_name=excluded.unit_name, scope=excluded.scope",
        (service_id, unit_name),
    )
    database.execute("DELETE FROM frontends WHERE service_id = ?", (service_id,))
    database.execute(
        "INSERT INTO frontends(url, service_id, display_name, port, socket_path, icon, is_home_screen) VALUES(?, ?, ?, 0, ?, ?, 1)",
        (socket_path, service_id, display_name, socket_path, icon_value),
    )
    database.execute("DELETE FROM log_files WHERE service_id = ?", (service_id,))
    database.execute("INSERT INTO log_files(path, service_id) VALUES(?, ?)", (log_path, service_id))
database.close()
PY
else
    printf '[%s] python3 is unavailable; skipped registry update for Home Screen logs.\n' "$(timestamp)" >> "$log_path"
fi

if [ "$command" = "install" ]; then
    systemctl --user stop dev.outergroup.HomeScreen.service >/dev/null 2>&1 || true
fi
systemctl --user daemon-reload
systemctl --user enable --now dev.outergroup.HomeScreen.socket

if [ "$command" = "update" ]; then
    printf 'Home Screen updated to %s. The new version will run the next time Home Screen starts.\n' "__HOME_SCREEN_VERSION__"
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

echo "Home Screen installed, but $socket_path did not appear." >&2
exit 1
INSTALL_SH
python3 - "${OUTPUT_ROOT}/latest/install.sh" "${ASSET_VERSION}" "${PUBLIC_BASE_URL}" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text()
text = text.replace("__ASSET_VERSION__", sys.argv[2])
text = text.replace("__PUBLIC_BASE_URL__", sys.argv[3])
text = text.replace("__HOME_SCREEN_VERSION__", "__HOME_SCREEN_VERSION_REPLACE__")
path.write_text(text)
PY
python3 - "${OUTPUT_ROOT}/latest/install.sh" "${HOME_SCREEN_VERSION}" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text()
text = text.replace("__HOME_SCREEN_VERSION_REPLACE__", sys.argv[2])
path.write_text(text)
PY
chmod 0755 "${OUTPUT_ROOT}/latest/install.sh"

echo "Packaged assets under ${OUTPUT_ROOT}"
