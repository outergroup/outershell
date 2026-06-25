# outerctl And Socket Messages

`outerctl` is the command-line client for the `outershelld` command socket API.
Each `outerctl <resource> <action>` command maps to one dedicated structured
binary request message. Tools can call the same socket messages directly when a
command-line process would be unnecessary overhead.

The socket API is intended for fast local control. A client sends a
length-prefixed frame to the `outershelld` API socket, and `outershelld` returns
a structured binary response. `outerctl` is only a convenient CLI wrapper
around those same messages.

## Socket Basics

The socket path is selected in this order:

```text
OUTERSHELLD_API_SOCKET
DARWIN_USER_TEMP_DIR/outershelld-api, TMPDIR/outershelld-api, or /tmp/outershelld-api-<uid> for a user on macOS
/var/run/outershelld-api for root on macOS
XDG_RUNTIME_DIR/outershelld-api or /run/user/<uid>/outershelld-api on Linux
/run/outershelld-api for root on Linux
```

Every frame starts with a little-endian length prefix:

```text
bytes 0..3:    UInt32 message length, L
bytes 4..<4+L: message bytes
```

Every message begins with:

```text
bytes 0..1: UInt16 messageType
```

Request message types are allocated contiguously from `10` through `32`.
Dedicated responses are allocated contiguously from `100` through `108`.

Strings are encoded as offset-based references into the same message:

```text
StringRef32:
bytes 0..3: UInt32 offset to UTF-8 bytes
bytes 4..7: UInt32 UTF-8 byte length
```

Offsets are relative to byte 0 of the message, not the frame. Empty optional
strings are encoded as a zero-length `StringRef32`.

Lists of strings use a compact offset to a list payload in the variable region:

```text
StringListRef32:
bytes 0..3: UInt32 offset to first StringRef32 entry, or 0 when item count is 0
bytes 4..7: UInt32 item count

List payload:
bytes 0..: item count consecutive StringRef32 entries
```

The nested `StringRef32` entries use the same offset rule as every other
reference: offsets are relative to byte 0 of the full message.

Command request messages do not include a registry path. The `outershelld`
instance that receives the request chooses its configured registry. Connect to
the user daemon to mutate the user registry, or connect to the root daemon to
mutate the system registry.

Mutating command requests return:

```text
commandResponse, messageType = 100
bytes 2..5:    UInt32 process-style exit status
bytes 6..13:   StringRef32 stdout bytes
bytes 14..21:  StringRef32 stderr bytes
```

Mutating commands usually return an empty `stdout` on success and diagnostics
in `stderr` on failure.

List requests return dedicated binary list response messages. `outerctl`
converts those responses into TSV for command-line output; socket clients
should consume the binary rows directly.

All list response messages start with:

```text
bytes 2..5:    UInt32 status, 0 for success
bytes 6..13:   StringRef32 error message
bytes 14..17:  UInt32 row count
bytes 18..21:  UInt32 row size in bytes
bytes 22..:    row count consecutive rows of row size bytes, followed by referenced string data
```

## Backend Commands

Backends are service records. They hold the stable service id, display name, and
the service-manager registration used by Outer Shell.

### `outerctl backend upsert`

```bash
outerctl backend upsert \
  --backend <service-id> \
  --name <display-name> \
  [--systemd-unit <unit>] \
  [--launchd-plist <plist-path>] \
  [--outershell-owns true]
```

Creates or updates a backend service record. This is the root record that app,
log, service-manager, and opener rows refer to by service id.

Socket message: `backendUpsertRequest` (`messageType = 10`)

```text
bytes 2..3:    UInt16 flags
bytes 4..11:   StringRef32 backend service id
bytes 12..19:  StringRef32 display name
bytes 20..27:  StringRef32 platform service-manager entry
```

Response: `commandResponse` (`messageType = 100`).

Flags:

```text
0x01 owns platform service-manager entry
```

The platform service-manager entry is a systemd unit name on Linux and a
launchd plist path on macOS. `outerctl` accepts `--systemd-unit` and
`--launchd-plist` as command-line spellings, but they encode into the same
field. Use only the spelling that matches the platform.

### `outerctl backend remove`

```bash
outerctl backend remove --backend <service-id>
```

