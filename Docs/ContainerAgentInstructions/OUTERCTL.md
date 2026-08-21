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
    (--socket-argument=OPTION | --tcp-port PORT) \
    -- /absolute/path/to/server [ARGUMENTS...]
```

Use exactly one transport:

- `--socket-argument=OPTION` starts the executable with an additional
  `OPTION=/run/user/0/org.outershell.image.ID` argument. Use this when the
  server can listen directly on a Unix socket.
- `--tcp-port PORT` starts the declared server and publishes its loopback TCP
  port through a Unix socket. The image must provide `bash` and `socat`.

The service starts eagerly with the container and is restarted after failure.
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
        --ServerApp.root_dir=/work
```

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

Outer Shell displays the declaration under **COMMAND LINE TOOLS**. Selecting it
copies a host command that opens the tool interactively inside this container.
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
