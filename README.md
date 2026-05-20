# Navigator

Navigator is an outerframe app for launching apps and viewing the backends registered on the machine where the app is running. It reads the Outer Loop registry SQLite database directly and tails registered log files in place, so logs do not need to be synced back to Outer Loop.

This app will replace the old Outer Loop Services UI and the built-in log viewer. It currently includes:

- A Backends table sourced from `backends`, `frontends`, `log_files`, `systemd_backends`, and `launchd_backends`
- An inline log viewer for the selected backend
- Start, stop, uninstall, and create flows for systemd-managed backends
- User or root installation for bundled systemd backends
- Bundled app installation, starting with Top

## Build

```bash
./build_run.sh
```

## Run

```bash
PORT=7354
./build/macos/Release/NavigatorBackend --port "$PORT" --bundles-dir ./build/run/bundles
```

Open this URL in Outer Loop or Outerframe:

```text
http://127.0.0.1:7354/
```

For ad hoc Unix socket testing:

```bash
SOCKET_PATH="$(getconf DARWIN_USER_TEMP_DIR)dev.outergroup.Navigator"
./build/macos/Release/NavigatorBackend \
  --socket-path "$SOCKET_PATH" \
  --bundles-dir ./build/run/bundles
```

For a macOS LaunchAgent, prefer launchd-owned socket activation. Use the
per-user Darwin temp directory as the closest macOS analogue to
`XDG_RUNTIME_DIR`, and use `dev.outergroup.Navigator` as the socket filename:

```xml
<key>ProgramArguments</key>
<array>
  <string>/path/to/NavigatorBackend</string>
  <string>--socket-path</string>
  <string>/var/folders/.../T/dev.outergroup.Navigator</string>
  <string>--bundles-dir</string>
  <string>/path/to/bundles</string>
</array>
<key>Sockets</key>
<dict>
  <key>Listener</key>
  <dict>
    <key>SockPathName</key>
    <string>/var/folders/.../T/dev.outergroup.Navigator</string>
    <key>SockPathMode</key>
    <integer>384</integer>
  </dict>
</dict>
```

Then register the socket with `outerctl`:

```bash
outerctl app add --backend dev.outergroup.Navigator \
  --socket-path "$SOCKET_PATH" \
  --name Navigator \
  --url http+unix://$SOCKET_PATH/ \
  --home-screen
```

Navigator uses `/` for Apps, `/backends` for the backend table, and `/new` for
the create flow.

For a user systemd unit, use `%t` for the socket root so systemd resolves it to
the user's `XDG_RUNTIME_DIR`:

```ini
ExecStart=/path/to/NavigatorBackend --socket-path %t/dev.outergroup.Navigator --bundles-dir /path/to/bundles
```

By default the backend reads the user registry:

```text
~/Library/dev.outergroup.OuterLoop/registry.sqlite3
```

On Linux it also reads the system/root registry:

```text
/var/lib/outergroup/outeragent/registry.sqlite3
```

Override the user registry path with either `--database`, `BACKENDS_REGISTRY_DB`, or `OUTERLOOP_REGISTRY_DB`. Override the system registry path with `--system-database`, `BACKENDS_SYSTEM_REGISTRY_DB`, or `OUTERLOOP_SYSTEM_REGISTRY_DB`.

```bash
./build/macos/Release/NavigatorBackend \
  --port 7354 \
  --bundles-dir ./build/run/bundles \
  --database ~/Library/dev.outergroup.OuterLoop/registry.sqlite3
```

## Bundled Apps

Backends looks for bundled app payloads in `build/run/bundled-apps` next to a local build, then in `./bundled-apps` relative to its working directory. You can override that with `--bundled-apps-dir` or `BACKENDS_BUNDLED_APPS_DIR`.

Bundled apps use this layout:

```text
bundled-apps/<AppName>/
  MacOS/
    <BackendBinary>
  RemoteLinuxBinaries/
    aarch64/<BackendBinary>
    x86_64/<BackendBinary>
  bundles/
    <ContentName>.bundle.macos-arm.aar
    <ContentName>.bundle.macos-x86.aar
  app-icon.png
```

Backends currently bundles Top and Files on Linux/SSH, and Top on localhost macOS. `build_run.sh` builds and stages the macOS Top backend with Xcode ahead of time; install-time code only copies that prebuilt payload.

On Linux, when a bundled app is installed for the current user, Backends copies the payload into `~/.outeragent/<service id>`, writes its user systemd unit, records the backend/log metadata in the registry, and starts the service. On macOS, localhost installs copy the payload into `~/Library/dev.outergroup.OuterLoop/backends/<service id>`, write a LaunchAgent, record metadata in the registry, and start the service.

Bundled apps can also be installed as root from the action menu. Root installs use a system systemd unit, copy the payload into `/opt/outergroup/<service id>`, write logs under `/var/log/outergroup`, write registry metadata to `/var/lib/outergroup/outeragent/registry.sqlite3`, and put Unix sockets under the system runtime directory, such as `/run/dev.outergroup.Top`. These operations use `sudo`; if sudo needs a password, the Backends UI prompts and retries the operation.

Bundled apps register their own frontend with `outerctl` after they start. Root-installed bundled apps run `outerctl` through a small wrapper that sets `OUTERAGENT_ROOT=/var/lib/outergroup/outeragent`, so frontend and log metadata are recorded in the system registry.

## API

- `GET /api/backends`
- `GET /api/logs?serviceID=<id>&logIndex=0&bytes=262144`
- `POST /api/control?serviceID=<id>&operation=start`
- `POST /api/control?serviceID=<id>&operation=stop`
- `POST /api/control?serviceID=<id>&operation=run`
- `POST /api/control?serviceID=<id>&operation=uninstall`
