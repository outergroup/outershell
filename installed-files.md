# Outer Shell Installed Files

This document lists the persistent files and sockets installed by Outer Shell and its bundled apps. Temporary `mktemp` files used during install are not retained and are not listed here.

## Path Variables

- `<user-state>` is `${OUTERSHELL_HOME}` when set. Otherwise it is `$HOME/Library/Application Support/outershell` on macOS and `${XDG_STATE_HOME:-$HOME/.local/state}/outershell` on Linux. When `outershelld` is running as effective uid 0 on Linux and `OUTERSHELL_HOME` is not set, it uses `<system-state>` instead of `/root/.local/state/outershell`.
- `<user-cache>` is `$HOME/Library/Caches/outershell` on macOS and `${XDG_CACHE_HOME:-$HOME/.cache}/outershell` on Linux.
- `<user-runtime>` is `$(getconf DARWIN_USER_TEMP_DIR)` on macOS and `${XDG_RUNTIME_DIR:-/run/user/$(id -u)}` on Linux.
- `<system-state>` is `/Library/Application Support/outershell` on macOS and `/var/lib/outershell` on Linux.
- `<system-cache>` is `/var/cache/outershell` on Linux.
- `<service-id>` is the backend service identifier, such as `org.outershell.Profile`.
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
- Linux user default: `<user-runtime>/outershelld-api`
- Linux direct-root default: `/run/outershelld-api`

Outer Shell log views fetch log files by the full path registered in `log_files.path`.

Linux systemd units use `StandardOutput=append:` and `StandardError=append:` for service logs. This requires systemd v240 or newer.

## Outer Shell User Install

macOS public install:

- `<user-state>/apps/org.outershell.OuterShell/Outer Shell.app/`
- `<user-state>/apps/org.outershell.OuterShell/Outer Shell.app/Contents/Resources/app-icon.png`
- `<user-state>/apps/org.outershell.OuterShell/Outer Shell.app/Contents/Resources/bundles/OuterShell.bundle.macos-arm.aar`
- `<user-state>/apps/org.outershell.OuterShell/Outer Shell.app/Contents/Resources/bundles/OuterShell.bundle.macos-x86.aar`
- `<user-state>/apps/org.outershell.OuterShell/version`
- `<user-state>/outershelld/outershelld`
- `<user-state>/outershelld/version`
- `<user-state>/bin/outerctl`
- `$HOME/Library/LaunchAgents/org.outershell.OuterShell.plist`
- `$HOME/Library/Logs/org.outershell.OuterShell/output.log`
- `<user-runtime>/org.outershell.OuterShell`
- `<user-runtime>/outershelld-api`

Linux public install:

- `<user-state>/outer-shell/OuterShellBackend`
- `<user-state>/outer-shell/bundles/`
- `<user-state>/outer-shell/app-icon.png`
- `<user-state>/outer-shell/version`
- `<user-state>/outer-shell/logs/OuterShellBackend.log`
- `<user-state>/outershelld/outershelld`, or a symlink to `<system-state>/outershelld/outershelld` when matching root support is installed
- `<user-state>/outershelld/version`
- `<user-state>/outershelld/logs/outershelld.log`
- `<user-state>/bin/outerctl`, or a symlink to `<system-state>/bin/outerctl` when matching root support is installed
- `$HOME/.config/systemd/user/org.outershell.OuterShell.service`
- `$HOME/.config/systemd/user/org.outershell.OuterShell.socket`
- `$HOME/.config/systemd/user/outershelld.service`
- `$HOME/.config/systemd/user/outershelld.socket`
- `<user-runtime>/org.outershell.OuterShell`
- `<user-runtime>/outershelld-api`

Linux public install when connected directly as root:

- `<system-state>/outer-shell/OuterShellBackend`
- `<system-state>/outer-shell/bundles/`
- `<system-state>/outer-shell/app-icon.png`
- `<system-state>/outer-shell/version`
- `<system-state>/outershelld/outershelld`
- `<system-state>/outershelld/version`
- `<system-state>/bin/outerctl`
- `/etc/systemd/system/org.outershell.OuterShell.service`
- `/etc/systemd/system/org.outershell.OuterShell.socket`
- `/etc/systemd/system/outershelld.service`
- `/etc/systemd/system/outershelld.socket`
- `/var/log/outershell/org.outershell.OuterShell.log`
- `/var/log/outershell/outershelld.log`
- `/run/org.outershell.OuterShell`
- `/run/outershelld-api`
- `<system-state>/system-binary-users/uid-0`
- `<system-cache>/outer-shell/install/` for downloaded public install/update artifacts while install or update is in progress
- `<system-cache>/outer-shell/bundled-apps/` for downloaded bundled app staging while install is in progress

