# Outer Shell

Most of this README is written by AI, to serve as useful context for coding agents.

Human-written documentation is here: https://outershell.org/

Outer Shell is an Outerframe app for installing, launching, and opening apps on
the machine it is connected to. It is the default home app used by Outer Loop for
local and SSH sessions.

This repository contains the Outer Shell app, its local registry daemon, the
command-line tool used by installed apps, release packaging scripts, and the
starter app catalog format. The bundled apps themselves live in their own
repositories and publish their own payload archives.

## Components

- `Outer Shell.app` / `OuterShellAgent`: the macOS agent app. It hosts the Outer
  Shell HTTP backend locally and owns the subtle macOS menu bar status item.
- `OuterShellBackend`: the HTTP backend that serves the Outerframe UI and bridges
  UI requests to `outershelld`.
- `outershelld`: the registry daemon. It owns the local socket API, the registry,
  content-type/openers metadata, app install records, and log metadata.
- `outerctl`: the small command-line client used by installers and apps to update
  the registry through `outershelld`.
- `Outer Shell` Xcode target: the Outerframe frontend bundle for the Outer Shell UI.
- `Resources/app-catalog.example.json`: the app catalog schema used to discover
  installable starter apps.

The old internal product name was "Backends"; some source file or target names
still reflect that history, but user-facing names should be "Outer Shell" or
`org.outershell.*`.

## Run Locally

For localhost macOS development, run the agent app:

```bash
SOCKET_PATH="$(getconf DARWIN_USER_TEMP_DIR)org.outershell.OuterShell"
"./build/macos/Release/Outer Shell.app/Contents/MacOS/Outer Shell" \
  --socket-path "$SOCKET_PATH" \
  --app-base-url https://outershell.org/outer-shell/apps
```

For backend-only development, run `outershelld` and `OuterShellBackend`
separately:

```bash
PORT=7354
API_SOCKET="$(getconf DARWIN_USER_TEMP_DIR)outershelld-api"
./build/macos/Release/outershelld --api-socket-path "$API_SOCKET" &
./build/macos/Release/OuterShellBackend \
  --port "$PORT" \
  --api-socket-path "$API_SOCKET" \
  --bundles-dir ./build/run/bundles
```

Then open:

```text
http://127.0.0.1:7354/
```

## Registry And Installed Files

Outer Shell stores its registry as an `.orwa` file. The default user registry is:

```text
macOS: ~/Library/Application Support/outershell/registry.orwa
Linux: ${XDG_STATE_HOME:-~/.local/state}/outershell/registry.orwa
```

Linux root/system installs use:

```text
/var/lib/outershell/registry.orwa
```

See these documents for details:

- `installed-files.md`: files, units, logs, caches, sockets, and root locations.
- `outershell-registry.md`: registry structure.
- `file-association-api.md`: content types and openers.
- `outershelld-socket-api.md`: daemon socket API.
- `outerctl-socket-messages.md`: binary protocol used by `outerctl`.

## App Catalog And Bundled Apps

Outer Shell does not embed every starter app. The app catalog points to
platform-specific app archives, and each app owns its own backend/frontend build.
See:

- `Resources/app-catalog.example.json` for the catalog shape.
- `Resources/README.md` for Outer Shell release assets and starter app archive
  layout.

Installed app payloads are copied into user or root locations appropriate for the
platform, registered with `outershelld`, and launched through launchd on macOS or
systemd on Linux.

## Release Packaging

Release assets are produced by:

```bash
PUBLIC_BASE_URL="https://example.com/outer-shell" \
OUTER_SHELL_VERSION="0.1.0" \
APP_CATALOG_PATH="/path/to/app-catalog.json" \
./Scripts/package_release.sh
```

The script writes:

```text
build/release/outer-shell/latest/install.sh
build/release/outer-shell/latest/version.txt
build/release/outer-shell/latest/outer-shell-linux-aarch64.tar.gz
build/release/outer-shell/latest/outer-shell-linux-x86_64.tar.gz
build/release/outer-shell/latest/outer-shell-macos-arm64.zip
build/release/outer-shell/latest/outer-shell-macos-x86_64.zip
build/release/outer-shell/app-catalog.json
```

Publishing to a website, object storage, or CDN is intentionally handled outside
this repository.

## Related Documentation

- `custom-backend-integration.md`: how custom shell-command apps are generated
  and installed.
- `Resources/README.md`: release asset and app archive layouts.
