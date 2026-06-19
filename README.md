# Outer Shell

Outer Shell is an outerframe app for launching apps and viewing the backends registered on the machine where the app is running. `outershelld` owns the local socket API used by `outerctl` and the registry. `OuterShellBackend` owns the HTTP server for the Outer Shell UI and talks to `outershelld` over that socket.

This app will replace the old Outer Loop Services UI and the built-in log viewer. It currently includes:

- A backend-services table sourced from `backends`, `frontends`, `log_files`, `systemd_backends`, and `launchd_backends`
- An inline log viewer for the selected backend
- Start, stop, uninstall, and create flows for systemd-managed backends
- User or root installation for bundled systemd backends
- Starter app installation from the Outer Shell app catalog

## Build

```bash
./build_run.sh
```

## Run

For localhost on macOS, use the Outer Shell agent app. It hosts the Outer Shell
socket and owns the menu bar service-list item from one process:

```bash
SOCKET_PATH="$(getconf DARWIN_USER_TEMP_DIR)org.outershell.OuterShell"
"./build/macos/Release/Outer Shell.app/Contents/MacOS/Outer Shell" \
  --socket-path "$SOCKET_PATH" \
  --app-base-url https://outershell.org/outer-shell/apps
```

For backend-only development, run the API broker and HTTP backend separately:

```bash
PORT=7354
API_SOCKET="$(getconf DARWIN_USER_TEMP_DIR)outershelld-api"
./build/macos/Release/outershelld --api-socket-path "$API_SOCKET" &
./build/macos/Release/OuterShellBackend \
  --port "$PORT" \
  --api-socket-path "$API_SOCKET" \
  --bundles-dir ./build/run/bundles
```

By default `outershelld` opens an `outerctl` API socket at
`${OUTERSHELLD_API_SOCKET}`, `$XDG_RUNTIME_DIR/outershelld-api`, or a
platform-specific per-user temporary path. Use `--api-socket-path` to override
it.

Open this URL in Outer Loop or Outerframe:

```text
http://127.0.0.1:7354/
```

For ad hoc Unix socket testing:

```bash
SOCKET_PATH="$(getconf DARWIN_USER_TEMP_DIR)org.outershell.OuterShell"
API_SOCKET="$(getconf DARWIN_USER_TEMP_DIR)outershelld-api"
./build/macos/Release/outershelld \
  --api-socket-path "$API_SOCKET" &
./build/macos/Release/OuterShellBackend \
  --socket-path "$SOCKET_PATH" \
  --api-socket-path "$API_SOCKET" \
  --bundles-dir ./build/run/bundles
```

For a macOS LaunchAgent, prefer the Outer Shell agent app with launchd-owned
socket activation. Use the per-user Darwin temp directory as the closest macOS
analogue to `XDG_RUNTIME_DIR`, and use `org.outershell.OuterShell` as the
socket filename. `RunAtLoad` keeps the menu bar item available at login, while
the same process also receives socket-activation traffic:

```xml
<key>ProgramArguments</key>
<array>
  <string>/path/to/Outer Shell.app/Contents/MacOS/Outer Shell</string>
  <string>--socket-path</string>
  <string>/var/folders/.../T/org.outershell.OuterShell</string>
  <string>--app-base-url</string>
    <string>https://outershell.org/outer-shell/apps</string>
</array>
<key>RunAtLoad</key>
<true/>
<key>ProcessType</key>
<string>Interactive</string>
<key>LimitLoadToSessionType</key>
<string>Aqua</string>
<key>Sockets</key>
<dict>
  <key>Listener</key>
  <dict>
    <key>SockPathName</key>
    <string>/var/folders/.../T/org.outershell.OuterShell</string>
    <key>SockPathMode</key>
    <integer>384</integer>
  </dict>
</dict>
```

Then register the socket with `outerctl`:

```bash
outerctl app add --backend org.outershell.OuterShell \
  --socket-path "$SOCKET_PATH" \
  --name "Outer Shell" \
  --url http+unix://$SOCKET_PATH/ \
  --icon-path /path/to/app-icon.png
```

Outer Shell uses `/` for Apps, `/backends` for the backend table, and `/new` for
the create flow.

For a user systemd unit, use `%t` for the socket root so systemd resolves it to
the user's `XDG_RUNTIME_DIR`:

```ini
ExecStart=/path/to/OuterShellBackend --stay-alive --socket-path %t/org.outershell.OuterShell --api-socket-path %t/outershelld-api --bundles-dir /path/to/bundles
```

The socket-activated API unit templates live in `Resources/systemd`. The socket
unit owns `%t/outershelld-api` and passes it to `outershelld` as the `api` file
descriptor.

By default the backend reads the user registry. On Linux:

```text
${XDG_STATE_HOME:-~/.local/state}/outershell/registry.orwa
```

On macOS:

```text
~/Library/Application Support/outershell/registry.orwa
```

On Linux it also reads the system/root registry:

