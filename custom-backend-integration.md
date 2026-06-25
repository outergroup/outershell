# Custom Backend Integration

This guide explains how to make an existing web backend appear in Outer Shell.
The short version is:

1. Run your backend under the platform service manager.
2. Register the backend, app entry point, and logs with `outerctl`.
3. Let Outer Shell start, stop, open, and tail the backend.

Most backends expose HTTP on either a loopback TCP port or a Unix socket. The
examples use TensorBoard with placeholder paths, but the same pattern works for
Jupyter, local dashboards, development servers, and internal tools.

## Start Simple

If you only want to run a command on a fixed port or Unix socket, use Outer
Shell's built-in creation flow first. From the Apps page, choose `Add app`, then
use one of the command recipes. Outer Shell will generate the wrapper script,
service-manager unit, registry rows, and log path for you.

That flow is usually enough for simple commands such as:

```bash
cd ~/work/project && .venv/bin/tensorboard --logdir runs --host 127.0.0.1 --port 6006
```

For Unix socket commands, choose `Unix Socket` in the recipe and enter the
absolute socket path the command should bind. The generated script exports
`SOCKET_PATH`, so the command can use that same path if the tool supports it.

The rest of this document intentionally shows the raw pieces. You only need
these details when you want to package your own installer, customize the
systemd/launchd unit, run as root, or update runtime URLs from your backend.

## Terms

- **Backend**: the managed service, identified by a stable id such as
  `com.example.TensorBoard`.
- **App**: a user-facing entry point for the backend. Apps have names, URLs,
  optional icons, and optional list placement.
- **Registry**: metadata owned by `outershelld` and updated through `outerctl`.
- **User install**: a service and registry entry owned by the current user.
- **Root install**: a service and registry entry owned by root.

Backends do not have icons. App entries have icons.

## `outerctl`

`outerctl` talks to the local `outershelld` socket API. Run it as the same user
that owns the registry you want to update.

On Linux, the public Outer Shell installer installs it here for user installs:

```bash
OUTERCTL="${XDG_STATE_HOME:-$HOME/.local/state}/outershell/bin/outerctl"
```

For a Linux root/system install, run it with `sudo` and set the root Outer Shell
home:

```bash
sudo OUTERSHELL_HOME=/var/lib/outershell \
  /home/alice/.local/state/outershell/bin/outerctl ...
```

On macOS, user installs normally use the `outerctl` bundled with the local
Outer Shell installation:

```bash
OUTERSHELL_HOME="$HOME/Library/Application Support/outershell"
OUTERCTL="$OUTERSHELL_HOME/bin/outerctl"
```

For a macOS root install, run the bundled `outerctl` with `sudo` and a root
Outer Shell home:

```bash
sudo OUTERSHELL_HOME=/Library/Application\ Support/outershell \
  "$HOME/Library/Application Support/outershell/bin/outerctl" ...
```

## Shared Example Values

The examples below use the same service id and app metadata on Linux and macOS:

```bash
SERVICE_ID="com.example.TensorBoard"
DISPLAY_NAME="TensorBoard"
FRONTEND_ID="$SERVICE_ID:main"
PORT="6006"
```

Bind backend HTTP servers to `127.0.0.1` unless you have a specific reason to
listen publicly. Outer Loop reaches the backend through its normal forwarding
path; the backend does not need to expose itself to the network.

## Linux: systemd User Unit

Create a payload directory and Python environment:

```bash
APP_ROOT="$HOME/.local/share/example-tensorboard"
LOG_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/outershell/logs/$SERVICE_ID"

mkdir -p "$APP_ROOT" "$LOG_DIR"
cd "$APP_ROOT"
uv venv .venv
uv pip install tensorboard
mkdir -p runs
```

Create a small runner script:

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

Enable the unit. Starting immediately is optional; Outer Shell can start it
later when the user opens the app.

```bash
systemctl --user daemon-reload
systemctl --user enable "$SERVICE_ID.service"
```

For initial testing:

```bash
systemctl --user start "$SERVICE_ID.service"
systemctl --user status "$SERVICE_ID.service"
curl -I "http://127.0.0.1:$PORT/"
```