Removes one backend service record.

Socket message: `backendRemoveRequest` (`messageType = 11`)

```text
bytes 2..9: StringRef32 backend service id
```

Response: `commandResponse` (`messageType = 100`).

The backend must not still have app, log, service-manager, or opener records.
Clear dependent rows first.

### `outerctl backend list`

```bash
outerctl backend list [--backend <service-id>]
```

Lists registered backend records. If `--backend` is present, the response
contains only the record whose service id exactly matches that value; if it is
empty, all backend records are returned.

Socket message: `backendListRequest` (`messageType = 12`)

```text
bytes 2..9: StringRef32 backend service id, empty for all backends
```

Response message: `backendListResponse` (`messageType = 101`)

This response uses the common list response header above. Its `row count`
field tells you how many backend rows follow at byte 22. Use the header's
`row size` field as the stride between rows; the current row size is 36 bytes.

```text
Backend row, 36 bytes:
bytes 0..3:    UInt32 flags
bytes 4..11:   StringRef32 service id
bytes 12..19:  StringRef32 display name
bytes 20..27:  StringRef32 systemd unit name
bytes 28..35:  StringRef32 launchd plist path
```

Flags:

```text
0x01 owns platform service-manager entry
```

`outerctl` prints these rows as TSV columns `service_id`, `display_name`,
`unit_name`, `unit_path`, and `owns_unit`.

## App Commands

App rows are user-facing entry points for a backend. Each app has either a TCP
port or a Unix socket endpoint. App icons live on app rows, not backend rows.

### `outerctl app add`

```bash
outerctl app add \
  --backend <service-id> \
  --name <display-name> \
  --url <url-or-path> \
  [--frontend-id <id>] \
  [--icon-path <path>] \
  [--list <suggested-list>] \
  [--port <port> | --socket-path <socket-path>]
```

Creates or updates a user-facing app entry point for a backend. The entry can
target a TCP port, a Unix socket, or an already-formed URL.

Socket message: `appAddRequest` (`messageType = 13`)

```text
bytes 2..5:    UInt32 port
bytes 6..13:   StringRef32 backend service id
bytes 14..21:  StringRef32 display name
bytes 22..29:  StringRef32 URL
bytes 30..37:  StringRef32 frontend id
bytes 38..45:  StringRef32 icon path
bytes 46..53:  StringRef32 frontend list
bytes 54..61:  StringRef32 socket path
```

Response: `commandResponse` (`messageType = 100`).

Set either `port` or `socket path`, not both. If `frontend id` is empty,
`outershelld` derives the main app id from the backend id.

### `outerctl app remove`

```bash
outerctl app remove \
  --backend <service-id> \
  [--frontend-id <id>] \
  [--port <port> | --socket-path <socket-path>]
```

Removes one app entry point for a backend. The command identifies the entry by
frontend id when provided, otherwise by socket path or port.

Socket message: `appRemoveRequest` (`messageType = 14`)

```text
bytes 2..5:    UInt32 port
bytes 6..13:   StringRef32 backend service id
bytes 14..21:  StringRef32 frontend id
bytes 22..29:  StringRef32 socket path
```

Response: `commandResponse` (`messageType = 100`).

### `outerctl app clear`

```bash
outerctl app clear --backend <service-id>
```

Removes all app entry points for the backend.

Socket message: `appClearRequest` (`messageType = 15`)

```text
bytes 2..9: StringRef32 backend service id
```

Response: `commandResponse` (`messageType = 100`).

### `outerctl app list`

```bash
outerctl app list [--backend <service-id>]
```

Lists app entry points. If `--backend` is present, the response contains only
apps whose backend service id exactly matches that value.

Socket message: `appListRequest` (`messageType = 16`)

```text
bytes 2..9: StringRef32 backend service id, empty for all app rows
```

Response message: `appListResponse` (`messageType = 102`)

This response uses the common list response header above. Its `row count`
field tells you how many app rows follow at byte 22. Use the header's
`row size` field as the stride between rows; the current row size is 60 bytes.

