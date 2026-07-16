# Hello World

A multi-platform [outerframe](https://outerframe.org) app. One server exposes a
shared API and negotiates between independent HTML and macOS implementations.
The enabled platforms and server isolation policy live in `app.env`.

This is deliberately multi-platform, not cross-platform: `html/` is ordinary
HTML/CSS/JavaScript, while `macos/` is a compiled CALayer-based bundle written
in Swift or Objective-C. They share behavior through `server/`; they do not
share a UI toolkit.

## What happens at `/`

- A normal browser receives `html/index.html`.
- Outer Loop sends `Outerframe-Accept: application/vnd.outerframe`. When the
  macOS target is enabled, the server returns `app.outer`, and Outer Loop then
  fetches `/frontend/macos-arm` or `/frontend/macos-x86`.
- Outer Loop’s **Develop → Outerframe → Always Use HTML** option suppresses the
  capability header, so the same URL loads the HTML implementation.

If only one platform is enabled, the server behaves predictably: an HTML-only
app always serves HTML, while a macOS-only app returns `406 Not Acceptable` to
ordinary browsers.

## Project layout

```text
app                 build, development, deployment, and sync commands
app.env             app identity, enabled platforms, isolation, languages
html/               HTML implementation, when enabled
macos/              Xcode bundle project, when enabled
server/             shared HTTP server and API, generated as Go or C
artifacts/          compiled platform payloads; macOS builds land here
deploy/             host/container service definitions and install scripts
Dockerfile          runtime image, for containerized apps
```

This directory is user-owned canonical source. New App defaults to
`~/outerframe-apps/<project>` on the server, but its Project Location picker can
put the source root anywhere writable by your server account. It is an ordinary
project folder: open it over SSH, let coding agents edit it there, initialize a
Git repository, and push it to the Git host of your choice. The HTML
implementation lives beside the server, while platform build machines return
compiled output to `artifacts/`.

The running app never reads files directly from this source directory.
`./app deploy` builds a snapshot and installs it under Outer Shell's private
runtime directory (or bakes the snapshot into the container image). Source
edits become live only after another deploy. `./app run-local` is the explicit
live-source development path.

`artifacts/` is also part of the canonical project contract, but its generated
contents are gitignored. A Mac publishes `app.outer` and compiled frontend
archives there; the next deployment includes them in the runtime snapshot or
container image. Keep `artifacts/.gitkeep` so the handoff location exists in a
fresh clone.

Outer Shell also creates a small macOS builder folder when macOS is enabled.
That folder is intentionally not a copy of this project. It contains the Xcode
source needed by a Mac plus three commands:

```bash
./platform sync       # replace local macos/ with this server's source
./platform build      # compile locally with Xcode
./platform publish    # send only compiled macOS artifacts back here
```

Publishing calls `./app accept-platform macos` on this server. Host-mode apps
update their installed payload; containerized apps rebuild so the new assets
are included in the image. This same contract can support future Windows and
other platform builders without moving the canonical project.

## Local development

Run all selected implementations through the same local server:

```bash
./app run-local
```

Open the printed URL in a normal browser for HTML or in Outer Loop for the
native macOS implementation. If macOS is enabled, Xcode is required.

For a containerized app, exercise the exact public-web image locally:

```bash
./app run-container
```

## Private Outer Shell deployment

New App installs this project on the current server and performs the first
deployment automatically. Later, from this directory on the server, run:

```bash
./app deploy
```

Host-mode apps install a native binary as a systemd user service. Containerized
apps build their image on this host, run with a read-only root filesystem,
and receive only two writable mounts: persistent `/data` and the runtime
directory containing the Outer Shell Unix socket. The server needs Docker,
systemd, and Outer Shell.

The deployed service is disposable output. Make lasting changes in the
canonical Project Location chosen in New App, then deploy again; do not edit
`~/.local/share/outershell-apps`.

Platform build machines publish through the narrow artifact command:

```bash
./app accept-platform macos /path/to/incoming-artifacts
```

For containerized apps this rebuilds and restarts the image, because platform
assets are part of the image.

## Open-web deployment

Containerized apps use the same standard Dockerfile on private hosts and
public container platforms. The entrypoint listens on `$PORT` when supplied,
or port 8080 otherwise. The image and task runner do not select or require a
hosting provider.

Web deployment is a small Bash plugin interface. List the adapters available to
this project, then choose one explicitly:

```bash
./app web-providers
./app deploy-web railway
```

Railway is included as one optional adapter; it is not the default. Additional
adapters can be checked into `deploy/web-providers/<name>`, installed for the
current user at `~/.config/outerframe/web-providers/<name>`, or exposed on
`PATH` as `outerframe-web-provider-<name>`.

The dispatcher stages every selected platform, then invokes the adapter with
`deploy` as its first argument. It exports `OUTERFRAME_PROJECT_ROOT`,
`OUTERFRAME_APP_ID`, `OUTERFRAME_APP_NAME`, `OUTERFRAME_CONTAINER_IMAGE`,
`OUTERFRAME_CONTAINER_PORT`, and `OUTERFRAME_DOCKERFILE`. Remaining command-line
arguments are forwarded unchanged, so provider-native project, service, and
environment options remain available:

```bash
./app deploy-web railway --service my-service --environment production
```

An adapter owns provider authentication, upload, status, and domain behavior.
It exits with the provider command's status. Outer Shell and `outerctl` are not
part of this path; the canonical folder remains deployable over an ordinary SSH
shell or by a coding agent.

## Useful commands

```text
./app build             build the selected platforms and server/container
./app build-platforms   stage every selected platform
./app deploy            build and deploy from this server project
./app accept-platform   accept output from a platform build machine
./app web-providers     list installed web deployment adapters
./app deploy-web NAME   deploy through a selected provider adapter
./app logs              follow private-server logs
./app status            inspect the private service
./app uninstall         remove the private deployment
./app clean             remove local build products (not artifacts/)
```

## Shared API

Both starter implementations call `GET /api/hello`. The endpoint uses four
little-endian offset/length string records rather than JSON, keeping the
contract equally natural for C, Swift, Objective-C, Go, and JavaScript.