## macOS: launchd User Plist

Create a payload directory and Python environment:

```bash
APP_ROOT="$HOME/Library/Application Support/example-tensorboard"
LOG_DIR="$HOME/Library/Logs/outershell/$SERVICE_ID"

mkdir -p "$APP_ROOT" "$LOG_DIR"
cd "$APP_ROOT"
uv venv .venv
uv pip install tensorboard
mkdir -p runs
```

Create a small runner script:

```bash
cat > "$APP_ROOT/run-tensorboard.sh" <<'SH'
#!/bin/sh
set -eu

APP_ROOT="${APP_ROOT:-$HOME/Library/Application Support/example-tensorboard}"
PORT="${PORT:-6006}"

exec "$APP_ROOT/.venv/bin/tensorboard" \
  --logdir "$APP_ROOT/runs" \
  --host 127.0.0.1 \
  --port "$PORT"
SH
chmod 0755 "$APP_ROOT/run-tensorboard.sh"
```

Create `~/Library/LaunchAgents/com.example.TensorBoard.plist`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>com.example.TensorBoard</string>

  <key>ProgramArguments</key>
  <array>
    <string>/Users/alice/Library/Application Support/example-tensorboard/run-tensorboard.sh</string>
  </array>

  <key>EnvironmentVariables</key>
  <dict>
    <key>APP_ROOT</key>
    <string>/Users/alice/Library/Application Support/example-tensorboard</string>
    <key>PORT</key>
    <string>6006</string>
  </dict>

  <key>WorkingDirectory</key>
  <string>/Users/alice/Library/Application Support/example-tensorboard</string>

  <key>StandardOutPath</key>
  <string>/Users/alice/Library/Logs/outershell/com.example.TensorBoard/tensorboard.log</string>
  <key>StandardErrorPath</key>
  <string>/Users/alice/Library/Logs/outershell/com.example.TensorBoard/tensorboard.log</string>

  <key>KeepAlive</key>
  <false/>
</dict>
</plist>
```

Replace `/Users/alice` with the real home directory. Then load the plist:

```bash
launchctl bootstrap "gui/$(id -u)" "$HOME/Library/LaunchAgents/$SERVICE_ID.plist"
```

Starting immediately is optional. For initial testing:

```bash
launchctl kickstart -k "gui/$(id -u)/$SERVICE_ID"
launchctl print "gui/$(id -u)/$SERVICE_ID"
curl -I "http://127.0.0.1:$PORT/"
```

## Register with Outer Shell

Registration is intentionally almost identical across Linux and macOS. The only
backend-level difference is whether you point Outer Shell at a systemd unit or a
launchd plist.

On Linux:

```bash
"$OUTERCTL" backend upsert \
  --backend "$SERVICE_ID" \
  --name "$DISPLAY_NAME" \
  --systemd-unit "$SERVICE_ID.service"
```

On macOS:

```bash
"$OUTERCTL" backend upsert \
  --backend "$SERVICE_ID" \
  --name "$DISPLAY_NAME" \
  --launchd-plist "$HOME/Library/LaunchAgents/$SERVICE_ID.plist" \
  --outershell-owns true
```

Then register the app and log path. These commands are the same shape on both
platforms:

```bash
"$OUTERCTL" app add \
  --backend "$SERVICE_ID" \
  --frontend-id "$FRONTEND_ID" \
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

- `backend upsert` creates the managed service identity and records the
  systemd unit or launchd plist so Outer Shell can start and stop it.
- `app add` creates the Apps-page entry point. Reuse the same `--frontend-id`
  for runtime endpoint updates.
- `log add` lets Outer Shell show the service log viewer.

After registration, refresh Outer Shell. TensorBoard should appear on the Apps
page. If the service is stopped, clicking the app asks Outer Shell to start the
unit and waits for the registered port to become reachable before opening it.

## Runtime URL Updates

The install-time app row is the stable entry point. Outer Shell determines
whether the app is running from systemd or launchd, and it polls the registered
port or socket while starting an app.

