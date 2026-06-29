# outershelld Socket API

The outershelld API uses length-prefixed Unix socket frames with little-endian
scalars and offset-based references.

`outershelld` is the registry/API broker. The Outer Shell UI HTTP backend runs
as `OuterShellBackend` and talks to the broker socket instead of owning that
socket itself.

For a command-oriented view of the same request messages, see
[`outerctl-socket-messages.md`](outerctl-socket-messages.md).

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

String lists use a 4-byte offset to a variable-region list payload:

```text
StringListRef32:
bytes 0..3: UInt32 little-endian offset to first StringRef32 entry, or 0 when item count is 0
bytes 4..7: UInt32 little-endian item count

List payload:
bytes 0..: item count consecutive StringRef32 entries
```

Nested `StringRef32` entries still use offsets relative to byte 0 of the full
message.

Every request begins with:

```text
bytes 0..1: UInt16 messageType
```

## Request Messages

`outerctl` is a command-line client for these messages. It parses command-line
arguments locally and sends one dedicated binary message for each command
action.

Registry command requests do not carry a registry path. The `outershelld`
instance that receives the request chooses its configured registry, such as the
user registry for a user daemon or the system registry for a root daemon.

Request message types are allocated in one contiguous block:

```text
10 backendUpsertRequest
11 backendRemoveRequest
12 backendListRequest
13 appUpsertRequest
14 appRemoveRequest
15 appListRequest
16 logAddRequest
17 logRemoveRequest
18 logListRequest
19 contentTypeAddRequest
20 contentTypeRemoveRequest
21 contentTypeListRequest
22 openerUpsertRequest
23 openerRemoveRequest
24 openerListRequest
25 fileOpenersQuery
26 uiRequest
```

Response message types are allocated in one contiguous block:

```text
100 commandResponse
101 backendListResponse
102 appListResponse
103 logListResponse
104 contentTypeListResponse
105 openerListResponse
106 fileOpenersResponse
107 uiResponse
```

Flags:

```text
0x01 owns platform service-manager entry
0x02 include icons
```

### Backend

`backendUpsertRequest` (`messageType = 10`):

```text
bytes 2..3:    UInt16 flags
bytes 4..11:   StringRef32 backend service id
bytes 12..19:  StringRef32 display name
bytes 20..27:  StringRef32 platform service-manager entry
```

Returns `commandResponse` (`messageType = 100`).

The platform service-manager entry is a systemd unit name on Linux and a
launchd plist path on macOS.

`backendRemoveRequest` (`11`):

```text
bytes 2..9: StringRef32 backend service id
```

Returns `commandResponse` (`messageType = 100`).

Removing a backend also removes its app, log, opener, and content-type rows,
plus its platform service-manager metadata.

`backendListRequest` (`12`):

```text
bytes 2..9: StringRef32 backend service id
```

Returns `backendListResponse` (`messageType = 101`).

### Apps

`appUpsertRequest` (`13`):

```text
bytes 2..3:    UInt16 endpoint kind
bytes 4..5:    UInt16 scheme
bytes 6..7:    UInt16 endpoint flags, currently 0
bytes 8..9:    UInt16 TCP port, 0 when not a TCP endpoint
bytes 10..17:  StringRef32 backend service id
bytes 18..25:  StringRef32 frontend id
bytes 26..33:  StringRef32 display name
bytes 34..41:  StringRef32 URL path
bytes 42..49:  StringRef32 TCP host
bytes 50..57:  StringRef32 Unix socket path
bytes 58..65:  StringRef32 icon path
bytes 66..73:  StringRef32 frontend list
```

Returns `commandResponse` (`messageType = 100`).

`appRemoveRequest` (`14`):

```text
bytes 2..5:    UInt32 port
bytes 6..13:   StringRef32 backend service id
bytes 14..21:  StringRef32 frontend id
bytes 22..29:  StringRef32 socket path
```

Returns `commandResponse` (`messageType = 100`).

`appListRequest` (`15`):

```text
bytes 2..9: StringRef32 backend service id
```

Returns `appListResponse` (`messageType = 102`).

### Logs

`logAddRequest` (`16`):

