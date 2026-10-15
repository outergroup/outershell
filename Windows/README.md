# Outer Shell for Windows

Outer Shell and Files are native C executables, with a statically linked runtime
and no CLR or managed DLL dependency. The Windows host uses the same C registry
model and ORWA persistence format as `outershelld`, plus the shared layout
validator. Files shares its binary listing encoder with its Unix backend. The
Windows adapter supplies named-pipe HTTP, process supervision, Task Scheduler, and optional SCM integration.
It serves the macOS outerframe frontend to Outer Loop, the shared HTML frontend
to web clients. The normal user installation runs Files independently.
Native Windows apps require neither WSL nor a TCP listener. WSL distributions can
optionally expose their Linux apps through the same Windows host.

## Build and install

Place the `Backends` and `Files` repositories beside each other. Install Visual
Studio C++ Build Tools, its CMake/Ninja components, and LLVM. Build the frontend
archives on a Mac first:

```sh
bash Windows/build-outerframe-bundles.sh
```

Copy `build/outerframe/bundles` to the Windows build machine. From Windows PowerShell:

```powershell
.\Windows\build.ps1 -BundlesDirectory C:\path\to\bundles
.\build\windows\install.ps1 -Start
```

`-FilesRoot`, `-VisualStudioRoot`, and `-OutputDirectory` override those locations.
`-Architecture arm64`, `x64`, or `x86` selects the target; the default is the host
architecture. `-CoreDirectory` uses an already-built directory containing both
executables. The production package contains `OuterShell.exe`, `FilesBackend.exe`,
both macOS outerframe archives, and the shared web assets. `outershell-core.dll` is built only as a registry-test
harness and is not installed. The installer removes obsolete managed/runtime DLLs
when upgrading an existing installation.

Install as the ordinary desktop user. Files, registry data, logs, and layout live
in `%LOCALAPPDATA%\OuterShell`. Installation registers two interactive-user tasks:
`OuterShell-<SID>` and `OuterShell-Files-<SID>`. Neither has a logon trigger;
`-Start` optionally starts Outer Shell immediately. The user must have a desktop
session. No password is stored. Tasks ignore duplicate launch requests, have no
execution time limit, and can recover after failures.

Both executables use the Windows GUI subsystem with a shared background entry
point, so Task Scheduler never allocates a console window for them. Command
arguments, exit codes, and redirected standard output remain available. Run
`Windows/test-background-launch.ps1` as the signed-in user after installation to
check the executable subsystem, task launches, console absence, and HTTP responses.
The test restarts both core tasks.

The installer writes pipe-to-task mappings to
`%LOCALAPPDATA%\org.outerssh\activations.tsv`. Opening an allowlisted endpoint
through Outer Loop's Windows Localhost mode or Windows SSH bridge activates its
task if its pipe is absent. Files survives Outer Shell exit. Both apps exit
normally after five minutes without active named-pipe connections; active WSL
relays count as activity for Outer Shell. Idle exits leave tasks enabled.
Manual Stop disables a task; Start or Restart re-enables it.

`activations.tsv` is UTF-8 without a BOM, one full pipe path and absolute task
path per line separated by a literal tab. Blank lines and LF/CRLF are accepted.
No quoting, escaping, or comments are supported. Duplicates (case-insensitive)
and malformed rows are rejected. The maximum size is 1 MiB. The bridge validates
file/directory ownership and ACLs separately from the HTTP allowlist. Writers
atomically replace the file while holding `activations.lock` in the same folder.

The optional `-SystemService` deployment retains its existing SCM behavior and
host-owned Files process; it does not install SYSTEM-account tasks.

The exact registered HTTP endpoints are added to
`%LOCALAPPDATA%\OuterSSH\http-pipe.allow`:

```text
\\.\pipe\org.outershell.outershell.<lowercase-user-SID>
\\.\pipe\org.outershell.files.<lowercase-user-SID>
```

`OuterShell.exe --pipe` prints the canonical shell endpoint. The binary registry
API is `\\.\pipe\org.outershell.outershelld.<SID>`, with the existing length-prefixed
Outer Shell protocol. It is not an HTTP allowlist entry. Backend/app/log
registration and removal, listing, and binary resource operations are implemented.
Other registry operations return explicit unsupported-operation errors.

