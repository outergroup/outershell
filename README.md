# Home Screen

Home Screen is an outerframe app for launching apps and viewing the backends registered on the machine where the app is running. It reads the Outer Loop registry SQLite database directly and tails registered log files in place, so logs do not need to be synced back to Outer Loop.

This app will replace the old Outer Loop Services UI and the built-in log viewer. It currently includes:

- A Backends table sourced from `backends`, `frontends`, `log_files`, `systemd_backends`, and `launchd_backends`
- An inline log viewer for the selected backend
- Start, stop, uninstall, and create flows for systemd-managed backends
- User or root installation for bundled systemd backends
- Starter app installation from the Home Screen app catalog

## Build

```bash
./build_run.sh
```

## Run

```bash
PORT=7354
./build/macos/Release/HomeScreenBackend --port "$PORT" --bundles-dir ./build/run/bundles
```

Open this URL in Outer Loop or Outerframe:

```text
http://127.0.0.1:7354/
```

For ad hoc Unix socket testing:

```bash
SOCKET_PATH="$(getconf DARWIN_USER_TEMP_DIR)dev.outergroup.HomeScreen"
./build/macos/Release/HomeScreenBackend \
  --socket-path "$SOCKET_PATH" \
  --bundles-dir ./build/run/bundles
```

For a macOS LaunchAgent, prefer launchd-owned socket activation. Use the
per-user Darwin temp directory as the closest macOS analogue to
`XDG_RUNTIME_DIR`, and use `dev.outergroup.HomeScreen` as the socket filename:

```xml
<key>ProgramArguments</key>
<array>
  <string>/path/to/HomeScreenBackend</string>
  <string>--socket-path</string>
  <string>/var/folders/.../T/dev.outergroup.HomeScreen</string>
  <string>--bundles-dir</string>
  <string>/path/to/bundles</string>
</array>
<key>Sockets</key>
<dict>
  <key>Listener</key>
  <dict>
    <key>SockPathName</key>
    <string>/var/folders/.../T/dev.outergroup.HomeScreen</string>
    <key>SockPathMode</key>
    <integer>384</integer>
  </dict>
</dict>
```

Then register the socket with `outerctl`:

```bash
outerctl app add --backend dev.outergroup.HomeScreen \
  --socket-path "$SOCKET_PATH" \
  --name "Home Screen" \
  --url http+unix://$SOCKET_PATH/ \
  --icon-file /path/to/app-icon.png \
  --home-screen
```

Home Screen uses `/` for Apps, `/backends` for the backend table, and `/new` for
the create flow.

For a user systemd unit, use `%t` for the socket root so systemd resolves it to
the user's `XDG_RUNTIME_DIR`:

```ini
ExecStart=/path/to/HomeScreenBackend --socket-path %t/dev.outergroup.HomeScreen --bundles-dir /path/to/bundles
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
./build/macos/Release/HomeScreenBackend \
  --port 7354 \
  --bundles-dir ./build/run/bundles \
  --database ~/Library/dev.outergroup.OuterLoop/registry.sqlite3
```

## Starter App Catalog

Home Screen keeps only the starter app catalog in this repo. The catalog is
[Resources/app-catalog.json](/Users/mrcslws/dev/src/Backends/Resources/app-catalog.json);
each listed app owns its backend/frontend build and publishes its tarball from
its own repo.

At runtime, Home Screen looks for app payloads in `build/run/bundled-apps` next
to a local build, then in the directory passed with `--bundled-apps-dir` or
`BACKENDS_BUNDLED_APPS_DIR`. If a Linux starter app payload is not present
locally, Home Screen downloads it from the catalog URL into
`$XDG_CACHE_HOME/outerloop/home-screen/bundled-apps` or
`~/.cache/outerloop/home-screen/bundled-apps`.

Starter app tarballs use this layout:

```text
<AppName>/
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

Home Screen currently offers Top, Files, Network Inspector, and Firehose on
Linux/SSH, and Top on localhost macOS. `build_run.sh` builds and stages the
macOS Top payload from the `~/dev/src/Top` checkout for local testing;
install-time code only copies that prebuilt payload.

On Linux, when a bundled app is installed for the current user, Backends copies the payload into `~/.outeragent/<service id>`, writes its user systemd unit, records the backend/log metadata in the registry, and starts the service. On macOS, localhost installs copy the payload into `~/Library/dev.outergroup.OuterLoop/backends/<service id>`, write a LaunchAgent, record metadata in the registry, and start the service.

Bundled apps can also be installed as root from the action menu. Root installs use a system systemd unit, copy the payload into `/opt/outergroup/<service id>`, write logs under `/var/log/outergroup`, write registry metadata to `/var/lib/outergroup/outeragent/registry.sqlite3`, and put Unix sockets under the system runtime directory, such as `/run/dev.outergroup.Top`. These operations use `sudo`; if sudo needs a password, the Backends UI prompts and retries the operation.

Bundled apps register their own frontend with `outerctl` after they start. Root-installed bundled apps run `outerctl` through a small wrapper that sets `OUTERAGENT_ROOT=/var/lib/outergroup/outeragent`, so frontend and log metadata are recorded in the system registry.

## Remote Distribution

Home Screen can be published as a small Linux installer plus per-architecture
archives. The generated installer installs a user systemd socket at:

```text
$XDG_RUNTIME_DIR/dev.outergroup.HomeScreen
```

and expands the matching architecture payload under:

```text
~/.outerloop/home-screen
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
