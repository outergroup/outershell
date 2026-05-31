# Custom Backend Integration

This guide documents the raw pieces needed to make an existing web backend show
up in Outer Shell. It intentionally avoids recipes and bundled-app machinery.
The goal is to make a normal Linux service, register it with `outerctl`, and let
Outer Shell display, launch, stop, and tail logs for it.

The example uses TensorBoard with placeholder paths. The same pattern should
work for other local web apps that already expose HTTP on a loopback port, such
as custom dashboards, notebook servers, or development tools.

## Terms

- **Backend**: the long-running service Outer Shell manages, identified by a
  stable service id such as `com.example.TensorBoard`.
- **Frontend**: one user-facing entry point for the backend, usually a URL on a
  loopback port or a Unix socket.
- **Registry**: metadata owned by `outershelld` and updated with `outerctl`.
- **User install**: a systemd user unit and the per-user registry under
  `${XDG_STATE_HOME:-~/.local/state}/outershell`.
- **Root install**: a system systemd unit and the system registry under
  `/var/lib/outershell`.

## Where `outerctl` Lives

The public Outer Shell installer installs `outerctl` here on Linux:

```bash
OUTERCTL="${XDG_STATE_HOME:-$HOME/.local/state}/outershell/bin/outerctl"
```

Run `outerctl` as the same user that owns the registry you want to update. For a
root/system install, run it with `sudo` and set `OUTERSHELL_HOME` to the
system registry root:

```bash
sudo OUTERSHELL_HOME=/var/lib/outershell \
  /home/alice/.local/state/outershell/bin/outerctl ...
```

## Example Layout

For a user-owned TensorBoard service:

```text
~/.local/share/example-tensorboard/
  run-tensorboard.sh
  icon.png
  runs/
  .venv/
```

The service id should be globally stable. Reverse-DNS style identifiers are a
good default:

```bash
SERVICE_ID="com.example.TensorBoard"
DISPLAY_NAME="TensorBoard"
APP_ROOT="$HOME/.local/share/example-tensorboard"
PORT="6006"
LOG_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/outershell/logs/$SERVICE_ID"
```

## Create the Python Environment

Use whatever environment manager is normal for the backend. A `uv`-managed venv
is a reasonable modern default:

```bash
mkdir -p "$APP_ROOT"
cd "$APP_ROOT"
uv venv .venv
uv pip install tensorboard
mkdir -p runs "$LOG_DIR"
```

## Create the Runner

Use a small wrapper instead of putting a long command directly in the systemd
unit. This keeps environment setup, paths, and future runtime announcements in
one place.

```bash
cat > "$APP_ROOT/run-tensorboard.sh" <<'SH'
#!/bin/sh
set -eu

APP_ROOT="${APP_ROOT:-$HOME/.local/share/example-tensorboard}"
PORT="${PORT:-6006}"

exec "$APP_ROOT/.venv/bin/tensorboard" \
  --logdir "$APP_ROOT/runs" \
  --host 127.0.0.1 \
  --port "$PORT"
SH
chmod 0755 "$APP_ROOT/run-tensorboard.sh"
```

Binding to `127.0.0.1` is usually right for Outer Loop/Outer Shell usage. Outer
Loop reaches the service through SSH forwarding; the backend does not need to
listen on a public interface.

## Create the User systemd Unit

Create `~/.config/systemd/user/com.example.TensorBoard.service`:

```ini
[Unit]
Description=TensorBoard
After=network.target

[Service]
Type=simple
Environment=APP_ROOT=%h/.local/share/example-tensorboard
Environment=PORT=6006
ExecStart=%h/.local/share/example-tensorboard/run-tensorboard.sh
Restart=on-failure
WorkingDirectory=%h/.local/share/example-tensorboard
StandardOutput=append:%h/.local/state/outershell/logs/com.example.TensorBoard/tensorboard.log
StandardError=append:%h/.local/state/outershell/logs/com.example.TensorBoard/tensorboard.log

[Install]
WantedBy=default.target
```

Then enable it. Enabling records that the app is installable/runnable; starting
it immediately is optional.

```bash
systemctl --user daemon-reload
systemctl --user enable com.example.TensorBoard.service
```

For initial testing:

```bash
systemctl --user start com.example.TensorBoard.service
systemctl --user status com.example.TensorBoard.service
```

## Register with Outer Shell

Registration should happen at install time, after the unit and icon path exist.
These commands are idempotent enough for installer/update scripts to rerun.

```bash
"$OUTERCTL" backend upsert \
  --backend "$SERVICE_ID" \
  --name "$DISPLAY_NAME" \
  --icon-path "$APP_ROOT/icon.png"

"$OUTERCTL" systemd set \
  --backend "$SERVICE_ID" \
  --unit "$SERVICE_ID.service"

"$OUTERCTL" app clear \
  --backend "$SERVICE_ID"

"$OUTERCTL" app add \
  --backend "$SERVICE_ID" \
  --port "$PORT" \
  --name "$DISPLAY_NAME" \
  --url "/" \
  --icon-path "$APP_ROOT/icon.png" \
  --list "Machine Learning"

"$OUTERCTL" log clear \
  --backend "$SERVICE_ID"

"$OUTERCTL" log add \
  --backend "$SERVICE_ID" \
  --path "$LOG_DIR/tensorboard.log"
```

