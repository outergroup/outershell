#!/bin/bash
set -euo pipefail
if [[ $EUID == 0 ]]; then echo 'Run this installer as the Linux desktop user.' >&2; exit 1; fi
root="$(cd "$(dirname "$0")/.." && pwd)"
files="${1:-$root/../Files}"
source="$root/build/wsl"
state="${XDG_STATE_HOME:-$HOME/.local/state}/outershell"
install="$state/wsl"
runtime="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export XDG_RUNTIME_DIR="$runtime"
export DBUS_SESSION_BUS_ADDRESS="unix:path=$runtime/bus"
units="$HOME/.config/systemd/user"
mkdir -p "$install" "$units" "$HOME/.config/outerssh"
chmod 700 "$state" "$install" "$HOME/.config/outerssh"
for unit in org.outershell.Files org.outershell.OuterShell outershelld; do
    systemctl --user stop "$unit.socket" "$unit.service" 2>/dev/null || true
done
for exe in outershelld OuterShellBackend outerctl FilesBackend; do install -m 755 "$source/$exe" "$install/$exe"; done
mkdir -p "$install/web" "$install/files-web"
cp -R "$root/Resources/OuterShellWeb/." "$install/web/"
cp -R "$files/Resources/FilesWeb/." "$install/files-web/"
install -m 644 "$files/app-icon.png" "$install/files-icon.png"
for unit in outershelld org.outershell.OuterShell org.outershell.Files; do
    socket="$runtime/$unit"
    descriptor=http
    if [[ $unit == outershelld ]]; then socket="$runtime/outershelld-api"; descriptor=api; fi
    cat > "$units/$unit.socket" <<UNIT
[Unit]
Description=$unit socket
[Socket]
ListenStream=$socket
FileDescriptorName=$descriptor
SocketMode=0600
Service=$unit.service
[Install]
WantedBy=sockets.target
UNIT
done
cat > "$units/outershelld.service" <<UNIT
[Unit]
Description=Outer Shell registry
[Service]
Environment="OUTERSHELL_HOME=$state"
Environment=OUTER_SHELL_SERVICE_MANAGER=systemd
ExecStart="$install/outershelld" --api-socket-path "$runtime/outershelld-api"
Restart=on-failure
UNIT
cat > "$units/org.outershell.OuterShell.service" <<UNIT
[Unit]
Description=Outer Shell
Requires=outershelld.socket
After=outershelld.socket
[Service]
Environment="OUTERSHELL_HOME=$state"
Environment=OUTER_SHELL_SERVICE_MANAGER=systemd
ExecStart="$install/OuterShellBackend" --socket-path "$runtime/org.outershell.OuterShell" --api-socket-path "$runtime/outershelld-api" --web-root "$install/web" --web-only
Restart=on-failure
UNIT
cat > "$units/org.outershell.Files.service" <<UNIT
[Unit]
Description=Files
Requires=outershelld.socket
After=outershelld.socket
[Service]
ExecStart="$install/FilesBackend" --socket-path "$runtime/org.outershell.Files" --api-socket-path "$runtime/outershelld-api" --web-root "$install/files-web" --web-only --icon-file "$install/files-icon.png"
Restart=on-failure
UNIT
systemctl --user daemon-reload
systemctl --user enable --now outershelld.socket org.outershell.OuterShell.socket org.outershell.Files.socket
for app in OuterShell Files; do
    name="$app"
    if [[ $app == OuterShell ]]; then name='Outer Shell'; fi
    "$install/outerctl" backend upsert --backend "org.outershell.$app" --name "$name" --unit "org.outershell.$app.service" --outershell-owns true
    icon=()
    if [[ $app == Files ]]; then icon=(--icon-path "$install/files-icon.png"); fi
    "$install/outerctl" app upsert --backend "org.outershell.$app" --name "$name" --socket-path "$runtime/org.outershell.$app" --url / "${icon[@]}"
    path="$runtime/org.outershell.$app"
    touch "$HOME/.config/outerssh/http-unix.allow"
    if ! grep -Fxq -- "$path" "$HOME/.config/outerssh/http-unix.allow" && ! grep -Fxq -- "%t/org.outershell.$app" "$HOME/.config/outerssh/http-unix.allow"; then printf '%s\n' "$path" >> "$HOME/.config/outerssh/http-unix.allow"; fi
done
chmod 600 "$HOME/.config/outerssh/http-unix.allow"
printf 'Installed Outer Shell and Files for %s.\n' "$(id -un)"
