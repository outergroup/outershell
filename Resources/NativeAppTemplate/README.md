# Hello World

A full-stack [outerframe](https://outerframe.org) app: a backend that runs
locally or on a Linux server, serving a native macOS frontend to
[Outer Loop](https://outerloop.sh), registered as an app with
[Outer Shell](https://outershell.org).

The frontend is a CALayer-based macOS bundle generated as Swift or
Objective-C. The backend is generated as Go or C. It serves the `.outer`
descriptor, the platform bundle archives, and a small binary API that the
frontend calls over the SSH tunnel.

## Requirements

- **Mac**: Xcode (for the frontend bundle) and Docker Desktop (for reproducible
  Linux backend builds). No Apple ID or signing setup is needed; the bundle
  builds with code signing disabled. If Docker is unavailable, an SSH deploy
  of a C backend builds on the target instead when `cc` and `make` are
  installed there; it also uses `strip` when available.
- **Server**: Linux with systemd and [Outer Shell installed](https://outershell.org/install/).
- **Outer Loop** on the Mac, with an SSH connection to the server.

## Quick start

```bash
./app target "ssh -p 22 you@your-server"   # optional; generated projects are prefilled
./app deploy
```

Then open the server in Outer Loop. "Hello World" appears in its app
list; opening it starts the backend, downloads the frontend, and shows a
greeting fetched live from the server.

## Development loops

Local, no server involved (the fast loop):

```bash
./app run-local
# open http://127.0.0.1:8787/ in Outer Loop
```

Frontend-only update to the live server (no backend rebuild or restart):

```bash
./app push-frontend
```

Full redeploy after backend changes:

```bash
./app deploy
```

Build every Linux release variant:

```bash
./app build-matrix
```

C backends produce glibc and musl binaries for aarch64 and x86_64. glibc uses
the manylinux2014 (glibc 2.17) baseline and musl uses musllinux 1.2. Go
backends are built once per architecture because `CGO_ENABLED=0` makes them
independent of the target libc. Release binaries are stripped.

Watch the backend:

```bash
./app logs
./app status
```

Remove everything from the server:

```bash
./app uninstall
```

## Layout

```
app                 task runner; all build/deploy logic lives here and in deploy/
app.env             app identity and frontend/backend languages -- single source of truth
target.env          deploy target kind + optional ssh argv (gitignored)
frontend/           Xcode project producing HelloFullstack.bundle
  Frontend/         the app's own code (start here)
  Vendor/           outerframe host plumbing (socket protocol, layer registration)
  Scripts/          generate_outer.py writes the .outer descriptor
backend/            HTTP server; Go projects include a pinned toolchain Dockerfile
deploy/             systemd unit template + scripts that run on the server
```

## How it fits together

1. `./app build` compiles the bundle with xcodebuild, thins it per
   architecture, packs each slice as an Apple Archive, writes `app.outer`
   (pointing at `/frontend`), and builds the backend for the selected deploy
   target. For C backends it detects glibc versus musl and uses the matching
   manylinux2014 or musllinux 1.2 toolchain.
2. `./app deploy` installs locally when `OUTER_TARGET_KIND=local`. For SSH
   targets, it streams the payload as a tarball over your exact SSH command (no
   scp/rsync assumptions), then streams `deploy/remote-install.sh`, which swaps
   the payload into `~/.local/share/outershell-apps/<app-id>/`, installs a
   systemd user unit, and registers the backend and app with `outerctl`.
3. The backend listens on a Unix domain socket in `$XDG_RUNTIME_DIR`; Outer
   Shell knows the socket path and starts the service on demand. No ports to
   choose, no port collisions.
4. When you open the app, Outer Loop fetches `/` (the `.outer` descriptor),
   downloads `/frontend/macos-arm`, and runs the bundle in a sandboxed
   process. The frontend then calls `/api/hello` through the SOCKS proxy,
   i.e. over the same SSH connection.

## Backend API style

The generated `/api/hello` endpoint intentionally does not use JSON. Outer
Shell apps usually use small little-endian binary messages so the backend can
be written in C without bringing in a parser dependency. The endpoint uses
`Content-Type: application/octet-stream`; the endpoint path identifies the payload
format. The sample response is:

```
8 bytes   little-endian uint32 offset, uint32 length for message
8 bytes   little-endian uint32 offset, uint32 length for hostname
8 bytes   little-endian uint32 offset, uint32 length for os
8 bytes   little-endian uint32 offset, uint32 length for time
N bytes   UTF-8 string data referenced by those records
```

For your real app, prefer similarly explicit binary records over generic text
serialization unless a human-editable format is part of the product.

## Renaming this app

Edit `app.env`. The one identity baked into the frontend project is the Xcode
target name (`HelloFullstack`); if you rename the target, scheme, and
`Frontend/HelloFullstackContent.swift` class, update `XCODE_SCHEME` to match.
Then `./app deploy` registers the new id and `./app uninstall` (before
renaming) removes the old one.

`BACKEND_LANGUAGE` is generated as either `go` or `c`. It controls the backend
build path used by `./app`; changing it after generation means replacing the
contents of `backend/` with an implementation in that language.
`FRONTEND_LANGUAGE` records whether this project was generated from the Swift
or Objective-C frontend template.
