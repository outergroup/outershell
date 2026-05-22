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
require_file "${RUN_ROOT}/bundles/BackendsContent.bundle.macos-arm.aar"
require_file "${RUN_ROOT}/bundles/BackendsContent.bundle.macos-x86.aar"
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
    mkdir -p "${root}/bundles"
    install -m 0755 "${PACKAGE_ROOT}/RemoteLinuxBinaries/${arch}/HomeScreenBackend" "${root}/HomeScreenBackend"
    install -m 0644 "${REPO_ROOT}/app-icon.png" "${root}/app-icon.png"
    install -m 0644 "${RUN_ROOT}/bundles/BackendsContent.bundle.macos-arm.aar" "${root}/bundles/BackendsContent.bundle.macos-arm.aar"
    install -m 0644 "${RUN_ROOT}/bundles/BackendsContent.bundle.macos-x86.aar" "${root}/bundles/BackendsContent.bundle.macos-x86.aar"
    tar --format ustar --no-xattrs -C "${STAGING_ROOT}/home-screen-${arch}" -czf "${OUTPUT_ROOT}/latest/home-screen-${arch}.tar.gz" HomeScreen
}

stage_home_screen aarch64
stage_home_screen x86_64

ASSET_VERSION="$(date -u +%Y%m%d%H%M%S)"
cat > "${OUTPUT_ROOT}/latest/install.sh" <<'INSTALL_SH'
#!/bin/sh
set -eu

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

runtime_dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
install_root="$HOME/.outerloop/home-screen"
unit_dir="$HOME/.config/systemd/user"
socket_path="$runtime_dir/dev.outergroup.HomeScreen"
log_dir="$install_root/logs"
log_path="$log_dir/HomeScreenBackend.log"
runner_path="$install_root/run-home-screen.sh"
archive_path="$(mktemp)"

mkdir -p "$install_root" "$unit_dir" "$log_dir"
public_base_url="__PUBLIC_BASE_URL__"
app_base_url="${public_base_url%/}/apps"
download "${public_base_url%/}/latest/home-screen-${arch}.tar.gz?v=__ASSET_VERSION__" "$archive_path"
tar -xzf "$archive_path" -C "$install_root" --strip-components=1
rm -f "$archive_path"
chmod 0755 "$install_root/HomeScreenBackend"
touch "$log_path"

cat > "$runner_path" <<EOF
#!/bin/sh
exec "$install_root/HomeScreenBackend" --socket-path "\${1:-$socket_path}" --bundles-dir "$install_root/bundles" --bundled-apps-dir "$install_root/bundled-apps" --app-base-url "$app_base_url" >> "$log_path" 2>&1
EOF
chmod 0755 "$runner_path"

cat > "$unit_dir/dev.outergroup.HomeScreen.service" <<EOF
[Unit]
Description=Outer Group Home Screen

[Service]
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

printf '[%s] Installed Home Screen package from %s.\n' "$(date -Is)" "$public_base_url" >> "$log_path"

if command -v python3 >/dev/null 2>&1; then
    HOME_SCREEN_REGISTRY="$HOME/.outeragent/registry.sqlite3" \
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
name TEXT NOT NULL,
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
ensure_column("frontends", "icon", "TEXT")
ensure_column("frontends", "is_home_screen", "INTEGER NOT NULL DEFAULT 0")
ensure_column("frontends", "list", "TEXT")

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
        "INSERT INTO frontends(url, service_id, name, port, socket_path, icon, is_home_screen) VALUES(?, ?, ?, 0, ?, ?, 1)",
        (socket_path, service_id, display_name, socket_path, icon_value),
    )
    database.execute("DELETE FROM log_files WHERE service_id = ?", (service_id,))
    database.execute("INSERT INTO log_files(path, service_id) VALUES(?, ?)", (log_path, service_id))
database.close()
PY
else
    printf '[%s] python3 is unavailable; skipped registry update for Home Screen logs.\n' "$(date -Is)" >> "$log_path"
fi

systemctl --user stop dev.outergroup.HomeScreen.service >/dev/null 2>&1 || true
systemctl --user daemon-reload
systemctl --user enable --now dev.outergroup.HomeScreen.socket

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
path.write_text(text)
PY
chmod 0755 "${OUTPUT_ROOT}/latest/install.sh"

echo "Packaged assets under ${OUTPUT_ROOT}"