The pipe owner is its SID, with a protected DACL allowing that user and SYSTEM.
Remote SMB clients are rejected and first-instance creation rejects occupied
names. Outer Loop independently verifies the allowlist and the connected pipe's
owner and security-changing/server-creation rights. A SID in the name alone is
not a verified identity. Direct Windows navigation can show the verified account;
the macOS client receives the verified account/app label from the Windows bridge.

## Remote SSH access

Outer Loop syncs its bundled native bridge over SSH for each session. The helper
is not installed by Outer Shell and does not require a PATH entry. Elevated SSH
sessions stage the helper under ProgramData with administrator-only write access;
ordinary sessions use a private LocalAppData directory. Outer Loop removes its
session copy on disconnect.

Discovery, allowlist checks, and forwarding use native code. Set Windows
OpenSSH's default shell to `cmd.exe` for the lowest launch overhead. PowerShell
also works, but OpenSSH starts that configured shell for each native process.
Neither mode requires per-session uploads or nested PowerShell scripts.

## System service

From elevated PowerShell:

```powershell
.\build\windows\install.ps1 -SystemService
```

This installs a demand-start SCM service named `OuterShell` in Program Files.
Opening its shell or registry pipe triggers service startup. `-Start` starts it
immediately. The service runs as SYSTEM; its canonical SID is `s-1-5-18`, and its
state is in SYSTEM's LocalAppData. Its default pipe ACL grants no ordinary-user
access. Installing it does not grant users access to SYSTEM's Files process.
Use the per-user install for normal browsing.

## Features and current boundaries

The web home lists real registered backends and apps. Files supports local drive
and mounted-share browsing, text/media previews, downloads, bounded uploads, and
folder creation. Its home listing uses the Windows known Desktop folder, including
Parallels' `C:\Mac\Home\Desktop` redirection. Files start/stop/restart and enabled
state are available from Outer Shell. Registered SCM services can expose controls
when their registry record declares ownership.

The Windows host does not implement Unix recipes/installers, container creation,
content-type/openers registration, or cross-app Open With. Unsupported actions
return errors. User app registry changes add allowlist entries; removing an app
does not revoke an independently granted allowlist entry. Edit the allowlist to
revoke access.

HTTP uses one request per connection, at most 64 concurrent requests, 64 KiB
headers, 16 MiB request bodies, and a 35-second inactivity timeout. Content-length
and chunked uploads are supported. Downloads stream; previews are capped. There
are no WebSocket upgrades or HTTP range responses. File access runs with the host
account's normal filesystem permissions; Files is not a filesystem sandbox.

## Validation

`test_host.py <path-to-packaged-OuterShell.exe>` exercises HTTP framing, origin
checks, outerframe negotiation and both macOS bundle downloads, layout conflicts
and persistence, Unicode Files listings, binary/chunked
transfers, controls, concurrency, and Files independence. Install the package first
so the Files task exists. Run it under an account with
no existing Outer Shell process, because it uses that account's canonical pipes.
It uses temporary registry state and restores the account's prior allowlist.

`test_registry.py <matching-architecture-outershell-core.dll>` exercises persisted
app endpoints, binary/empty resources, backend removal, malformed requests, and
corrupt-file preservation. The shared Unix registry regression is
`Scripts/test_registry_list_responses.sh`. Outer Loop's WindowsSocketBridge tests
cover native ACL verification and multiplexed binary streams.

The implementation has been exercised on Windows 11 ARM and Windows 10 x64,
including Mac SSH native bridge discovery and real HTTP requests to the registry
and Files. The Windows helper is bundled by the macOS XPC service; the remote
machine needs neither Unix streamlocal forwarding nor a shell change.

## WSL distributions

The per-user Windows host discovers distributions registered to that Windows
account. Each appears as a separate WSL group, with apps belonging to the
configured default Linux user. Windows and Linux identities remain distinct;
the homepage groups apps under the distribution name. On macOS, WSL app links
show labels such as `Ubuntu / mrcslws / Files` after the Windows bridge verifies
the pipe identity. Editing the address still exposes the underlying pipe URL.
The Windows client currently displays the verified Windows owner and endpoint
identifier. SYSTEM's host does not
inherit the desktop user's WSL registrations.