```text
App row, 60 bytes:
bytes 0..3:    UInt32 port
bytes 4..11:   StringRef32 frontend id
bytes 12..19:  StringRef32 URL
bytes 20..27:  StringRef32 backend service id
bytes 28..35:  StringRef32 display name
bytes 36..43:  StringRef32 socket path
bytes 44..51:  StringRef32 icon path
bytes 52..59:  StringRef32 frontend list
```

`outerctl` prints these rows as TSV columns `frontend_id`, `url`,
`service_id`, `display_name`, `port`, `socket_path`, `icon_path`, and `list`.

## Log Commands

Log rows tell Outer Shell which files belong to a backend.

### `outerctl log add`

```bash
outerctl log add --backend <service-id> --path <log-path>
```

Registers a log file path as belonging to a backend.

Socket message: `logAddRequest` (`messageType = 17`)

```text
bytes 2..9:    StringRef32 backend service id
bytes 10..17:  StringRef32 log path
```

Response: `commandResponse` (`messageType = 100`).

### `outerctl log remove`

```bash
outerctl log remove --backend <service-id> --path <log-path>
```

Removes one registered log path for a backend.

Socket message: `logRemoveRequest` (`messageType = 18`)

```text
bytes 2..9:    StringRef32 backend service id
bytes 10..17:  StringRef32 log path
```

Response: `commandResponse` (`messageType = 100`).

### `outerctl log clear`

```bash
outerctl log clear --backend <service-id>
```

Removes all registered log paths for the backend.

Socket message: `logClearRequest` (`messageType = 19`)

```text
bytes 2..9: StringRef32 backend service id
```

Response: `commandResponse` (`messageType = 100`).

### `outerctl log list`

```bash
outerctl log list [--backend <service-id>]
```

Lists registered log paths. If `--backend` is present, the response contains
only log rows whose backend service id exactly matches that value.

Socket message: `logListRequest` (`messageType = 20`)

```text
bytes 2..9: StringRef32 backend service id, empty for all log rows
```

Response message: `logListResponse` (`messageType = 103`)

This response uses the common list response header above. Its `row count`
field tells you how many log rows follow at byte 22. Use the header's
`row size` field as the stride between rows; the current row size is 16 bytes.

```text
Log row, 16 bytes:
bytes 0..7:   StringRef32 log path
bytes 8..15:  StringRef32 backend service id
```

`outerctl` prints these rows as TSV columns `path` and `service_id`.

## Service Manager Commands

Service-manager metadata is now stored on the backend row. Prefer
`backend upsert --systemd-unit` or `backend upsert --launchd-plist` for writes.

### `outerctl systemd clear`

```bash
outerctl systemd clear --backend <service-id>
```

Clears the Linux systemd service-manager metadata stored on the backend row.

Socket message: `serviceManagerClearRequest` (`messageType = 21`)

```text
bytes 2..9: StringRef32 backend service id
```

Response: `commandResponse` (`messageType = 100`).

### `outerctl systemd list`

```bash
outerctl systemd list [--backend <service-id>]
```

Lists backend systemd registrations. If `--backend` is present, the response
contains only the systemd row for that backend service id.

Socket message: `serviceManagerListRequest` (`messageType = 22`)

```text
bytes 2..9: StringRef32 backend service id, empty for all service-manager rows
```

Response message: `serviceManagerListResponse` (`messageType = 104`)

This response uses the common list response header above. Its `row count`
field tells you how many service-manager rows follow at byte 22. Use the
header's `row size` field as the stride between rows; the current row size is
20 bytes.

```text
Service-manager row, 20 bytes:
bytes 0..3:    UInt32 flags
bytes 4..11:   StringRef32 backend service id
bytes 12..19:  StringRef32 platform service-manager entry
```

Flags:

```text
0x01 owns platform service-manager entry
```

On Linux, `outerctl systemd list` prints `service_id` and `unit_name`. On
macOS, `outerctl launchd list` prints `service_id`, `plist_path`, and
`owns_plist`.

### `outerctl launchd clear`

```bash
outerctl launchd clear --backend <service-id>
```

Clears the macOS launchd service-manager metadata stored on the backend row.

Socket message: `serviceManagerClearRequest` (`messageType = 21`)

```text
bytes 2..9: StringRef32 backend service id
```

Response: `commandResponse` (`messageType = 100`).