What each row does:

- `backend upsert` creates the app identity Outer Shell displays.
- `systemd set` teaches Outer Shell how to start and stop the backend.
- `app add` creates the Apps-page entry point. Use `--port` for TCP HTTP
  backends and `--socket-path` for Unix-socket HTTP backends.
- `log add` lets Outer Shell show the service log viewer.

After registration, refresh Outer Shell. TensorBoard should appear on the Apps
page. If the service is stopped, clicking it should ask Outer Shell to start the
systemd unit and then open the frontend.

## Runtime Announcements

Fixed-port apps can register their frontend once at install time. Apps whose URL
changes at runtime should update their frontend when they start.

Examples that may need runtime updates:

- A Jupyter server that prints a new tokenized URL on each run.
- A backend that chooses a free port dynamically.
- A backend that creates different frontend entries based on local state.

For those apps, keep the install-time `backend upsert`, `systemd set`, and
`log add` commands, but have the runner or backend process call
`outerctl app add` after it knows the final port and URL. If the app can expose
a stable port or stable Unix socket, prefer that; it makes start/open behavior
simpler.

## Verify Registration

After registration, these commands should show the backend, systemd unit,
frontend, and log file:

```bash
"$OUTERCTL" backend list --backend "$SERVICE_ID"
"$OUTERCTL" systemd list --backend "$SERVICE_ID"
"$OUTERCTL" app list --backend "$SERVICE_ID"
"$OUTERCTL" log list --backend "$SERVICE_ID"
```

If the frontend row is present but clicking the app fails, first verify the
service outside Outer Shell:

```bash
systemctl --user status "$SERVICE_ID.service"
curl -f "http://127.0.0.1:$PORT/"
```

## Unix Socket HTTP Backends

If your backend can listen on a Unix socket instead of a TCP port, register it
like this:

```bash
SOCKET_PATH="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/com.example.MyApp"

"$OUTERCTL" app add \
  --backend com.example.MyApp \
  --socket-path "$SOCKET_PATH" \
  --name "My App" \
  --url "/" \
  --icon-path "$APP_ROOT/icon.png"
```

For systemd socket activation, create a `.socket` unit that owns the socket and
a matching `.service` unit that receives it. Use `SocketMode=0600` for user
units. For root/system units, avoid making root-owned sockets writable by normal
users; root-only HTTP backends should be reached through Outer Loop's root
socket bridge.

## Root/System Units

Root installs are useful when the backend genuinely needs root privileges. They
should be explicit and conservative.

Example system paths:

```text
/opt/example-tensorboard/
  run-tensorboard.sh
  icon.png
  runs/
  .venv/
/var/log/outergroup/com.example.TensorBoard/tensorboard.log
/etc/systemd/system/com.example.TensorBoard.service
```

After creating the system unit:

```bash
sudo systemctl daemon-reload
sudo systemctl enable com.example.TensorBoard.service
```

Register the backend in the system registry:

```bash
sudo OUTERSHELL_HOME=/var/lib/outershell "$OUTERCTL" backend upsert \
  --backend "$SERVICE_ID" \
  --name "$DISPLAY_NAME" \
  --icon-path /opt/example-tensorboard/icon.png

sudo OUTERSHELL_HOME=/var/lib/outershell "$OUTERCTL" systemd set \
  --backend "$SERVICE_ID" \
  --unit "$SERVICE_ID.service"

sudo OUTERSHELL_HOME=/var/lib/outershell "$OUTERCTL" app clear \
  --backend "$SERVICE_ID"

sudo OUTERSHELL_HOME=/var/lib/outershell "$OUTERCTL" app add \
  --backend "$SERVICE_ID" \
  --port "$PORT" \
  --name "$DISPLAY_NAME" \
  --url "/" \
  --icon-path /opt/example-tensorboard/icon.png

sudo OUTERSHELL_HOME=/var/lib/outershell "$OUTERCTL" log clear \
  --backend "$SERVICE_ID"

sudo OUTERSHELL_HOME=/var/lib/outershell "$OUTERCTL" log add \
  --backend "$SERVICE_ID" \
  --path /var/log/outergroup/com.example.TensorBoard/tensorboard.log
```

A normal user Outer Shell can read the system registry and show system apps. If
opening a root-only Unix socket is required, Outer Loop will need a root socket
bridge or a direct root SSH session. For fixed loopback TCP ports, decide
carefully whether the backend should be reachable by normal users on the host.

## Updating an Integration

When an installer or update script reruns:

