# Outer Shell Installed Files

This document lists the persistent files and sockets installed by Outer Shell and its bundled apps. Temporary `mktemp` files used during install are not retained and are not listed here.

## Path Variables

- `<user-state>` is `${OUTERSHELL_HOME}` when set. Otherwise it is `$HOME/Library/Application Support/outershell` on macOS and `${XDG_STATE_HOME:-$HOME/.local/state}/outershell` on Linux.
- `<user-runtime>` is `$(getconf DARWIN_USER_TEMP_DIR)` on macOS and `${XDG_RUNTIME_DIR:-/run/user/$(id -u)}` on Linux.
- `<system-state>` is `/Library/Application Support/outershell` on macOS and `/var/lib/outershell` on Linux.
- `<service-id>` is the backend service identifier, such as `dev.outergroup.Profile`.
- `<bundle-prefix>` is the outerframe bundle prefix, such as `ProfileContent`.

## Registry And API

User registry files:

- `<user-state>/registry.orwa`
- `<user-state>/registry.orwa.lock`
- `<user-state>/registry.orwa.tmp.XXXXXX` while rewriting the registry

System registry files:

- `<system-state>/registry.orwa`
- `<system-state>/registry.orwa.lock`
- `<system-state>/registry.orwa.tmp.XXXXXX` while rewriting the registry

Registry overrides:

- `OUTERSHELL_REGISTRY` or `BACKENDS_REGISTRY_DB` overrides the user registry path.
- `OUTERSHELL_SYSTEM_REGISTRY` or `BACKENDS_SYSTEM_REGISTRY_DB` overrides the system registry path.

API socket:

- `OUTERSHELLD_API_SOCKET` overrides the API socket path.
- macOS default: `<user-runtime>/outershelld-api`
- Linux default: `<user-runtime>/outershelld-api`

Outer Shell log views should use the exact `log_files.path` value registered in the registry. A backend service ID can exist in both the user and system registries, so resolving a log by only `serviceID` and `logIndex` can select the wrong scope.

## Outer Shell User Install

macOS public install:

- `<user-state>/outer-shell/Outer Shell.app/`
- `<user-state>/outer-shell/outershelld`
- `<user-state>/outer-shell/bin/outerctl`
- `<user-state>/outer-shell/bundles/`
- `<user-state>/outer-shell/bundled-apps/`
- `<user-state>/outer-shell/app-icon.png`
- `<user-state>/outer-shell/version`
- `<user-state>/bin/outerctl`
- `$HOME/Library/LaunchAgents/org.outershell.OuterShell.plist`
- `$HOME/Library/Logs/org.outershell.OuterShell/output.log`
- `<user-runtime>/org.outershell.OuterShell`
- `<user-runtime>/outershelld-api`

Linux public install:

- `<user-state>/outer-shell/OuterShellBackend`
- `<user-state>/outer-shell/outershelld`
- `<user-state>/outer-shell/bin/outerctl`
- `<user-state>/outer-shell/bundles/`
- `<user-state>/outer-shell/bundled-apps/`
- `<user-state>/outer-shell/app-icon.png`
- `<user-state>/outer-shell/version`
- `<user-state>/outer-shell/run-outer-shell.sh`
- `<user-state>/outer-shell/run-outershelld.sh`
- `<user-state>/outer-shell/logs/OuterShellBackend.log`
- `<user-state>/outer-shell/logs/outershelld.log`
- `<user-state>/bin/outerctl`
- `$HOME/.config/systemd/user/org.outershell.OuterShell.service`
- `$HOME/.config/systemd/user/org.outershell.OuterShell.socket`
- `$HOME/.config/systemd/user/outershelld.service`
- `$HOME/.config/systemd/user/outershelld.socket`
- `<user-runtime>/org.outershell.OuterShell`
- `<user-runtime>/outershelld-api`

## Root Helper

The root helper is installed the first time Outer Shell needs to install, uninstall, or update root-owned records:

- `/usr/local/libexec/outershelld-root-tool`

Linux also removes old helper units if present while installing the current helper:

- `/etc/systemd/system/outershelld-root-helper-<uid>.service`
- `/etc/systemd/system/outershelld-root-helper-<uid>.socket`

## Bundled Apps

Bundled app payloads use these generated files.

macOS user install:

- `<user-state>/apps/<service-id>/<binary-name>`
- `<user-state>/apps/<service-id>/bundles/<bundle-prefix>.bundle.macos-arm.aar`
- `<user-state>/apps/<service-id>/bundles/<bundle-prefix>.bundle.macos-x86.aar`
- `<user-state>/apps/<service-id>/app-icon.png` when the app has a raster icon
- `$HOME/Library/LaunchAgents/<service-id>.plist`
- `$HOME/Library/Logs/<service-id>/output.log`
- `<user-runtime>/<service-id>` for socket-activated apps