Discovery inspects running distributions and leaves stopped ones asleep. Start
and Stop control the entire distribution (`wsl --terminate` stops all its
processes); app controls use the Linux Outer Shell registry and systemd. Start
holds a lightweight Linux stdin reader open so WSL keeps the distribution running;
Stop and host shutdown release that session. A stopped
distribution retains its discovered app list until the Windows host restarts.
Installing, importing, deleting, and changing the default user of distributions
remain WSL administration tasks.

For each registered Linux Unix socket, the host publishes a stable Windows pipe:

```text
\\.\pipe\wsl.<128-bit-endpoint-digest>.<lowercase-Windows-user-SID>
```

The digest identifies the distribution registration, Linux UID, and socket path.
It is a routing identifier, not an access credential. Pipes use the same protected
Windows ACL and exact-path allowlist as native apps. Each connection launches
`wsl.exe --distribution NAME --exec /usr/local/bin/outer-socket-bridge bridge
--socket PATH`. The Linux bridge independently checks its Unix allowlist and socket
ownership on every connection. No Linux SSH server, exposed port, or cross-OS
filesystem socket access is required. The relay carries bytes in both directions;
Linux Outer Shell registry responses have their socket addresses translated to
Windows pipes so nested homepage links work.

### Install Linux support

Use a systemd-enabled WSL 2 distribution. As its ordinary Linux user, run:

```sh
bash Backends/Windows/install-wsl-support.sh
```

This invokes the standard Linux release installer, then installs the packaged
Linux socket bridge under `/usr/local/bin` and enables user lingering with sudo
when needed. It does not require a compiler or a Files source checkout. Outer
Shell uses the normal Linux registry, installation paths, systemd socket units,
app catalog, and app installers. Top, Plaintext, Files, and the other bundled
apps are installed through **Add more…**, using the same release archives as any
other Linux host. Their backends run in Linux and serve the normal macOS
outerframe bundles to Mac clients and HTML where supported.

For development, build the ordinary Linux release package and pass its installer
and archive explicitly:

```sh
bash Backends/Windows/install-wsl-support.sh /path/to/install.sh /path/to/outer-shell-linux-x86_64-musl.tar.gz
```

The archive override supports the same static Linux packages as `./app deploy`.
The Windows host only supplies distribution discovery and transport to the
Linux sockets; it does not supply a separate WSL app catalog. Linger makes the
user manager available when WSL starts without an interactive Linux login.
Refresh the Windows homepage after installation. `OuterShell.exe --wsl-list`
prints a diagnostic snapshot.

The adapter exposes Unix-socket apps registered with the distribution's default
user, including system-scoped apps. System-scoped endpoints launch the trusted
Linux bridge with `wsl --user root`; the bridge still enforces the root-owned
`/etc/outerssh/http-unix.allow`. User endpoints run as the default Linux user.
The Windows account already owns the distribution and can launch WSL as root.
The homepage presents separate WSL user and root cards. App service and frontend
IDs remain unchanged; display caches and ordering are scoped to each card.
Each WSL card exposes **Add more…** directly in the Windows homepage. The
Windows API forwards catalog, recipe, and installation requests to the standard
Linux Outer Shell, including its administrator-password flow. Opening the
catalog starts a stopped distribution. The inner Outer Shell launcher is hidden;
its backend remains responsible for Linux app management. WSL app menus include
scoped uninstall actions. Running indicators reflect the Linux service process;
a listening socket alone does not mean the app is running.
App controls carry service scope, and system controls use the registered systemd
unit through `wsl --user root`, without an interactive sudo prompt.
The adapter supports at most 256 published endpoints and 64 simultaneous relay
connections.
Discovery refreshes every five seconds while the homepage is being observed.
It does not install support automatically or expose Linux root as Windows SYSTEM.
Cross-app Open With and WSL recipe/mount/shell editors are not implemented.

### WSL validation

With a configured Windows host and a Windows bridge executable:

```sh
python3 Windows/test_wsl.py marcbook-windows 'C:\path\to\outer-socket-bridge.exe' Ubuntu
```

This test starts and stops the selected distribution. It checks discovery, app
controls, native Windows and WSL homepage bundles, nested homepage routing,
standard Files, Top, and Plaintext native bundles, binary/Unicode transfers, Linux allowlist
revocation (restored afterward), stopped-distribution discovery, and reconnection.
Outer Loop's Windows named-pipe browser integration test additionally launches
WSL Files from its own group through the Localhost AppContainer and verifies the
Linux home directory and verified Windows pipe identity.

## Commands

On a Windows user card, **Add more… → Run commands** registers an independent,
least-privilege interactive task. Supply a display name, identifier, multiline
`cmd.exe` script, and TCP port (the default) or named pipe. The command script must keep running until the server exits. For example, use `python server.py`, not `start python server.py`. The form
uses a POST body and supports UTF-8 commands; working directory defaults to the
user profile. Configure the app itself to listen on the specified endpoint. TCP endpoints use
`127.0.0.1`; no firewall rule or public listener is created by Outer Shell.

Scripts, a private runner copy, and output logs live beneath
`%LOCALAPPDATA%\OuterShell\commands\org.outershell.command.<identifier>`.
The runner contains the process tree in a Windows job object. Start/stop/restart,
logs, and uninstall are exposed in the backend menu. Uninstall removes the task,
activation registration, script, runner, log, and only allowlist entries owned
by that registration. Commands start when created and are launched explicitly
through Outer Shell thereafter. They do not add task activation mappings: opening
a stopped command endpoint does not launch it. Built-in Outer Shell and Files
retain their independent bridge activation mappings.
Administrator-scope command registration is not offered by this form.

`test_task_activation.py <installed-OuterShell.exe>` checks cold and concurrent
activation, independent Files lifetime, invalid registrations, and allowlist
checks. `test_commands.py <OuterShell.exe>` tests command creation, Unicode,
logs, process-tree cleanup, persistence, restart, and uninstall. Run both as the
signed-in desktop user, with existing same-user hosts stopped for the latter.


### Administrator backends

The built-in Windows experience creates ordinary-user tasks only. It does not
provide an Administrator card, elevated command setup, or a UAC setup utility.
Previously created administrator tasks are not deleted or automatically surfaced.
Custom backends registered explicitly through the registry API remain supported;
their privileges and named-pipe permissions are the responsibility of their owner.
Linux root backends inside WSL retain the normal Linux authorization flow.

### Windows beta installer

`OuterShell.iss` creates the per-user setup executable. It installs Outer Shell,
Files, and their web/native frontend assets, and uses the bundled native setup
helper to register InteractiveToken/LeastPrivilege tasks and pipe activation
mappings. The SSH bridge belongs to Outer Loop, is synced by the client over SSH,
and is not part of this installer. No PowerShell is used by setup itself.

Build the backend first, sign its two executables, then run:

```powershell
.\Windows\package-installer.ps1 -PayloadDirectory .\build\windows -Architecture x64 -Version 0.1.0 -SignCommand '<SignTool command ending with $f>'
```

Use `-DevelopmentUnsigned` instead of `-SignCommand` only for manual testing.
Automatic installation refuses unsigned packages and builds without a configured
publisher. Repeat packaging for arm64 and x86. Publish the signed outputs as:

- `/outer-shell/latest/outer-shell-windows-x64-setup.exe`
- `/outer-shell/latest/outer-shell-windows-arm64-setup.exe`
- `/outer-shell/latest/outer-shell-windows-x86-setup.exe`

Serve these directly over HTTPS, without redirects. Setting
`WINDOWS_INSTALLER_ROOT` when running `scripts/package_release.sh` adds these
three prebuilt installers to its `latest` output directory.

Double-click setup for interactive installation. Outer Loop uses
`/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-`. Installation is per-user even
when started by an elevated SSH session; the resulting tasks run unelevated.
An interactive Windows login is still required for task activation.

