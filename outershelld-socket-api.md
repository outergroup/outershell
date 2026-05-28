# outershelld Socket API

The outershelld API uses length-prefixed Unix socket frames with little-endian
scalars and offset-based references.

`outershelld` is the registry/API broker. The Outer Shell UI HTTP backend runs
as `OuterShellBackend` and talks to the broker socket instead of owning that
socket itself.

```text
Socket frame:
bytes 0..3:    UInt32 little-endian message length, L
bytes 4..<4+L: message bytes

StringRef32:
bytes 0..3: UInt32 little-endian offset to UTF-8 bytes
bytes 4..7: UInt32 little-endian UTF-8 byte length
```

Offsets are relative to the beginning of the message, where byte 0 is the first
byte of `messageType`.

## `outerctlInvoke` request (`messageType = 1`)

This transitional request moves existing `outerctl` registry operations behind
outershelld. Future APIs should add typed messages instead of extending this
argv wrapper.

```text
bytes 2..5:           UInt32 argc
bytes 6..13:          StringRef32 registry database path
bytes 14..<14+8*argc: argv StringRef32 entries
```

## `outerctlInvokeResponse` (`messageType = 2`)

```text
bytes 2..5:    UInt32 process-style exit status
bytes 6..13:   StringRef32 stdout bytes
bytes 14..21:  StringRef32 stderr bytes
```

## `fileOpenersQuery` (`messageType = 3`)

Returns applications that can open a file extension.

```text
bytes 2..9:    StringRef32 extension, with or without leading "."
bytes 10..17:  StringRef32 file path, used to expand the opener URL template
```

## `fileOpenersResponse` (`messageType = 4`)

```text
bytes 2..5:    UInt32 status, 0 for success
bytes 6..13:   StringRef32 error message
bytes 14..17:  UInt32 opener row count
bytes 18..:    opener rows, 40 bytes each

Opener row:
bytes 0..7:    StringRef32 extension
bytes 8..15:   StringRef32 service id
bytes 16..23:  StringRef32 display name
bytes 24..31:  StringRef32 socket path
bytes 32..39:  StringRef32 resolved URL
```

## File Opener Registry

The daemon-owned API includes `opener` commands for apps that can open files by
extension, plus the typed `fileOpenersQuery` message for direct socket clients:

```bash
outerctl opener add --backend dev.outergroup.Plaintext \
  --extension txt \
  --socket-path "$XDG_RUNTIME_DIR/dev.outergroup.Plaintext" \
  --name Plaintext \
  --url-template '?file={file}'

outerctl opener list --extension .txt --file /path/to/file.txt
```

Extensions are stored lowercase without the leading dot. `url_template` uses one
placeholder, `{file}`, which outershelld replaces with a percent-encoded file
path while resolving the final app URL.

Lookup is user-contextual. A backend that receives an HTTP request over a Unix
socket should inspect the peer UID for that request and query that user's
`outershelld` socket, normally `/run/user/<uid>/outershelld-api`. The user
broker returns user registry openers plus system registry openers whose socket
path is accessible to that user. TCP HTTP listeners cannot provide peer
credentials, so they should be treated as a local/development fallback and use
the backend process UID.