macOS root install:

- `<system-state>/apps/<service-id>/<binary-name>`
- `<system-state>/apps/<service-id>/bundles/<bundle-prefix>.bundle.macos-arm.aar`
- `<system-state>/apps/<service-id>/bundles/<bundle-prefix>.bundle.macos-x86.aar`
- `<system-state>/apps/<service-id>/app-icon.png` when the app has a raster icon
- `<system-state>/apps/<service-id>/outerctl-system`
- `/Library/LaunchDaemons/<service-id>.plist`
- `/Library/Logs/<service-id>.log`
- `/var/run/<service-id>` for socket-activated apps

Linux user install:

- `<user-state>/apps/<service-id>/<binary-name>`
- `<user-state>/apps/<service-id>/bundles/<bundle-prefix>.bundle.macos-arm.aar`
- `<user-state>/apps/<service-id>/bundles/<bundle-prefix>.bundle.macos-x86.aar`
- `<user-state>/apps/<service-id>/app-icon.png` when the app has a raster icon
- `<user-state>/apps/<service-id>/version`
- `<user-state>/apps/<service-id>/backend.log`
- `$HOME/.config/systemd/user/<service-id>.service`
- `$HOME/.config/systemd/user/<service-id>.socket` for socket-activated apps
- `<user-runtime>/<service-id>` for socket-activated apps

Linux root install:

- `/opt/outergroup/<service-id>/<binary-name>`
- `/opt/outergroup/<service-id>/bundles/<bundle-prefix>.bundle.macos-arm.aar`
- `/opt/outergroup/<service-id>/bundles/<bundle-prefix>.bundle.macos-x86.aar`
- `/opt/outergroup/<service-id>/app-icon.png` when the app has a raster icon
- `/opt/outergroup/<service-id>/version`
- `/opt/outergroup/<service-id>/outerctl-as-user`
- `/etc/systemd/system/<service-id>.service`
- `/etc/systemd/system/<service-id>.socket` for socket-activated apps
- `/var/log/outergroup/<service-id>.log`
- `/run/<service-id>` for socket-activated apps

When a Linux root install supports both root and user mode, the installer also writes a user systemd unit that points at the root-owned payload:

- `$HOME/.config/systemd/user/<service-id>.service`
- `$HOME/.config/systemd/user/<service-id>.socket` for socket-activated apps
- `<user-state>/apps/<service-id>/backend.log`

Bundled app table:

| App | Service ID | Binary | Bundle Prefix | Root |
| --- | --- | --- | --- | --- |
| Top | `dev.outergroup.Top` | `TopBackend` | `TopContent` | optional |
| Files | `dev.outergroup.Files` | `FilesBackend` | `FilesContent` | optional |
| Plaintext | `dev.outergroup.Plaintext` | `PlaintextBackend` | `PlaintextContent` | optional |
| Network Inspector | `dev.outergroup.NetworkInspector` | `NetworkInspectorBackend` | `NetworkInspectorContent` | optional |
| Firehose | `dev.outergroup.Firehose` | `FirehoseBackend` | `TraceContent` | required |
| Profile | `dev.outergroup.Profile` | `ProfileBackend` | `ProfileContent` | optional |

## User-Created Backends

Backends created from the Add Apps UI are user installs.

macOS:

- `<user-state>/apps/<service-id>/`
- `$HOME/Library/LaunchAgents/<service-id>.plist`
- `$HOME/Library/Logs/<service-id>/output.log`
- Generated recipe scripts live at the user-selected script path.

Linux:

- `<user-state>/apps/<service-id>/`
- `<user-state>/apps/<service-id>/output.log`
- `$HOME/.config/systemd/user/<service-id>.service`
- Generated recipe scripts live at the user-selected script path.

Custom backends that use Unix sockets or fixed ports register those frontends in `<user-state>/registry.orwa`.

## Uninstall Cleanup

Uninstalling Outer Shell removes its service unit, socket unit, launch agent, runtime sockets, registry entries, and `<user-state>/outer-shell`. It does not delete `<user-state>/registry.orwa` or root-installed bundled app payloads.

Uninstalling a bundled app removes its unit/plist, socket unit when present, app payload directory, registry backend/app/log/opener records, and registered log path for the selected scope.

Legacy paths may be migrated or removed during install:

- macOS legacy user root: `$HOME/Library/dev.outergroup.OuterLoop`
- Linux legacy user root: `$HOME/.outeragent`
- Linux legacy home screen root: `$HOME/.outerloop/outer-shell`
- macOS legacy system root: `/Library/dev.outergroup.OuterLoop`
- Linux legacy system root: `/var/lib/outergroup/outeragent`
