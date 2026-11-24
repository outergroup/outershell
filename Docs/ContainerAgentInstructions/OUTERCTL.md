# Publishing container tools with `outerctl`

`outerctl image` records portable declarations in an OCI image. These commands
are intended for Dockerfile `RUN` instructions: they do not need a running
`outershelld`, and they do not start software during the image build. When the
container starts, Outer Shell's internal service manager reads the declarations
and publishes them to the server-level Outer Shell.

## Publish an app

```dockerfile
RUN outerctl image add-app \
    --id ID \
    --name "Display Name" \
    [--url /initial/path] \
    [--working-directory /absolute/path] \
    [--icon-path /absolute/path/icon.png] \
    [--start manual|eager] \
    (--socket-argument=OPTION | --tcp-port PORT) \
    -- /absolute/path/to/server [ARGUMENTS...]
```

Use exactly one transport:

- `--socket-argument=OPTION` starts the executable with an additional
  `OPTION=/run/user/0/org.outershell.image.ID` argument. Use this when the
  server can listen directly on a Unix socket.
- `--tcp-port PORT` starts the declared server and publishes its loopback TCP
  port through a Unix socket. The image must provide `bash` and `socat`.

By default (`--start manual`), the service starts the first time the user
opens the app after the container starts. Use `--start eager` for a service
that should run whenever the container runs, such as one that does work in the
background. Either way, it is restarted after failure.
Its logs are exposed through Outer Shell. `--url` defaults to `/`, and
`--working-directory` defaults to `/root`.

Example:

```dockerfile
RUN outerctl image add-app \
    --id jupyter-work \
    --name "JupyterLab: work" \
    --url /lab \
    --working-directory /work \
    --socket-argument=--ServerApp.sock \
    -- /opt/venvs/default/bin/jupyter-lab \
        --allow-root \
        --no-browser \
        --ServerApp.sock_mode=0600 \
        --IdentityProvider.token= \
        --ServerApp.password= \
        --ServerApp.allow_remote_access=True \
        --ServerApp.root_dir=/work
```

Outer Loop passes the browser's host name through to the app, the way a reverse
proxy would. The app sees a `Host` header like
`<encoded-path>.unix.outerssh.localhost`, not `localhost`. Most servers accept
any host name, but some only accept `localhost` and reject everything else with
`403 Forbidden`. JupyterLab is one of them, so the example passes
`--ServerApp.allow_remote_access=True`. This is safe only because the server is
reachable just through the container's Unix socket or its loopback address. Don't
use it for a server on a published TCP port, especially with the token disabled.

Apps that check the browser's `Origin` instead need the exact address. Outer
Shell starts every app with it in `OUTERSHELL_APP_ORIGIN`, for example
`http://f5yhe2lw….unix.outerssh.localhost`. The address is specific to the
server and the container, so read it at startup rather than writing it into the
Dockerfile. A start script can pass it to the app's allowed-origins setting
before starting the app:

```dockerfile
RUN printf '%s\n' '#!/bin/sh' 'set -eu' \
        'example-app config set allowed-origins "$OUTERSHELL_APP_ORIGIN"' \
        'exec example-app serve --port 8000' \
        > /usr/local/bin/example-app-start \
    && chmod 0755 /usr/local/bin/example-app-start
```

`OUTERSHELL_APP_ORIGINS` lists every address when one app serves several
endpoints; `OUTERSHELL_APP_ORIGIN` is set only when there is exactly one.

## Publish an app in the running container

A declaration in the Dockerfile takes effect at the next rebuild. To publish
the same app right away, run the identical `outerctl image add-app` command in
the running container, then run the registration script it writes:

```sh
sh /etc/outershell/apps.d/org.outershell.image.ID.sh
```

The app then appears in Outer Shell and starts like an app declared in the
image. Running `outerctl image add-command` in the running container publishes
a command-line tool the same way, without a separate script.

## Publish a command-line tool

```dockerfile
RUN outerctl image add-command \
    --id ID \
    --name "Display Name" \
    [--working-directory /absolute/path] \
    [--user USER] \
    [--icon-path /absolute/path/icon.png] \
    -- /absolute/path/to/executable [ARGUMENTS...]
```

Outer Shell displays the declaration under the container's **Terminal
commands**. Selecting it opens the tool interactively inside this container,
either in Terminal or through a command you can copy.
The working directory defaults to `/root`, and the user defaults to `root`.

Example:

```dockerfile
COPY codex-cli.png /usr/local/share/outershell/icons/codex-cli.png

RUN outerctl image add-command \
    --id codex \
    --name "Codex CLI" \
    --working-directory /work \
    --user root \
    --icon-path /usr/local/share/outershell/icons/codex-cli.png \
    -- /usr/local/bin/codex --dangerously-bypass-approvals-and-sandbox
```

## Rules and inspection

- IDs contain 1–48 letters, digits, periods, underscores, or hyphens.
- Executables, working directories, and icon paths are absolute paths inside
  the image.
- Icon files must already exist when `outerctl image` runs. Common bitmap
  formats are supported; keep the file at or below 512 KB.
- Use a different stable ID for each distinct launcher.
- Add declarations to the Dockerfile. Running them only in a live container is
  not a reproducible configuration and does not replace an Outer Shell rebuild.
- `outerctl image list-commands` prints the command declarations in the current
  image for machine consumption.

The installed `outerctl` prints its usage synopsis when an invalid declaration
is rejected. Treat that binary as authoritative if it differs from this file.
