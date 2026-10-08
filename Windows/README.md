# Outer Shell for Windows

Outer Shell and Files are native C executables, with a statically linked runtime
and no CLR or managed DLL dependency. The Windows host uses the same C registry
model and ORWA persistence format as `outershelld`, plus the shared layout
validator. Files shares its binary listing encoder with its Unix backend. The
Windows adapter supplies named-pipe HTTP, process supervision, and SCM integration.
It serves the shared Outer Shell web app and starts a separate Files process.
No WSL or TCP listener is involved.

## Build and install

Place the `Backends` and `Files` repositories beside each other. Install Visual
Studio C++ Build Tools, its CMake/Ninja components, and LLVM. From Windows PowerShell:

```powershell
.\Windows\build.ps1
.\build\windows\install.ps1 -Start
```

`-FilesRoot`, `-VisualStudioRoot`, and `-OutputDirectory` override those locations.
`-Architecture arm64`, `x64`, or `x86` selects the target; the default is the host
architecture. `-CoreDirectory` uses an already-built directory containing both
executables. The production package contains `OuterShell.exe`, `FilesBackend.exe`,
and the shared web assets. `outershell-core.dll` is built only as a registry-test
harness and is not installed. The installer removes obsolete managed/runtime DLLs
when upgrading an existing installation.

Install as the ordinary desktop user. Files, registry data, logs, and layout live
in `%LOCALAPPDATA%\OuterShell`. Installation registers an interactive logon task
`OuterShell-<SID>`; `-Start` starts that task, so it survives an installing SSH
connection closing. The user must have an interactive logon session. The task
restarts the host after failures. A Windows job object kills Files if its owning host exits, including abrupt
termination. Service shutdown cancels pending named-pipe I/O and waits for workers.

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

The first Windows port does not implement Unix recipes/installers, containers,
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
checks, layout conflicts and persistence, Unicode Files listings, binary/chunked
transfers, controls, concurrency, and child cleanup. Run it under an account with
no existing Outer Shell process, because it uses that account's canonical pipes.
It uses temporary registry state and restores the account's prior allowlist.

`test_registry.py <matching-architecture-outershell-core.dll>` exercises persisted
app endpoints, binary/empty resources, backend removal, malformed requests, and
corrupt-file preservation. The shared Unix registry regression is
`Scripts/test_registry_list_responses.sh`. Outer Loop's WindowsSocketBridge tests
cover native ACL verification and multiplexed binary streams.

The implementation has been exercised on Windows 11 ARM and Windows 10 x64,
including Mac SSH bridge discovery/upload and real HTTP requests to the registry
and Files. The Windows helper is bundled by the macOS XPC service; the remote
machine needs neither Unix streamlocal forwarding nor a shell change.