### `outerctl launchd list`

```bash
outerctl launchd list [--backend <service-id>]
```

Lists backend launchd registrations. If `--backend` is present, the response
contains only the launchd row for that backend service id.

Socket message: `serviceManagerListRequest` (`messageType = 22`)

```text
bytes 2..9: StringRef32 backend service id, empty for all service-manager rows
```

The response is the same `serviceManagerListResponse` (`messageType = 104`)
described above.

## Content Type Commands

Content types are normalized, lowercase dotted identifiers such as
`public.text` or `org.example.source`. A type can conform to one or more other
types, and it can match extensions, exact filenames, or MIME type hints.

### `outerctl content-type add`

```bash
outerctl content-type add \
  --backend <service-id> \
  --content-type <identifier> \
  --name <display-name> \
  [--conforms-to <comma-separated-types>] \
  [--extensions <comma-separated-extensions>] \
  [--filenames <comma-separated-filenames>] \
  [--mime-types <comma-separated-mime-types>]
```

Creates or updates one backend-owned custom content type definition. These
definitions augment the built-in content types used for file matching.
`backend remove` clears the custom definitions owned by that backend.

Socket message: `contentTypeAddRequest` (`messageType = 23`)

```text
bytes 2..9:    StringRef32 backend service id
bytes 10..17:  StringRef32 content type
bytes 18..25:  StringRef32 display name
bytes 26..33:  StringListRef32 conforms-to content types
bytes 34..41:  StringListRef32 filename extensions
bytes 42..49:  StringListRef32 exact filenames
bytes 50..57:  StringListRef32 MIME types
```

Response: `commandResponse` (`messageType = 100`).

### `outerctl content-type remove`

```bash
outerctl content-type remove --backend <service-id> --content-type <identifier>
```

Removes one backend-owned custom content type definition. Built-in content
types are not stored in the registry and cannot be removed.

Socket message: `contentTypeRemoveRequest` (`messageType = 24`)

```text
bytes 2..9:    StringRef32 backend service id
bytes 10..17:  StringRef32 content type
```

Response: `commandResponse` (`messageType = 100`).

### `outerctl content-type clear`

```bash
outerctl content-type clear --backend <service-id>
```

Removes all custom content type definitions owned by one backend. `backend
remove` does this automatically before removing the backend row.

Socket message: `contentTypeClearRequest` (`messageType = 25`)

```text
bytes 2..9: StringRef32 backend service id
```

Response: `commandResponse` (`messageType = 100`).

Built-in content types are not stored in the registry and are not removed by
this command.

### `outerctl content-type list`

```bash
outerctl content-type list [--backend <service-id>] [--content-type <identifier>]
```

Lists built-in and custom content type definitions. If `--content-type` is
present, the response contains only the definition whose identifier exactly
matches the normalized value. If `--backend` is present, the response contains
only custom definitions owned by that backend; built-ins have no backend owner
and are omitted.

Socket message: `contentTypeListRequest` (`messageType = 26`)

```text
bytes 2..9:    StringRef32 backend service id, empty for all backends and built-ins
bytes 10..17:  StringRef32 content type identifier, empty for all content types
```

Response message: `contentTypeListResponse` (`messageType = 105`)

This response uses the common list response header above. Its `row count`
field tells you how many content-type rows follow at byte 22. Use the header's
`row size` field as the stride between rows; the current row size is 56 bytes.

```text
Content-type row, 56 bytes:
bytes 0..7:    StringRef32 backend service id, empty for built-ins
bytes 8..15:   StringRef32 identifier
bytes 16..23:  StringRef32 display name
bytes 24..31:  StringListRef32 conforms-to content types
bytes 32..39:  StringListRef32 filename extensions
bytes 40..47:  StringListRef32 exact filenames
bytes 48..55:  StringListRef32 MIME types
```

`outerctl` prints these rows as TSV columns `service_id`, `identifier`,
`display_name`, `conforms_to`, `extensions`, `filenames`, and `mime_types`,
joining each list column with commas for display.

## Opener Commands

Openers connect a backend to a content type. When a file matches the content
type or any child type, Outer Shell can present the backend as an app that can
open the file.

### `outerctl opener add`

