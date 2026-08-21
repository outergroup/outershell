# Outer Shell Portable Services

`outershelld` can supervise services directly on Unix hosts where neither launchd nor an operational systemd instance is available. Current release packaging enables this for containers and minimal Linux distributions. The service format and supervisor avoid Linux service-manager APIs so the same model can be used by future native FreeBSD and OpenBSD packages.

The installer selects a manager as follows:

1. macOS uses launchd.
2. Other hosts use systemd when `systemctl` exists, `/run/systemd/system` exists, and `systemctl show-environment` succeeds in the selected user or system scope.
3. Otherwise the installer starts `outershelld` with its internal service manager.

Set `OUTERSHELL_SERVICE_MANAGER=systemd` or `OUTERSHELL_SERVICE_MANAGER=internal` to override automatic selection. A forced but non-operational systemd selection is an install error.

## File Format

Each service is one UTF-8 text file named `<service-id>.outerservice`. The service identifier is the filename without the suffix and may contain ASCII letters, digits, `.`, `_`, and `-`.

The format is deliberately stricter than a general INI parser:

- Sections and keys are case-sensitive.
- Blank lines and whole-line comments beginning with `#` or `;` are ignored.
- A setting is `Key=Value`, split at the first `=`. Surrounding whitespace is removed.
- There are no quoted strings, escapes, inline comments, substitutions, or includes.
- Unknown keys, unknown sections, invalid values, and repeated scalar keys are errors.
- `Argument`, `Environment`, and `PassEnvironment` are ordered list keys and may repeat.
- Paths must be absolute. An argument occupies exactly one `argv` element; use an explicit `/bin/sh` executable when shell evaluation is wanted.

Example:

```ini
[Service]
Format=1
Name=Example HTTP service
Executable=/opt/example/bin/server
Argument=--listen-fd
Argument=3
WorkingDirectory=/opt/example
Environment=EXAMPLE_MODE=production
PassEnvironment=TZ
EnvironmentPolicy=clean
Start=socket
Restart=on-failure
RestartDelayMilliseconds=1000
StopTimeoutMilliseconds=10000
LogPath=/var/log/example.log
Essential=false

[Socket.http]
Type=unix
Path=/run/example.sock
Mode=0600
Backlog=64
```

`[Service]` keys:

| Key | Meaning | Default |
| --- | --- | --- |
| `Format` | Required format version; currently `1` | required |
| `Name` | Human-readable name | service identifier |
| `Executable` | Absolute executable path | required |
| `Argument` | One argument, in declaration order | none |
| `WorkingDirectory` | Absolute child working directory | unchanged |
| `Environment` | One `NAME=value` child environment entry | none |
| `PassEnvironment` | Copy one named variable from `outershelld` | none |
| `EnvironmentPolicy` | `clean` or `inherit` | `clean` |
| `Start` | `manual`, `eager`, or `socket` | `manual` |
| `Restart` | `never`, `on-failure`, or `always` | `on-failure` |
| `RestartDelayMilliseconds` | Delay before restart, at most one hour | `1000` |
| `StopTimeoutMilliseconds` | SIGTERM grace period before SIGKILL | `10000` |
| `LogPath` | Absolute combined stdout/stderr append log | inherited |
| `Essential` | Shut down the internal manager if this service exits permanently | `false` |

A socket section is named `[Socket.<descriptor-name>]`. Unix sockets use `Type=unix` and an absolute `Path`. IPv4 TCP sockets use `Type=tcp`, `Address` (default `127.0.0.1`), and `Port`. `Mode` applies to Unix sockets; `Backlog` defaults to 64.

## Activation and Lifecycle

The manager binds activation sockets before starting children. A `Start=socket` service remains dormant until a listener becomes readable, so the pending connection stays queued while the child launches.

Children receive activation descriptors starting at file descriptor 3 and the systemd-compatible variables `LISTEN_PID`, `LISTEN_FDS`, and `LISTEN_FDNAMES`. `LISTEN_FDNAMES` follows socket-section declaration order. Each child runs in its own process group; stop sends SIGTERM to that group and SIGKILL after the configured timeout. `outershelld` reaps children and applies the configured restart policy.

Bundled-app installation updates one definition at a time. A new app becomes available immediately; replacing or uninstalling an existing app stops only that app. Other managed children, including the Outer Shell frontend serving the install request, continue running.

Register portable services with:

```sh
outerctl backend upsert \
  --backend org.example.Service \
  --name "Example" \
  --service-file /absolute/path/org.example.Service.outerservice \
  --outershell-owns true
```

When authoring a container image, `outerctl image add-app` writes the service
file and startup registration without contacting a running `outershelld`:

```dockerfile
RUN outerctl image add-app \
    --id example \
    --name "Example" \
    --socket-argument=--socket \
    -- /opt/example/bin/server
```

Use `--tcp-port PORT` instead of `--socket-argument` for a loopback TCP server.
That form writes a small `bash` and `socat` adapter which publishes the app on a
Unix socket when the container starts.

The internal manager is intentionally not used on a working launchd/systemd host. The same registry remains compatible across all three managers.
