#!/bin/bash
set -euo pipefail
if [[ $EUID == 0 ]]; then echo 'Run this installer as the Linux desktop user.' >&2; exit 1; fi
if [[ $# -gt 2 ]]; then echo "Usage: $0 [INSTALLER [ARCHIVE]]" >&2; exit 1; fi
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
systemctl --user show-environment >/dev/null
installer="${1:-}"
archive="${2:-}"
temporary=""
trap '[[ -z "$temporary" ]] || rm -f "$temporary"' EXIT
if [[ -z "$installer" ]]; then
    temporary="$(mktemp)"
    curl --fail --location --silent --show-error \
        "${OUTER_SHELL_PUBLIC_BASE_URL:-https://outershell.org/outer-shell}/latest/install.sh" \
        --output "$temporary"
    installer="$temporary"
fi
if [[ ! -f "$installer" ]]; then echo "Installer not found: $installer" >&2; exit 1; fi
if [[ -n "$archive" ]]; then
    if [[ ! -f "$archive" ]]; then echo "Archive not found: $archive" >&2; exit 1; fi
    export OUTERSHELL_INSTALL_ARCHIVE="$(cd "$(dirname "$archive")" && pwd)/$(basename "$archive")"
fi
OUTERSHELL_SERVICE_MANAGER=systemd bash "$installer" install
state="${OUTERSHELL_HOME:-${XDG_STATE_HOME:-$HOME/.local/state}/outershell}"
case "$(uname -m)" in
    x86_64|amd64) architecture=x86_64 ;;
    aarch64|arm64) architecture=aarch64 ;;
    *) echo 'Unsupported WSL architecture.' >&2; exit 1 ;;
esac
bridge="$state/outershelld/container-bootstrap/bin/musl/$architecture/outer-socket-bridge"
if ! cmp -s "$bridge" /usr/local/bin/outer-socket-bridge; then
    sudo install -o root -g root -m 755 "$bridge" /usr/local/bin/outer-socket-bridge
fi
if [[ "$(loginctl show-user "$(id -un)" --property=Linger --value)" != yes ]]; then
    sudo loginctl enable-linger "$(id -un)"
fi
printf 'Installed standard Linux Outer Shell for %s. Install bundled apps from Add more….\n' "$(id -un)"
