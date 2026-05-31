# Custom Backend Integration

This guide shows the raw pieces needed to make an existing web backend appear in
Outer Shell. It avoids recipe and bundled-app machinery: create a normal
systemd unit, register it with `outerctl`, and let Outer Shell display, launch,
stop, and tail logs for it.

The example uses TensorBoard with placeholder paths. The same pattern works for
local dashboards, notebook servers, and development tools that expose HTTP on a
loopback port or Unix socket.

## Terms

- **Backend**: the managed service, identified by a stable id such as
  `com.example.TensorBoard`.
- **App**: one user-facing entry point for the backend. Apps have names, URLs,
  optional icons, and optional list placement.
- **Registry**: metadata owned by `outershelld` and updated with `outerctl`.
- **User install**: a systemd user unit plus the per-user registry under
  `${XDG_STATE_HOME:-~/.local/state}/outershell`.
- **Root install**: a system systemd unit plus the system registry under
  `/var/lib/outershell`.

## Where `outerctl` Lives

The public Outer Shell installer installs `outerctl` here on Linux:

```bash
OUTERCTL="${XDG_STATE_HOME:-$HOME/.local/state}/outershell/bin/outerctl"
```

Run `outerctl` as the same user that owns the registry you want to update. For a
root/system install, run it with `sudo` and set `OUTERSHELL_HOME`:

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

```bash
SERVICE_ID="com.example.TensorBoard"
DISPLAY_NAME="TensorBoard"
APP_ROOT="$HOME/.local/share/example-tensorboard"
PORT="6006"
LOG_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/outershell/logs/$SERVICE_ID"
```

## Create the Python Environment

Use whatever environment manager is normal for the backend. A `uv`-managed venv
is a reasonable default:

```bash
mkdir -p "$APP_ROOT" "$LOG_DIR"
cd "$APP_ROOT"
uv venv .venv
uv pip install tensorboard
mkdir -p runs
```

## Create the Runner

Use a small wrapper instead of putting a long command directly in the systemd
unit.

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

Then enable it. Starting it immediately is optional.

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

Registration should happen at install time, after the unit and log path exist.
Backends do not have icons; app entries do.

```bash
"$OUTERCTL" backend upsert \
  --backend "$SERVICE_ID" \
  --name "$DISPLAY_NAME" \
  --systemd-unit "$SERVICE_ID.service"

"$OUTERCTL" app add \
  --backend "$SERVICE_ID" \
  --frontend-id "$SERVICE_ID:main" \
  --port "$PORT" \
  --name "$DISPLAY_NAME" \
  --url "127.0.0.1:$PORT/" \
  --icon-path "$APP_ROOT/icon.png" \
  --list "Machine Learning"

"$OUTERCTL" log add \
  --backend "$SERVICE_ID" \
  --path "$LOG_DIR/tensorboard.log"
```

What each row does:

- `backend upsert` creates the managed service identity and records its systemd
  unit, so Outer Shell can start and stop it.
- `app add` creates the Apps-page entry point. Reuse the same `--frontend-id`
  for runtime endpoint updates.
- `log add` lets Outer Shell show the service log viewer.

After registration, refresh Outer Shell. TensorBoard should appear on the Apps
page. If the service is stopped, clicking it should ask Outer Shell to start the
systemd unit and then open the app.

## Runtime Announcements

The install-time app row is the stable entry point. Outer Shell decides whether
the app is running from the service manager and waits for the registered
port/socket to become reachable when starting an app.

If the endpoint changes at runtime, call `app add` with the same `--frontend-id`
after the backend knows the final port, socket, or URL:

```bash
"$OUTERCTL" app add \
  --backend "$SERVICE_ID" \
  --frontend-id "$SERVICE_ID:main" \
  --port "$PORT" \
  --name "$DISPLAY_NAME" \
  --url "127.0.0.1:$PORT/"
```

Examples that may need runtime updates:

- A Jupyter server that prints a new tokenized URL on each run.
- A backend that chooses a free port dynamically.
- A backend that creates different app entries based on local state.

Use `--port` for TCP HTTP backends and `--socket-path` for Unix-socket HTTP
backends. Scripts do not need to announce "running" or clear app state on exit.

## Verify Registration

After registration, these commands should show the backend, app, and log file:

```bash
"$OUTERCTL" backend list --backend "$SERVICE_ID"
"$OUTERCTL" app list --backend "$SERVICE_ID"
"$OUTERCTL" log list --backend "$SERVICE_ID"
```

If clicking the app fails, first verify the service outside Outer Shell:

```bash
systemctl --user status "$SERVICE_ID.service"
curl -I "http://127.0.0.1:$PORT/"
```

## Root Install

For a root-owned backend, install the payload and unit in system locations, then
run the same registration commands with `sudo`:

```bash
sudo systemctl daemon-reload
sudo systemctl enable com.example.TensorBoard.service

sudo OUTERSHELL_HOME=/var/lib/outershell "$OUTERCTL" backend upsert \
  --backend "$SERVICE_ID" \
  --name "$DISPLAY_NAME" \
  --systemd-unit "$SERVICE_ID.service"

sudo OUTERSHELL_HOME=/var/lib/outershell "$OUTERCTL" app add \
  --backend "$SERVICE_ID" \
  --frontend-id "$SERVICE_ID:main" \
  --port "$PORT" \
  --name "$DISPLAY_NAME" \
  --url "127.0.0.1:$PORT/" \
  --icon-path /opt/example-tensorboard/icon.png \
  --list "Machine Learning"

sudo OUTERSHELL_HOME=/var/lib/outershell "$OUTERCTL" log add \
  --backend "$SERVICE_ID" \
  --path /var/log/example-tensorboard/tensorboard.log
```

The root systemd unit should also set `OUTERSHELL_HOME=/var/lib/outershell` if
the backend needs to update its app row at runtime because the endpoint changes.

Root sockets should be root-owned and not group/world writable. Outer Loop opens
root-owned socket apps through its sudo root socket bridge.