```bash
outerctl opener add \
  --backend <service-id> \
  --content-type <identifier> \
  --socket-path <socket-path> \
  --name <display-name> \
  [--url-template <template>] \
  [--rank <non-negative-integer>] \
  [--capabilities view,edit]
```

Creates or updates an opener record for a backend and content type. The opener
record tells Outer Shell that the backend can view, edit, or otherwise open
files that match that content type.

Socket message: `openerAddRequest` (`messageType = 27`)

```text
bytes 2..5:    UInt32 rank
bytes 6..9:    UInt32 capability flags
bytes 10..17:  StringRef32 backend service id
bytes 18..25:  StringRef32 content type
bytes 26..33:  StringRef32 display name
bytes 34..41:  StringRef32 socket path
bytes 42..49:  StringRef32 URL template
```

Response: `commandResponse` (`messageType = 100`).

Capability flags:

```text
0x01 view
0x02 edit
```

If `--capabilities` is omitted, `outerctl` sends both flags.

The URL template can include `{file}`. `outershelld` percent-encodes the file
path and substitutes it into the template when resolving opener URLs.

### `outerctl opener remove`

```bash
outerctl opener remove \
  --backend <service-id> \
  --content-type <identifier>
```

Removes one opener record for the backend and registered content type.

Socket message: `openerRemoveRequest` (`messageType = 28`)

```text
bytes 2..9:    StringRef32 backend service id
bytes 10..17:  StringRef32 content type
```

Response: `commandResponse` (`messageType = 100`).

### `outerctl opener clear`

```bash
outerctl opener clear --backend <service-id>
```

Removes all opener records for the backend.

Socket message: `openerClearRequest` (`messageType = 29`)

```text
bytes 2..9: StringRef32 backend service id
```

Response: `commandResponse` (`messageType = 100`).

### `outerctl opener list`

```bash
outerctl opener list \
  [--backend <service-id>] \
  [--content-type <identifier>]
```

Lists registered opener records. If `--backend` is present, only opener rows
for that backend service id are returned. If `--content-type` is present, only
opener rows whose registered content type exactly matches the normalized value
are returned. This is a registry listing filter; it does not expand content
type conformance and does not infer a file's type.

Use `fileOpenersQuery` when you want “which apps can open this file?” behavior
with content-type inference, optional explicit content type, conformance
expansion, and resolved URLs.

Socket message: `openerListRequest` (`messageType = 30`)

```text
bytes 2..9:    StringRef32 backend service id, empty for all openers
bytes 10..17:  StringRef32 registered content type, empty for all openers
```

Response message: `openerListResponse` (`messageType = 106`)

This response uses the common list response header above. Its `row count`
field tells you how many opener rows follow at byte 22. Use the header's
`row size` field as the stride between rows; the current row size is 48 bytes.

```text
Opener row, 48 bytes:
bytes 0..3:    UInt32 rank
bytes 4..11:   StringRef32 content type
bytes 12..19:  StringRef32 backend service id
bytes 20..27:  StringRef32 display name
bytes 28..35:  StringRef32 socket path
bytes 36..43:  StringRef32 URL template
bytes 44..47:  UInt32 capability flags
```

`outerctl` prints these rows as TSV columns `content_type`, `service_id`,
`display_name`, `socket_path`, `url_template`, `rank`, and `capabilities`.

## Querying File Openers Directly

File opener lookup also has a specialized read-only query. Use it when the
caller needs opener data for a file. This message does not take a backend
service id; it asks the daemon to infer or use a content type, expand conformance
types, and return all matching openers.

```text
fileOpenersQuery, messageType = 31
bytes 2..9:    StringRef32 file path
bytes 10..17:  StringRef32 content type, optional
bytes 18..25:  StringRef32 requester user, optional
```

The response is:

```text
fileOpenersResponse, messageType = 107
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

## When To Use `outerctl` Versus Messages

Use `outerctl` for shell scripts, packaging hooks, and debugging. It gives you
human-readable commands and TSV output.

Use the structured socket messages when a program is already running and wants
to avoid spawning a process, reparsing text, or serializing through shell
quoting rules. The command messages are stable binary records with fixed fields
and offset-based variable data, so they can be generated directly from native
data structures.