In this direct-root Linux mode, Outer Shell treats bundled app installs as system installs. It should not create `/root/.local/state/outershell` or `$HOME/.config/systemd/user` units for the root account.

Downloaded bundled app staging directories are removed after a successful install. Failed installs can leave staging files behind for inspection or retry.

## Linux Root Support

A non-root Linux Outer Shell can promote `outershelld` and `outerctl` into system scope the first time it needs to install, uninstall, or update root-owned records. This installs the system `outershelld` API socket and rewires the user executable paths to the root-owned binaries:

- `<system-state>/outershelld/outershelld`
- `<system-state>/outershelld/version`
- `<system-state>/bin/outerctl`
- `/etc/systemd/system/outershelld.service`
- `/etc/systemd/system/outershelld.socket`
- `/var/log/outershell/outershelld.log`
- `/run/outershelld-api`
- `<user-state>/outershelld/outershelld -> <system-state>/outershelld/outershelld`
- `<user-state>/bin/outerctl -> <system-state>/bin/outerctl`
- `<system-state>/system-binary-users/uid-<uid>`

The `system-binary-users` directory contains blank marker files. A valid `uid-<uid>` marker means that user install is using the shared root binaries. A valid `uid-0` marker means the direct-root Outer Shell install is using them. A valid `root-apps` marker means one or more Linux root-installed bundled apps still depend on `<system-state>/bin/outerctl`. The shared Linux root support files are removed only when no valid marker remains.

Linux removes old helper files and units when installing current root support:

- `/usr/local/libexec/outershelld-root-helper`
- `/etc/systemd/system/outershelld-root-helper-<uid>.service`
- `/etc/systemd/system/outershelld-root-helper-<uid>.socket`

macOS root app installs promote the same `outershelld` and `outerctl` tools into system scope. Outer Shell itself remains a user LaunchAgent and is never installed as a root app. After promotion, user tool paths and the legacy root-tool path are symlinks to root-owned tools:

- `<system-state>/outershelld/outershelld`
- `<system-state>/bin/outerctl`
- `<user-state>/outershelld/outershelld -> <system-state>/outershelld/outershelld`
- `<user-state>/bin/outerctl -> <system-state>/bin/outerctl`
- `/usr/local/libexec/outershelld-root-tool -> <system-state>/outershelld/outershelld`

## Bundled Apps

Bundled app payloads use these generated files.

macOS user install:

- `<user-state>/apps/<service-id>/<app-name>.app`
- `<user-state>/apps/<service-id>/<app-name>.app/Contents/MacOS/<binary-name>`
- `<user-state>/apps/<service-id>/<app-name>.app/Contents/Resources/bundles/<bundle-prefix>.bundle.macos-arm.aar`
- `<user-state>/apps/<service-id>/<app-name>.app/Contents/Resources/bundles/<bundle-prefix>.bundle.macos-x86.aar`
- `<user-state>/apps/<service-id>/<app-name>.app/Contents/Resources/app-icon.png` when the app has a raster icon
- `$HOME/Library/LaunchAgents/<service-id>.plist`
- `$HOME/Library/Logs/<service-id>/output.log`
- `<user-runtime>/<service-id>` for socket-activated apps

macOS root install:

- `<system-state>/apps/<service-id>/<app-name>.app`
- `<system-state>/apps/<service-id>/<app-name>.app/Contents/MacOS/<binary-name>`
- `<system-state>/apps/<service-id>/<app-name>.app/Contents/Resources/bundles/<bundle-prefix>.bundle.macos-arm.aar`
- `<system-state>/apps/<service-id>/<app-name>.app/Contents/Resources/bundles/<bundle-prefix>.bundle.macos-x86.aar`
- `<system-state>/apps/<service-id>/<app-name>.app/Contents/Resources/app-icon.png` when the app has a raster icon
- `/Library/LaunchDaemons/<service-id>.plist`
- `/Library/Logs/<service-id>.log`
- `/var/run/<service-id>` for socket-activated apps

The LaunchDaemon sets `OUTERCTL_PATH` to `<system-state>/bin/outerctl`.

When root support is installed from a non-root macOS session for an app that also supports user mode, Outer Shell also writes `$HOME/Library/LaunchAgents/<service-id>.plist`, `$HOME/Library/Logs/<service-id>/output.log`, and `<user-runtime>/<service-id>`. That LaunchAgent points at the root-owned payload under `<system-state>/apps/<service-id>`; it does not install another copy under `<user-state>/apps/<service-id>`.

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