```text
/var/lib/outershell/registry.orwa
```

Override the user registry path with either `--database`, `OUTERSHELL_REGISTRY`, or `BACKENDS_REGISTRY_DB`. Override the system registry path with `--system-database`, `OUTERSHELL_SYSTEM_REGISTRY`, or `BACKENDS_SYSTEM_REGISTRY_DB`. If an override still points at a legacy `registry.sqlite3` path, Outer Shell uses the sibling `registry.orwa` file and `outershelld` migrates the SQLite file when needed.

```bash
./build/macos/Release/outershelld \
  --api-socket-path "$API_SOCKET" \
  --database ~/.local/state/outershell/registry.orwa
```

## Starter App Catalog

Outer Shell keeps only the starter app catalog in this repo. The catalog is
[Resources/app-catalog.json](/Users/mrcslws/dev/src/Backends/Resources/app-catalog.json);
each listed app owns its backend/frontend build and publishes its tarball from
its own repo.

At runtime, Outer Shell looks for app payloads in `build/run/bundled-apps` next
to a local build, then in the directory passed with `--bundled-apps-dir` or
`BACKENDS_BUNDLED_APPS_DIR`. If a starter app payload is not present locally,
Outer Shell downloads it from the catalog URL into
`$HOME/Library/Caches/outershell/outer-shell/bundled-apps` on macOS,
`$XDG_CACHE_HOME/outershell/outer-shell/bundled-apps` or
`~/.cache/outershell/outer-shell/bundled-apps` on Linux, or
`/var/cache/outershell/outer-shell/bundled-apps` when Outer Shell is running
directly as root on Linux. Downloaded staging files are removed after a
successful install.

Starter app tarballs use this layout:

```text
<AppName>/
  <AppName>.app/                  # macOS localhost payload, when supported
    Contents/
      Info.plist
      MacOS/<BackendBinary>
      Resources/
        app-icon.png
        bundles/
          <ContentName>.bundle.macos-arm.aar
          <ContentName>.bundle.macos-x86.aar
  RemoteLinuxBinaries/
    aarch64/<BackendBinary>
    x86_64/<BackendBinary>
  bundles/                        # Linux/SSH payload resources
    <ContentName>.bundle.macos-arm.aar
    <ContentName>.bundle.macos-x86.aar
  app-icon.png
```

Outer Shell currently offers Top, Files, Network Inspector, and Firehose on
Linux/SSH, and Top on localhost macOS. Local `build_run.sh` builds and stages
the macOS Top payload from the `~/dev/src/Top` checkout for local testing.
Public Outer Shell packages do not embed bundled app payloads; each app archive
is published independently and must include its own macOS backend when it is
available on localhost macOS.

On Linux, when a bundled app is installed for the current user, Outer Shell copies the payload into `${XDG_STATE_HOME:-~/.local/state}/outershell/apps/<service id>`, writes its user systemd unit, records the backend/log metadata in the registry, and starts the service. On macOS, localhost installs copy the app bundle into `~/Library/Application Support/outershell/apps/<service id>/<AppName>.app`, write a LaunchAgent that runs the contained executable, record metadata in the registry, and start the service.

Bundled apps can also be installed as root from the action menu. Root installs use a system systemd unit, copy the payload into `/opt/outershell/<service id>`, write logs under `/var/log/outershell`, write registry metadata to `/var/lib/outershell/registry.orwa`, and put Unix sockets under the system runtime directory, such as `/run/org.outershell.Top`. These operations use `sudo`; if sudo needs a password, the Outer Shell UI prompts and retries the operation.

Bundled apps register their own frontend with the `outerctl` installed by Outer Shell. On Linux, the public Outer Shell installer places it at `${XDG_STATE_HOME:-~/.local/state}/outershell/bin/outerctl`; generated user systemd units use that path. Root-installed bundled apps run it through a small wrapper that sets `OUTERSHELL_HOME=/var/lib/outershell`, so frontend and log metadata are recorded in the system registry.

## Remote Distribution

Outer Shell can be published as a small Linux installer plus per-architecture
archives. The generated installer installs a user systemd socket at:

```text
$XDG_RUNTIME_DIR/org.outershell.OuterShell
```

and expands the matching architecture payload under:

```text
${XDG_STATE_HOME:-~/.local/state}/outershell/outer-shell
```

See [Resources/README.md](/Users/mrcslws/dev/src/Backends/Resources/README.md)
for the generic release asset layout. Public hosting, bucket paths, cache
invalidations, and app catalog generation should live outside this repository.

## API

- `GET /api/backends`
- `GET /api/logs?serviceID=<id>&logIndex=0&bytes=262144`
- `POST /api/control?serviceID=<id>&operation=start`
- `POST /api/control?serviceID=<id>&operation=stop`
- `POST /api/control?serviceID=<id>&operation=run`
- `POST /api/control?serviceID=<id>&operation=uninstall`
