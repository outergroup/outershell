# File Association / Opener API

This document describes the current Outer Shell file association API: how
content types are defined, how backends register themselves as file openers,
how clients query openers, and how the registry stores those records.

## Concepts

`outershelld` owns content type inference. Apps can define content types using
Apple-style identifiers such as `public.text` or `public.markdown`, but the API
calls them content types rather than UTIs.

A content type record contains:

- `service_id`: backend that owns the custom declaration; built-ins have no owner
- `identifier`: normalized type identifier, for example `public.text`
- `display_name`: label shown to people
- `conforms_to`: content type identifiers this type conforms to
- `extensions`: filename extensions without leading dots
- `filenames`: exact filenames, for example `Makefile`
- `mime_types`: MIME type hints

An opener says that a backend can open files whose inferred content type is the
opener content type or conforms to it. For example, an opener registered for
`public.text` also matches `public.markdown`.

An opener record contains:

- `content_type`: normalized content type identifier
- `service_id`: backend service identifier
- `display_name`: label shown to users
- `socket_path`: Unix socket endpoint for the backend/app
- `url_template`: app URL template, defaulting to `?file={file}`
- `rank`: non-negative integer used for ordering within a content type
- `capabilities`: `view`, `edit`, or both

`url_template` supports the `{file}` placeholder. When resolving an opener,
`outershelld` percent-encodes the file path and substitutes it into the
template. If a `socket_path` exists, the resolved URL is prefixed with that
socket path and a separating `/` when needed.

Capability flags are stored as a UInt32 bitmask:

```text
0x01 view
0x02 edit
```

If capabilities are omitted, the opener is treated as both `view` and `edit`.

## Built-In Content Types

`outershelld` includes a small built-in type graph so common files can be
opened without every app installing definitions:

- `public.data`
- `public.text`
- `public.plain-text`
- `public.markdown`
- `public.json`
- `public.xml`
- `public.shell-script`
- `public.python-script`
- `public.javascript`
- `public.html`
- `public.css`
- `public.image`
- `public.png`
- `public.jpeg`
- `public.gif`
- `public.pdf`
- `public.zip-archive`

The built-ins can be listed with:

```bash
outerctl content-type list
```

## Defining Content Types

Custom content types are registered through `outerctl`:

```bash
outerctl content-type add \
  --backend org.outershell.Plaintext \
  --content-type org.outershell.example-source \
  --name 'Example Source' \
  --conforms-to public.text \
  --extensions example,exs \
  --filenames Examplefile \
  --mime-types text/x-example
```

Remove a custom content type:

```bash
outerctl content-type remove --backend org.outershell.Plaintext --content-type org.outershell.example-source
```

Remove all custom content type definitions owned by a backend:

```bash
outerctl content-type clear --backend org.outershell.Plaintext
```

Built-in content types are not stored in the registry and cannot be removed.
Removing a backend clears custom content type definitions owned by that backend.

## Registering Openers

Mutating opener operations use `outerctl` commands. The `outerctl` binary parses
the command line and sends dedicated structured messages such as
`openerAddRequest`, `openerRemoveRequest`, and `openerClearRequest` to
`outershelld`.

Add or update an opener:

```bash
outerctl opener add \
  --backend org.outershell.Plaintext \
  --content-type public.text \
  --socket-path "$XDG_RUNTIME_DIR/org.outershell.Plaintext" \
  --name Plaintext \
  --url-template '?file={file}' \
  --rank 0 \
  --capabilities view,edit
```

Remove one opener for a backend and content type:

```bash
outerctl opener remove \
  --backend org.outershell.Plaintext \
  --content-type public.text
```

Remove all opener records for a backend:

```bash
outerctl opener clear --backend org.outershell.Plaintext
```

The backend must already be registered before `opener add` or `opener remove`.
`opener clear` can run for orphaned backend identifiers so uninstall paths can
clean stale rows.

## Listing Openers

`outerctl opener list` reads the user registry and prints TSV:

```bash
outerctl opener list
outerctl opener list --backend org.outershell.Plaintext
outerctl opener list --content-type public.text
```

Columns:

```text
content_type  service_id  display_name  socket_path  url_template  rank  capabilities
```

This is a registry listing command. `--content-type` matches the opener's
registered content type exactly after normalization. It does not infer a file's
type and does not expand conformed-to content types.

## Typed Socket Query

Read-only file-to-opener lookup uses the typed `fileOpenersQuery` message on
the `outershelld` API socket. Use this message when the caller has a file path
and needs all apps that can open it.

Default API socket:

- macOS: `$OUTERSHELLD_API_SOCKET`, otherwise `$DARWIN_USER_TEMP_DIR/outershelld-api`, `$TMPDIR/outershelld-api`, or `/tmp/outershelld-api-<uid>`
- Linux: `$OUTERSHELLD_API_SOCKET`, otherwise `$XDG_RUNTIME_DIR/outershelld-api` or `/run/user/<uid>/outershelld-api`

Request message type: `31` (`OUTERSHELLD_API_FILE_OPENERS_QUERY`)