```bash
systemctl --user daemon-reload
"$OUTERCTL" backend upsert --backend "$SERVICE_ID" --name "$DISPLAY_NAME" --icon-path "$APP_ROOT/icon.png"
"$OUTERCTL" systemd set --backend "$SERVICE_ID" --unit "$SERVICE_ID.service"
"$OUTERCTL" app clear --backend "$SERVICE_ID"
"$OUTERCTL" app add --backend "$SERVICE_ID" --port "$PORT" --name "$DISPLAY_NAME" --url "/" --icon-path "$APP_ROOT/icon.png"
"$OUTERCTL" log clear --backend "$SERVICE_ID"
"$OUTERCTL" log add --backend "$SERVICE_ID" --path "$LOG_DIR/tensorboard.log"
```

Clear-and-add is currently the simplest way to keep frontend and log rows in
sync. This is a possible API improvement area.

## Uninstalling

For a user install:

```bash
systemctl --user disable --now "$SERVICE_ID.service" || true
rm -f "$HOME/.config/systemd/user/$SERVICE_ID.service"
systemctl --user daemon-reload

"$OUTERCTL" app clear --backend "$SERVICE_ID"
"$OUTERCTL" log clear --backend "$SERVICE_ID"
"$OUTERCTL" systemd clear --backend "$SERVICE_ID"
"$OUTERCTL" backend remove --backend "$SERVICE_ID"
```

Then remove the app files if desired:

```bash
rm -rf "$APP_ROOT"
```

For a root install, run the equivalent cleanup with `sudo systemctl` and
`sudo OUTERSHELL_HOME=/var/lib/outershell "$OUTERCTL" ...`.

## Full User Install Script Sketch

This is not a polished installer, but it shows the whole flow in one place:

```bash
#!/bin/sh
set -eu

SERVICE_ID="com.example.TensorBoard"
DISPLAY_NAME="TensorBoard"
APP_ROOT="$HOME/.local/share/example-tensorboard"
PORT="6006"
LOG_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/outershell/logs/$SERVICE_ID"
OUTERCTL="${XDG_STATE_HOME:-$HOME/.local/state}/outershell/bin/outerctl"
UNIT_DIR="$HOME/.config/systemd/user"
UNIT_PATH="$UNIT_DIR/$SERVICE_ID.service"

mkdir -p "$APP_ROOT" "$APP_ROOT/runs" "$LOG_DIR" "$UNIT_DIR"

cd "$APP_ROOT"
uv venv .venv
uv pip install tensorboard

cat > "$APP_ROOT/run-tensorboard.sh" <<'SH'
#!/bin/sh
set -eu
APP_ROOT="${APP_ROOT:-$HOME/.local/share/example-tensorboard}"
PORT="${PORT:-6006}"
exec "$APP_ROOT/.venv/bin/tensorboard" --logdir "$APP_ROOT/runs" --host 127.0.0.1 --port "$PORT"
SH
chmod 0755 "$APP_ROOT/run-tensorboard.sh"

cat > "$UNIT_PATH" <<EOF
[Unit]
Description=$DISPLAY_NAME
After=network.target

[Service]
Type=simple
Environment=APP_ROOT=$APP_ROOT
Environment=PORT=$PORT
ExecStart=$APP_ROOT/run-tensorboard.sh
Restart=on-failure
WorkingDirectory=$APP_ROOT
StandardOutput=append:$LOG_DIR/tensorboard.log
StandardError=append:$LOG_DIR/tensorboard.log

[Install]
WantedBy=default.target
EOF

systemctl --user daemon-reload
systemctl --user enable "$SERVICE_ID.service"

"$OUTERCTL" backend upsert --backend "$SERVICE_ID" --name "$DISPLAY_NAME" --icon-path "$APP_ROOT/icon.png"
"$OUTERCTL" systemd set --backend "$SERVICE_ID" --unit "$SERVICE_ID.service"
"$OUTERCTL" app clear --backend "$SERVICE_ID"
"$OUTERCTL" app add --backend "$SERVICE_ID" --port "$PORT" --name "$DISPLAY_NAME" --url "/" --icon-path "$APP_ROOT/icon.png" --list "Machine Learning"
"$OUTERCTL" log clear --backend "$SERVICE_ID"
"$OUTERCTL" log add --backend "$SERVICE_ID" --path "$LOG_DIR/tensorboard.log"
```

## Flow Issues To Consider Improving

- `outerctl app clear` plus `outerctl app add` is verbose for a single frontend.
  A first-class `app upsert` command would make installer scripts less fragile.
- The command names still say `app` for frontend rows. That is visible in
  third-party integration docs and may be confusing.
- Runtime URL updates are possible, but there is no explicit "frontend is
  running at this transient URL" command in the public CLI yet.
- Root registration requires setting `OUTERSHELL_HOME=/var/lib/outershell`.
  A clearer `outerctl --system ...` mode may be easier to document.
- Socket-activated services need examples for both TCP and Unix sockets. The
  Unix-socket path is better aligned with Outer Shell security, but many
  existing tools only support TCP.