```text
bytes 2..9:    StringRef32 backend service id
bytes 10..17:  StringRef32 log path
```

Returns `commandResponse` (`messageType = 100`).

`logRemoveRequest` (`17`):

```text
bytes 2..9:    StringRef32 backend service id
bytes 10..17:  StringRef32 log path
```

Returns `commandResponse` (`messageType = 100`).

`logListRequest` (`18`):

```text
bytes 2..9: StringRef32 backend service id
```

Returns `logListResponse` (`messageType = 103`).

### Content Types

`contentTypeAddRequest` (`messageType = 19`):

```text
bytes 2..9:    StringRef32 backend service id
bytes 10..17:  StringRef32 content type
bytes 18..25:  StringRef32 display name
bytes 26..33:  StringListRef32 conforms-to content types
bytes 34..41:  StringListRef32 filename extensions
bytes 42..49:  StringListRef32 MIME types
```

Returns `commandResponse` (`messageType = 100`).

`contentTypeRemoveRequest` (`messageType = 20`):

```text
bytes 2..9:    StringRef32 backend service id
bytes 10..17:  StringRef32 content type
```

Returns `commandResponse` (`messageType = 100`).

`contentTypeListRequest` (`messageType = 21`):

```text
bytes 2..9:    StringRef32 backend service id
bytes 10..17:  StringRef32 content type
```

Returns `contentTypeListResponse` (`messageType = 104`).

### Openers

`openerUpsertRequest` (`messageType = 22`):

```text
bytes 2..5:    UInt32 rank
bytes 6..9:    UInt32 capability flags
bytes 10..17:  StringRef32 backend service id, optional shorthand
bytes 18..25:  StringRef32 frontend id, optional if backend is present
bytes 26..33:  StringRef32 content type
bytes 34..41:  StringRef32 URL template
```

Returns `commandResponse` (`messageType = 100`).

Capability flags:

```text
0x01 view
0x02 edit
```

`openerRemoveRequest` (`messageType = 23`):

```text
bytes 2..9:    StringRef32 backend service id
bytes 10..17:  StringRef32 content type
```

Returns `commandResponse` (`messageType = 100`).

`openerListRequest` (`messageType = 24`):

```text
bytes 2..9:    StringRef32 backend service id
bytes 10..17:  StringRef32 content type
```

Returns `openerListResponse` (`messageType = 105`).

## Command Responses

Mutating command requests return `commandResponse` (`messageType = 100`):

```text
bytes 2..5:    UInt32 process-style exit status
bytes 6..13:   StringRef32 stdout bytes
bytes 14..21:  StringRef32 stderr bytes
```

List requests return dedicated binary responses instead of TSV. The common
response header is:

```text
bytes 2..5:    UInt32 status, 0 for success
bytes 6..13:   StringRef32 error message
bytes 14..17:  UInt32 row count
bytes 18..21:  UInt32 row size in bytes
bytes 22..:    row count consecutive rows of row size bytes, followed by referenced string data
```

`backendListResponse` (`101`) uses the common list response header. Its `row
count` field tells you how many backend rows follow at byte 22. Use the
header's `row size` field as the stride between rows; the current row size is
36 bytes:

```text
bytes 0..3:    UInt32 flags
bytes 4..11:   StringRef32 service id
bytes 12..19:  StringRef32 display name
bytes 20..27:  StringRef32 systemd unit name
bytes 28..35:  StringRef32 launchd plist path
```