```text
bytes 0..1:    UInt16 message type = 31
bytes 2..9:    StringRef32 file path
bytes 10..17:  StringRef32 content type, optional
bytes 18..25:  StringRef32 requester user, optional
```

If `content type` is empty, `outershelld` infers content types from the file
path. If a content type is provided, `outershelld` expands it through its
`conforms_to` graph. If `requester user` is present, `outershelld` queries
that user's registry before adding accessible system-registry openers.

Response message type: `107` (`OUTERSHELLD_API_FILE_OPENERS_RESPONSE`)

```text
bytes 0..1:    UInt16 message type = 107
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

Lookup behavior:

- A path query infers matching content types from exact filename, extension,
  file magic, shebang, and text sniffing.
- The inferred or explicit content types are expanded through `conforms_to`
  conformance.
- User-registry rows do not require a socket accessibility check.
- System-registry rows are included only if the current user can read and write
  the registered Unix socket path and the path is a socket.

## Files App Adapter

The Files app exposes a local HTTP endpoint that adapts file paths to the typed
socket query:

```text
GET  /api/openers?path=<path>
POST /api/openers
```

For `POST`, the body is a binary path request with the Files opener request
magic. Files resolves the path, sends the path to `outershelld`, and returns an
application/octet-stream response for the Files frontend. Files does not infer
extensions or text-ness locally.

Files response header:

```text
bytes 0..3:    UInt32 magic
bytes 4..7:    UInt32 version
bytes 8..11:   UInt32 row count
bytes 12..15:  UInt32 row size, currently 52
bytes 16..19:  UInt32 rows offset
bytes 20..23:  UInt32 variable data offset
bytes 24..27:  UInt32 total size
bytes 28..31:  UInt32 reserved
```

Each Files opener row is a seven-field, 52-byte row. The first six fields match
the daemon socket response: content type, service id, display name, socket path,
resolved URL, and capability flags. The final field is the owner username
resolved from the opener socket path. The Files frontend uses this owner name
to disambiguate openers when the same app is installed in both user and root
scopes.

For root-owned files, Files should apply root-instance routing only to openers
with the `edit` capability. A viewer-only opener can remain connected to the
user instance and should not be labeled with `(root)`. An editor-capable opener
that resolves to a root/system instance can be labeled as `Edit in <app>
(root)`.

## Normalization Rules

Content type normalization:

- value is lowercased
- value must contain at least one `.`
- only ASCII alphanumeric characters, `.`, `-`, and `_` are accepted

Extension lists in content type definitions use comma-separated extensions
without leading dots. Matching is case-insensitive.

Filename lists in content type definitions use comma-separated exact filenames.
Matching is case-sensitive.

On the socket API, these list-like content type fields are encoded as
`StringListRef32` payloads, not comma-separated strings. The `outerctl`
command-line interface accepts and prints comma-separated text as a human
convenience.

## Storage

The current registry is stored as an `ORWA` binary file.

Default user registry path:

- `$OUTERSHELL_REGISTRY`, if set
- `$BACKENDS_REGISTRY_DB`, if set
- otherwise `$OUTERSHELL_HOME/registry.orwa`
- on macOS, default `$OUTERSHELL_HOME` is `~/Library/Application Support/outershell`
- on Linux, default `$OUTERSHELL_HOME` is `$XDG_STATE_HOME/outershell` or `~/.local/state/outershell`

Default system registry path:

- `$OUTERSHELL_SYSTEM_REGISTRY`, if set
- `$BACKENDS_SYSTEM_REGISTRY_DB`, if set
- otherwise `/Library/Application Support/outershell/registry.orwa` on macOS
- otherwise `/var/lib/outershell/registry.orwa` on Linux

The binary `ORWA` file has six table descriptors:

```text
0: backends
1: frontends
2: frontend layouts
3: log files
4: content types
5: file openers
```

Content type rows are 112 bytes:

```text
bytes 0..15:    StringRef64 service_id
bytes 16..31:   StringRef64 identifier
bytes 32..47:   StringRef64 display name
bytes 48..63:   StringListRef64 conforms_to
bytes 64..79:   StringListRef64 extensions
bytes 80..95:   StringListRef64 filenames
bytes 96..111:  StringListRef64 mime_types
```

`StringListRef64` uses the same shape as `StringRef64`, but the second field is
an item count instead of a byte length. The referenced payload is that many
consecutive `StringRef64` entries.

File opener rows are 88 bytes:

```text
bytes 0..15:   StringRef64 content type
bytes 16..31:  StringRef64 service id
bytes 32..47:  StringRef64 display name
bytes 48..63:  StringRef64 socket path
bytes 64..79:  StringRef64 url template
bytes 80..83:  UInt32 rank
bytes 84..87:  UInt32 capability flags
```

The binary writer keeps strings in a shared variable region and stores string
references in rows. A write opens the registry with an exclusive `.lock` file,
updates the in-memory `RegistryStore`, then rewrites the `ORWA` file on commit.

## Current Gaps

- The public typed opener query returns resolved URLs and capabilities but not
  `rank` or `url_template`.
- Ordering is whatever the current registry file order produces.