Upgrades stop the two core tasks before replacing program files and register them
again afterward. Run setup again to repair a failed task-registration step.
Uninstall removes the core tasks, their activation mappings, and installed program
files, while retaining registry data, user-created command apps and output logs.
Run the per-user uninstaller unelevated; it refuses elevation so it cannot execute
a user-writable maintenance helper with administrator privileges.

See `OuterLoop/WindowsInstaller/README.md` in the Outer Loop repository for signing
service setup, publisher configuration and signature-verification tests.

### Windows updates

Outer Shell checks `/outer-shell/latest/windows-version.txt` when the homepage
requests `/api/backends`, at most once per day. The attempt timestamp persists across
restarts, including failed checks. There is no startup check or periodic timer; an
untouched server makes no update requests. Explicit Check for Updates bypasses the
automatic daily limit. The existing HTML and outerframe Check for Updates actions use the same
version metadata and control responses as Unix. Windows release versions are three
numeric components from `VERSION`; package generation uses that same version.
The release packaging script emits the Windows version marker only when all three
Windows installers are supplied. Publish the marker after the corresponding files.

Updates run as the ordinary user. The backend downloads the native-architecture
installer over HTTPS, rejects redirects and oversized responses, verifies its
Authenticode chain and Marcus Lewis publisher, and checks its file version against
the advertised release. It registers a separate least-privilege Task Scheduler job
using a staged copy of Outer Shell. This runner re-verifies the installer, waits for
installation to finish, records the result, and restarts the main task. It survives
the installer stopping the old backend. Failures appear in the backend status after
reconnection. Existing registry data and command tasks remain in place. Elevated
legacy service instances cannot invoke this per-user update path.

The `OUTERSHELL_UPDATE_TESTS` CMake option builds `OuterShellUpdateFixture` for
local signature rejection, version binding, and scheduled upgrade tests. Its local
payload injection is confined to the test binary; the production API accepts no
custom download URL, executable path, publisher, or installer arguments.

## WSL containers

Windows also discovers Linux containers created with Microsoft's `wslc.exe`
(WSL 2.9.3 or later). This is separate from WSL distribution discovery. Docker
is not required. The native host invokes the CLI directly, without PowerShell,
and uses `exec --interactive` to carry the existing socket-bridge protocol.
Each app receives a user-owned, allowlisted Windows named pipe with a Task Scheduler mapping that restarts Outer Shell after idle exit. The inner Linux
service manager and app images are shared with the other container platforms.

Outer Shell uses the ordinary interactive user's default WSL container session.
WSL containers has a separate default session for elevated processes, including
an elevated SSH shell. Containers created there do not appear in the ordinary
user's Outer Shell. Use an unelevated local terminal for manual container work.

The overview supports discovery, app opening and start/stop/restart, container
start/stop, and creation from OCI images with CPU and memory limits. Containers
created by Outer Shell can also be renamed and deleted; externally created
containers are attached without claiming ownership. Display names and limits
are stored as newline-delimited UTF-8 in `container-names` under the user's
Outer Shell directory. Runtime diagnostics go to `container-runtime.log`.

For a release payload, build static Linux `outershelld`, `outerctl`, and
`outer-socket-bridge` for the target Linux architecture. Run
`build-container-bootstrap.ps1 -LinuxBinariesDirectory ... -OutputDirectory ...`,
then pass that directory as `-ContainerBootstrapDirectory` to `build.ps1`.
The installer copies this optional image-building payload. These Linux tools
run inside containers; the Windows SSH helper continues to be synced by Outer
Loop and is not installed. The default image is built locally on first creation.

This first Windows adapter does not yet implement the Dockerfile editor,
rebuilds, mount configuration, image sharing, or terminal launcher discovery.
These capabilities are marked unavailable in snapshots. Custom images can use
the same `outerctl image add-app` and OCI app payload contract documented in
`Docs/ContainerCookbook.md`. Container runtime support should not be confused
with Windows-kernel containers; these are Linux containers.

For integration coverage, build an image from the prepared base with
`COPY --from=ghcr.io/outergroup/outershell-app-files:4 /outershell-rootfs/ /`,
tag it `outershell/windows-test:1`, and run `test-containers.ps1` on Windows.
The test creates and removes its own disposable container and covers native
Files responses, app controls, container controls, names, and logs.