`appListResponse` (`102`) uses the common list response header. Its `row count`
field tells you how many app rows follow at byte 22. Use the header's `row
size` field as the stride between rows; the current row size is 80 bytes:

```text
bytes 0..1:    UInt16 endpoint kind
bytes 2..3:    UInt16 scheme
bytes 4..5:    UInt16 endpoint flags, currently 0
bytes 6..7:    UInt16 TCP port, 0 when not a TCP endpoint
bytes 8..15:   StringRef32 frontend id
bytes 16..23:  StringRef32 backend service id
bytes 24..31:  StringRef32 display name
bytes 32..39:  StringRef32 TCP host
bytes 40..47:  StringRef32 Unix socket path
bytes 48..55:  StringRef32 URL path
bytes 56..63:  StringRef32 derived URL
bytes 64..71:  StringRef32 icon path
bytes 72..79:  StringRef32 frontend list
```

`logListResponse` (`103`) uses the common list response header. Its `row count`
field tells you how many log rows follow at byte 22. Use the header's `row
size` field as the stride between rows; the current row size is 16 bytes:

```text
bytes 0..7:   StringRef32 log path
bytes 8..15:  StringRef32 backend service id
```

`contentTypeListResponse` (`104`) uses the common list response header. Its
`row count` field tells you how many content-type rows follow at byte 22. Use
the header's `row size` field as the stride between rows; the current row size
is 48 bytes:

```text
bytes 0..7:    StringRef32 backend service id, empty for built-ins
bytes 8..15:   StringRef32 identifier
bytes 16..23:  StringRef32 display name
bytes 24..31:  StringListRef32 conforms-to content types
bytes 32..39:  StringListRef32 filename extensions
bytes 40..47:  StringListRef32 MIME types
```

`openerListResponse` (`105`) uses the common list response header. Its `row
count` field tells you how many opener rows follow at byte 22. Use the
header's `row size` field as the stride between rows; the current row size is
48 bytes:

```text
bytes 0..3:    UInt32 rank
bytes 4..11:   StringRef32 content type
bytes 12..19:  StringRef32 backend service id
bytes 20..27:  StringRef32 display name
bytes 28..35:  StringRef32 socket path
bytes 36..43:  StringRef32 URL template
bytes 44..47:  UInt32 capability flags
```

## `fileOpenersQuery` (`messageType = 25`)

Returns applications that can open a file path or an explicit content type.
This is the file-to-openers query; it does not take a backend service id.

```text
bytes 2..9:    StringRef32 file path
bytes 10..17:  StringRef32 content type, optional
bytes 18..25:  StringRef32 requester user, optional
```

If `content type` is empty, `outershelld` infers content types from exact
filename, extension, file magic, shebang, and text sniffing. If a content type
is provided, `outershelld` expands it through its `conforms_to` graph.
If `requester user` is present, the daemon queries that user's registry before
adding accessible system-registry openers.

## `fileOpenersResponse` (`messageType = 106`)

```text
bytes 2..5:    UInt32 status, 0 for success
bytes 6..13:   StringRef32 error message
bytes 14..17:  UInt32 opener row count
bytes 18..:    opener rows, 44 bytes each

Opener row:
bytes 0..7:    StringRef32 content type
bytes 8..15:   StringRef32 service id
bytes 16..23:  StringRef32 display name
bytes 24..31:  StringRef32 socket path
bytes 32..39:  StringRef32 resolved URL
bytes 40..43:  UInt32 capability flags
```

## File Opener Registry

The daemon-owned API includes `content-type` commands for defining content
types and `opener` commands for apps that can open files by content type:

```bash
outerctl content-type add \
  --backend org.outershell.Plaintext \
  --content-type org.outershell.example-source \
  --name 'Example Source' \
  --conforms-to public.text \
  --extensions example,exs \
  --mime-types text/x-example

outerctl opener upsert --backend org.outershell.Plaintext \
  --content-type public.text \
  --url-template '?file={file}' \
  --capabilities view,edit

outerctl opener list --content-type public.text
outerctl content-type list
```

Content type identifiers are lowercase dotted identifiers such as
`public.text`. `url_template` uses one placeholder, `{file}`, which
outershelld replaces with a percent-encoded file path while resolving the final
app URL.

Lookup is user-contextual. A backend that receives an HTTP request over a Unix
socket should inspect the peer UID for that request and query that user's
`outershelld` socket, normally `DARWIN_USER_TEMP_DIR/outershelld-api` on macOS
or `/run/user/<uid>/outershelld-api` on Linux. Root callers use
`/var/run/outershelld-api` on macOS and `/run/outershelld-api` on Linux. The
user broker returns user registry openers plus system registry openers whose
socket path is accessible to that user. TCP HTTP listeners cannot provide peer
credentials, so they should be treated as a local/development fallback and use
the backend process UID.
