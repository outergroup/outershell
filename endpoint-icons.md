# Endpoint icons

Apps that don't declare an `iconPath` get their home-screen icon from OuterSSH's
per-user endpoint icon cache. Outer Loop fills the cache on the server: after it
loads a page on an endpoint that has no cached icon, it stores the page's
favicon (or an Outerframe app's presentation icon) through OuterSSH's socket
helper. Outer Shell runs on that server as the same user, so it reads the cache
directly. The cache format is defined by OuterSSH in
`~/dev/src/outerloop/docs/endpoint-icons.md`.

Declared icons always take precedence: an explicit `iconPath`, a container
app's declared icon, or a bundled app icon. Outer Shell never writes the cache.

## Location and lookup

| Platform | Directory |
| --- | --- |
| macOS | `~/Library/Application Support/org.outerssh/endpoint-icons` |
| Linux | `${XDG_CONFIG_HOME:-~/.config}/outerssh/endpoint-icons` |
| Windows | `%LOCALAPPDATA%\OuterSSH\endpoint-icons` |

Each icon is named by the lowercase hex SHA-256 of the endpoint's canonical
`endpoints.d` line, followed by `.png`:

- `unix PATH` for a Unix socket,
- `pipe PATH` for a named pipe, with the path in ASCII lowercase,
- `tcp HOST PORT` for TCP, with the host in ASCII lowercase and `localhost`,
  `127.0.0.1` and `::1` all written as `localhost`.

The endpoint is the one browsers navigate to, which is also the one Outer Shell
publishes in `endpoints.d`:

| App | Endpoint |
| --- | --- |
| Host app (macOS, Linux) | its external socket path if set, otherwise its socket path, otherwise its TCP port |
| Host app (Windows) | its named pipe, otherwise its TCP port |
| macOS or Linux container app | its published host port, otherwise its host relay socket |
| WSL app | its host-side named pipe |

Outer Shell ignores an entry unless it is a regular file of at most 64 KiB that
starts with the PNG signature. On macOS and Linux the cache directory and file
must belong to the user and must not be group- or world-writable, and symlinks
are rejected. On Windows, reparse points are rejected. Large icons are scaled
down by the launcher as usual.

## Where it's implemented

- `outershelld/EndpointIcons.h`: macOS and Linux host apps, used when
  `outershelld` builds the `/api/backends` frontend payloads.
- `Agent/SafeSpaceManager.swift`: macOS container apps.
- `Resources/outershell-container-provider`: Linux container apps.
- `Windows/Icons.c`: Windows host apps (`OuterShell.c`) and WSL apps (`Wsl.c`).

The web and native launchers navigate straight to apps. Earlier versions wrapped
iconless apps in an `outerloop://observe-page-icon` URL and accepted icons at
`POST /api/icon-observation`; that protocol is gone, along with its tokens.
Icons saved that way on macOS and Linux remain in the app records. Windows no
longer reads its old `discovered-icons` directory; those apps get their icons back
the next time Outer Loop loads them.

## Tests

```sh
cc -std=gnu11 -Wall -Wextra -Werror -o /tmp/test_endpoint_icons Scripts/test_endpoint_icons.c && /tmp/test_endpoint_icons
python3 Scripts/test_container_endpoint_icons.py
node Scripts/test_web_container_navigation.cjs
```

On Windows, build with `-UpdateTests` and run
`OuterShellIconTests.exe ..\app-icon.png`; `test_host.py` covers the cached icon
reaching the Outer Shell frontend over the named pipe.
