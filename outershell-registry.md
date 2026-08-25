# Outer Shell Registry Binary Format

`registry.orwa` is the source of truth for Outer Shell's service registry and
private durable resources. Catalogs and container recipes are stored here
rather than in parallel JSON files.

The backend `unit_path` field can contain either a launchd plist path or a
portable `.outerservice` path. A `.outerservice` suffix selects `outershelld`'s
internal manager; `unit_name` remains empty. See [outerservice.md](outerservice.md).

All scalar values are little-endian. Strings are UTF-8 without a trailing NUL.
Offsets are absolute offsets from byte 0 of the containing file. A reference
with both offset and length equal to zero represents an absent or empty value.

## Shared References

```text
StringRef64 or DataRef64:
bytes 0..7:   UInt64 absolute offset, O
bytes 8..15:  UInt64 byte length, L

StringRef32:
bytes 0..3:   UInt32 absolute offset, O
bytes 4..7:   UInt32 byte length, L

StringListRef64:
bytes 0..7:   UInt64 absolute offset to consecutive StringRef64 values
bytes 8..15:  UInt64 item count
```

Referenced ranges are valid only when `offset <= fileLength` and
`length <= fileLength - offset`. All rows share one file-wide variable region,
allowing repeated strings to point to the same bytes.

## File Header

Version 1 currently has eight table descriptors in fixed order.

```text
bytes 0..3:     Magic bytes `ORWA`
bytes 4..7:     UInt32 format version, currently 1
bytes 8..167:   Eight TableDescriptor records
```

Each `TableDescriptor` is 20 bytes:

```text
bytes 0..7:    UInt64 absolute offset to first row
bytes 8..15:   UInt64 row count
bytes 16..19:  UInt32 row size
```

Descriptors are ordered as follows:

```text
0 backends
1 frontends
2 frontend_layouts
3 log_files
4 content_types
5 file_openers
6 resources
7 containers
```

Tables are contiguous immediately after the header. The variable region starts
after the final table. Pre-release three-, four-, six-, and seven-table files
are accepted and rewritten in the current layout on the next mutation.

## Tables

### `backends`

Row size: 68 bytes.

```text
bytes 0..15:   StringRef64 service_id
bytes 16..31:  StringRef64 display_name
bytes 32..47:  StringRef64 unit_name
bytes 48..63:  StringRef64 unit_path
bytes 64..67:  UInt32 flags; bit 0 = owns_unit
```

### `frontends`

Row size: 80 bytes.

```text
bytes 0..7:    StringRef32 frontend_id
bytes 8..15:   StringRef32 service_id
bytes 16..23:  StringRef32 display_name
bytes 24..31:  StringRef32 icon_path
bytes 32..39:  StringRef32 suggested_list
bytes 40..41:  UInt16 endpoint_kind; 0 none, 1 TCP, 2 Unix
bytes 42..43:  UInt16 endpoint_flags
bytes 44..45:  UInt16 endpoint_scheme; 0 default, 1 HTTP, 2 HTTPS
bytes 46..47:  reserved
bytes 48..55:  StringRef32 URL path
bytes 56..79:  endpoint payload
```

For a TCP endpoint the payload contains a host `StringRef32` at bytes 56..63
and a `UInt16` port at bytes 64..65. For a Unix endpoint it contains socket and
external-socket `StringRef32` values at bytes 56..63 and 64..71.

The registry stores endpoint metadata, not runtime status. User placement in
`frontend_layouts` overrides `suggested_list` when present.

### `frontend_layouts`

Row size: 32 bytes.

```text
bytes 0..15:   StringRef64 URL
bytes 16..31:  StringRef64 list
```

### `log_files`

Row size: 32 bytes.

```text
bytes 0..15:   StringRef64 path
bytes 16..31:  StringRef64 service_id
```

### `content_types`

Row size: 96 bytes.

```text
bytes 0..15:   StringRef64 service_id
bytes 16..31:  StringRef64 identifier
bytes 32..47:  StringRef64 display_name
bytes 48..63:  StringListRef64 conforms_to
bytes 64..79:  StringListRef64 extensions
bytes 80..95:  StringListRef64 MIME types
```

### `file_openers`

Row size: 56 bytes.

```text
bytes 0..15:   StringRef64 extension or content type
bytes 16..31:  StringRef64 frontend_id
bytes 32..47:  StringRef64 URL template
bytes 48..51:  UInt32 rank
bytes 52..55:  UInt32 capability flags
```

### `resources`

Row size: 32 bytes.

```text
bytes 0..15:   StringRef64 key
bytes 16..31:  DataRef64 opaque payload
```

Resources let subsystems keep typed private state in the same atomic registry
without coupling every field to the daemon's core schema. Container provider
caches and recipes use keys under `containers/` and encode their payloads as
typed offset archives.

### `containers`

Row size: 104 bytes.

```text
bytes 0..15:    StringRef64 stable identifier
bytes 16..31:   StringRef64 display name
bytes 32..47:   StringRef64 provider identifier
bytes 48..63:   StringRef64 provider runtime name
bytes 64..79:   StringRef64 optional project resource key
bytes 80..87:   UInt64 creation time in Unix milliseconds
bytes 88..91:   UInt32 CPU ceiling
bytes 92..95:   UInt32 memory ceiling in GiB
bytes 96..99:   UInt32 flags; bit 0 = owns_container
bytes 100..103: reserved
```

The provider and runtime name locate the real container. An owned record lets
Outer Shell create, rebuild, migrate, and delete that runtime. An attached
record is observational: Outer Shell may start or stop it through its provider,
discover its apps, and relay its sockets, but unregistering it never deletes
the runtime or its data. This is the container equivalent of `owns_unit` on a
backend row.

## Typed Resource Archive

The `ORWV` archive is an offset-based value tree used inside resource payloads
and `.outershell-container` transfer manifests.

```text
bytes 0..3:    Magic bytes `ORWV`
bytes 4..7:    UInt32 version, currently 1
bytes 8..15:   UInt64 absolute offset to root node
```

Every node is 24 bytes:

```text
byte 0:        UInt8 value type
bytes 1..7:    reserved
bytes 8..15:   UInt64 scalar value or payload offset
bytes 16..23:  UInt64 payload byte length or child count
```

Value types are:

```text
2 Boolean       scalar is 0 or 1
3 signed integer, stored as two's-complement UInt64
4 IEEE-754 binary64, stored in the scalar bits
5 UTF-8 string  payload offset and byte length
6 data          payload offset and byte length
7 array         payload is consecutive UInt64 child-node offsets
8 dictionary    payload is sorted pairs of UInt64 key/value node offsets
9 date          IEEE-754 seconds since the Unix epoch in the scalar bits
```

Dictionary keys are strings and are sorted by their UTF-8 key value so the
same logical value has stable bytes across implementations.

## Locking And Atomic Writes

Writers coordinate through `registry.orwa.lock`. Readers open and validate a
snapshot without taking the writer lock. Writers take an exclusive lock, write
a unique temporary file, `fsync` it, rename it over `registry.orwa`, then
`fsync` the containing directory.

```text
registry.orwa.tmp.XXXXXX -> registry.orwa
```