Most backends do not need to announce anything at runtime. Runtime updates are
only needed when the final endpoint changes after the process starts, for
example:

- Jupyter prints a new tokenized URL on each run.
- The backend chooses a free port dynamically.
- The backend creates different app entries based on local state.

In that case, call `app add` again with the same `--frontend-id` after the
backend knows the final endpoint:

```bash
"$OUTERCTL" app add \
  --backend "$SERVICE_ID" \
  --frontend-id "$FRONTEND_ID" \
  --port "$PORT" \
  --name "$DISPLAY_NAME" \
  --url "127.0.0.1:$PORT/"
```

Use `--port` for TCP HTTP backends and `--socket-path` for Unix-socket HTTP
backends. Scripts do not need to announce "running" on start or clear app state
on exit.

## Unix Socket Backends

If your backend listens on a Unix socket instead of a TCP port, register the app
with `--socket-path`:

```bash
"$OUTERCTL" app add \
  --backend "$SERVICE_ID" \
  --frontend-id "$FRONTEND_ID" \
  --socket-path "$XDG_RUNTIME_DIR/com.example.TensorBoard.sock" \
  --name "$DISPLAY_NAME" \
  --url "$XDG_RUNTIME_DIR/com.example.TensorBoard.sock"
```

For user services, the socket should be owned by the user. For root services,
the socket should be root-owned and not group/world writable. Outer Loop opens
root-owned socket apps through its sudo root socket bridge.

## Root Installs

Root installs use the same registry model, but the service manager and
`outerctl` commands run as root.

On Linux:

```bash
sudo systemctl daemon-reload
sudo systemctl enable "$SERVICE_ID.service"

sudo OUTERSHELL_HOME=/var/lib/outershell "$OUTERCTL" backend upsert \
  --backend "$SERVICE_ID" \
  --name "$DISPLAY_NAME" \
  --systemd-unit "$SERVICE_ID.service"
```

On macOS:

```bash
sudo launchctl bootstrap system "/Library/LaunchDaemons/$SERVICE_ID.plist"

sudo OUTERSHELL_HOME=/Library/Application\ Support/outershell "$OUTERCTL" backend upsert \
  --backend "$SERVICE_ID" \
  --name "$DISPLAY_NAME" \
  --launchd-plist "/Library/LaunchDaemons/$SERVICE_ID.plist" \
  --outershell-owns true
```

Then run the same root-scoped `app add` and `log add` commands:

```bash
sudo OUTERSHELL_HOME=/var/lib/outershell "$OUTERCTL" app add \
  --backend "$SERVICE_ID" \
  --frontend-id "$FRONTEND_ID" \
  --port "$PORT" \
  --name "$DISPLAY_NAME" \
  --url "127.0.0.1:$PORT/" \
  --icon-path /opt/example-tensorboard/icon.png \
  --list "Machine Learning"

sudo OUTERSHELL_HOME=/var/lib/outershell "$OUTERCTL" log add \
  --backend "$SERVICE_ID" \
  --path /var/log/example-tensorboard/tensorboard.log
```

Adjust `OUTERSHELL_HOME` for macOS root installs:

```bash
sudo OUTERSHELL_HOME=/Library/Application\ Support/outershell "$OUTERCTL" ...
```

If a root backend updates its runtime URL, its process should use the root
registry home when invoking `outerctl`.

## Verify

After registration, these commands should show the backend, app, and log file:

```bash
"$OUTERCTL" backend list --backend "$SERVICE_ID"
"$OUTERCTL" app list --backend "$SERVICE_ID"
"$OUTERCTL" log list --backend "$SERVICE_ID"
```

If clicking the app fails, first verify the service outside Outer Shell.

On Linux:

```bash
systemctl --user status "$SERVICE_ID.service"
curl -I "http://127.0.0.1:$PORT/"
```

On macOS:

```bash
launchctl print "gui/$(id -u)/$SERVICE_ID"
curl -I "http://127.0.0.1:$PORT/"
```

Then verify that the registry points at the same endpoint that the service is
actually serving.
