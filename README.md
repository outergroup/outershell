# Backends

Backends is an outerframe app for viewing the backends registered on the machine where the app is running. It reads the Outer Loop registry SQLite database directly and tails registered log files in place, so logs do not need to be synced back to Outer Loop.

This app will replace the old Outer Loop Services UI and the built-in log viewer. It currently includes:

- A Backends table sourced from `backends`, `frontends`, `log_files`, and `launchd_backends`
- An inline log viewer for the selected backend
- Start, stop, uninstall, and create flows for systemd-managed backends
- Bundled app installation, starting with Top

## Build

```bash
./build_run.sh
```

## Run

```bash
PORT=7354
./build/macos/Release/BackendsBackend --port "$PORT" --bundles-dir ./build/run/bundles
```

Open this URL in Outer Loop or Outerframe:

```text
http://127.0.0.1:7354/
```

For Outer Loop-managed deployments, prefer a Unix socket and register that socket with `outerctl`:

```bash
SOCKET_PATH="$XDG_RUNTIME_DIR/dev.outergroup.Backends"
./build/macos/Release/BackendsBackend \
  --socket-path "$SOCKET_PATH" \
  --bundles-dir ./build/run/bundles
```

For a user systemd unit, use `%t` for the socket root so systemd resolves it to
the user's `XDG_RUNTIME_DIR`:

```ini
ExecStart=/path/to/BackendsBackend --socket-path %t/dev.outergroup.Backends --bundles-dir /path/to/bundles
```

By default the backend reads:

```text
~/Library/dev.outergroup.OuterLoop/registry.sqlite3
```

Override the registry path with either `--database`, `BACKENDS_REGISTRY_DB`, or `OUTERLOOP_REGISTRY_DB`:

```bash
./build/macos/Release/BackendsBackend \
  --port 7354 \
  --bundles-dir ./build/run/bundles \
  --database ~/Library/dev.outergroup.OuterLoop/registry.sqlite3
```

## Bundled Apps

Backends looks for bundled app payloads in `./bundled-apps` relative to its working directory. You can override that with `BACKENDS_BUNDLED_APPS_DIR`.

Bundled apps use this layout:

```text
bundled-apps/<AppName>/
  RemoteLinuxBinaries/
    aarch64/<BackendBinary>
    x86_64/<BackendBinary>
  bundles/
    <ContentName>.bundle.macos-arm.aar
    <ContentName>.bundle.macos-x86.aar
  app-icon.png
```

Backends currently bundles Top and Files. When a bundled app is installed, Backends copies the payload into `~/.outeragent/<service id>`, writes its user systemd unit, records the backend/log metadata in the registry, and starts the service. Bundled apps register their own frontend with `outerctl` after they start.

## API

- `GET /api/backends`
- `GET /api/logs?serviceID=<id>&logIndex=0&bytes=262144`
- `POST /api/control?serviceID=<id>&operation=start`
- `POST /api/control?serviceID=<id>&operation=stop`
- `POST /api/control?serviceID=<id>&operation=run`
- `POST /api/control?serviceID=<id>&operation=uninstall`