- `/opt/outershell/<service-id>/<binary-name>`
- `/opt/outershell/<service-id>/bundles/<bundle-prefix>.bundle.macos-arm.aar`
- `/opt/outershell/<service-id>/bundles/<bundle-prefix>.bundle.macos-x86.aar`
- `/opt/outershell/<service-id>/app-icon.png` when the app has a raster icon
- `/opt/outershell/<service-id>/version`
- `/etc/systemd/system/<service-id>.service`
- `/etc/systemd/system/<service-id>.socket` for socket-activated apps
- `/var/log/outershell/<service-id>.log`
- `/run/<service-id>` for socket-activated apps
- `<system-state>/system-binary-users/root-apps`

Linux root app services use `<system-state>/bin/outerctl` directly. A root install initiated from a non-root Outer Shell session first installs Linux root support, then writes the root app systemd unit with `OUTERCTL_PATH=<system-state>/bin/outerctl`.

When a Linux root install is initiated from a non-root Outer Shell session and the app supports both root and user mode, the installer also writes a user systemd unit that points at the root-owned payload:

- `$HOME/.config/systemd/user/<service-id>.service`
- `$HOME/.config/systemd/user/<service-id>.socket` for socket-activated apps
- `<user-state>/apps/<service-id>/backend.log`

Bundled app table:

| App | Service ID | Binary | Bundle Prefix | Root |
| --- | --- | --- | --- | --- |
| Top | `org.outershell.Top` | `TopBackend` | `TopContent` | optional |
| Files | `org.outershell.Files` | `FilesBackend` | `FilesContent` | optional |
| Plaintext | `org.outershell.Plaintext` | `PlaintextBackend` | `PlaintextContent` | optional |
| Network Inspector | `org.outershell.NetworkInspector` | `NetworkInspectorBackend` | `NetworkInspectorContent` | optional |
| Firehose | `org.outershell.Firehose` | `FirehoseBackend` | `FirehoseContent` | required |
| Profile | `org.outershell.Profile` | `ProfileBackend` | `ProfileContent` | optional |

## User-Created Backends

Backends created from the Add Apps UI are user installs, except in Linux direct-root mode. In that case they are system installs.

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

Linux direct-root:

- `<system-state>/apps/<service-id>/`
- `/var/log/outershell/<service-id>.log`
- `/etc/systemd/system/<service-id>.service`
- Generated recipe scripts live at the user-selected script path.

Custom backends that use Unix sockets or fixed ports register those frontends in `<system-state>/registry.orwa`.

## Uninstall Cleanup

Uninstalling Outer Shell removes its service unit, socket unit, launch agent, runtime sockets, registry entries, Outer Shell frontend payload, and cached installer files. It does not delete root-installed bundled app payloads. Before shutting down, `outershelld` checks whether the user registry contains anything other than Outer Shell itself; if not, user-scope uninstall also removes the empty user registry, `apps`, `bin`, and `outershell` state directories, plus the user `outer-shell` cache directory. On macOS, that same empty-registry uninstall path also removes an empty `/Library/Application Support/outershell` registry directory when no root app payloads remain, prompting for an administrator password if needed. On Linux, user-scope uninstall removes that user's root-support marker and prompts for a sudo password if elevated cleanup is needed. Direct-root uninstall removes `uid-0`. Shared root support remains while any `system-binary-users` marker is still valid. When the last valid marker is removed and no root app payloads remain, Linux removes the shared root support files, the system registry, and empty root log/state directories.

Uninstalling a bundled app removes its unit/plist, socket unit when present, app payload directory, cached archive/extracted installer payload, registry backend/app/log/opener records, and registered log path for the selected scope. On Linux, uninstalling the last root-installed app removes the `root-apps` marker; if that was the last root-support marker, shared root support is also removed.

Legacy paths may be migrated or removed during install:

- macOS legacy user root: `$HOME/Library/dev.outergroup.OuterLoop`
- Linux legacy user root: `$HOME/.outeragent`
- Linux legacy user systemd unit: `$HOME/.config/systemd/user/outeragent.service`
- Linux legacy Outer Shell root: `$HOME/.outerloop/outer-shell`
- macOS legacy system root: `/Library/dev.outergroup.OuterLoop`
- Linux legacy system root: `/var/lib/outershell/outeragent`
- Linux legacy root systemd unit: `/etc/systemd/system/outerloop-rootd.service`

After a legacy `registry.sqlite3` is migrated or superseded by `registry.orwa`, `outershelld` renames it to `registry.sqlite3.migrated` so it is not imported again.
