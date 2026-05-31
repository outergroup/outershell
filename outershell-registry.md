# OuterShell Registry Binary Format

This file format is the source of truth for the outershell registry. Older
`registry.sqlite3` files are read only by migration code, which publishes a
`registry.orwa` file next to the old database.

All scalar values are little-endian. Strings are UTF-8 without a trailing NUL.
Offsets are absolute offsets from byte 0 of the file. A string or data reference
with `offset = 0` and `length = 0` means the value is absent or empty.

## Shared References

```text
StringRef64:
bytes 0..7:   UInt64 little-endian offset to UTF-8 bytes, O
bytes 8..15:  UInt64 little-endian UTF-8 byte length, L

DataRef64:
bytes 0..7:   UInt64 little-endian offset to raw bytes, O
bytes 8..15:  UInt64 little-endian data length, L
```

Referenced ranges are valid only when `offset <= fileLength` and
`length <= fileLength - offset`.

All table rows share a single file-wide variable region. This means common
values such as a backend `service_id` can point to the same bytes from multiple
tables.

## File Header

The fixed header stores the location, row count, and row size for each table.
Version 1 has exactly four table descriptors in fixed order.

```text
bytes 0..3:     Magic bytes `ORWA`
bytes 4..7:     UInt32 format version, currently 1

bytes 8..87:    Four TableDescriptor records
```

`TableDescriptor` is 20 bytes:

```text
bytes 0..7:    UInt64 absolute offset to first row
bytes 8..15:   UInt64 row count
bytes 16..19:  UInt32 row size
```

The table descriptors are in this order:

```text
0 backends
1 frontends
2 frontend_layouts
3 log_files
```

Tables are stored contiguously immediately after the header in descriptor order.
The variable region starts immediately after the last table.

## Tables

### `backends`

Row size: 68 bytes.

```text
bytes 0..15:   StringRef64 service_id
bytes 16..31:  StringRef64 display_name
bytes 32..47:  StringRef64 unit_name
bytes 48..63:  StringRef64 unit_path
bytes 64..67:  UInt32 flags
                bit 0 = owns_unit
```

### `frontends`

Row size: 113 bytes.

```text
bytes 0..15:    StringRef64 url
bytes 16..31:   StringRef64 service_id
bytes 32..47:   StringRef64 display_name
bytes 48..63:   StringRef64 icon_path
bytes 64..79:   StringRef64 suggested_list
byte 80:        UInt8 endpoint_kind
                 0 = none
                 1 = port
                 2 = socket_path
bytes 81..96:   EndpointPayload
bytes 97..112:  StringRef64 frontend_id

EndpointPayload when endpoint_kind = 1:
bytes 81..84:    UInt32 port
bytes 85..96:    zero-filled

EndpointPayload when endpoint_kind = 2:
bytes 81..96:    StringRef64 socket_path
```

The registry stores app metadata and endpoint hints, not runtime status. Outer
Shell derives whether an app is running from the registered service manager
unit.

`suggested_list` is announced by the backend. User placement is stored in
`frontend_layouts`; when a layout row exists for a URL, it overrides
`suggested_list`, including when the layout string is empty.

### `frontend_layouts`

Row size: 32 bytes.

```text
bytes 0..15:   StringRef64 url
bytes 16..31:  StringRef64 list
```

### `log_files`

Row size: 32 bytes.

```text
bytes 0..15:   StringRef64 path
bytes 16..31:  StringRef64 service_id
```

## Migration From SQLite

The migration/export step reads these SQLite tables:

```text
backends(service_id, display_name, unit_name, unit_path, owns_unit)
frontends(url, service_id, display_name, port, socket_path, icon_path, suggested_list, frontend_id)
frontend_layouts(url, list)
log_files(path, service_id)
```

Current migration code also understands older SQLite registries that keep
systemd and launchd metadata in side tables. Those values are folded into the
backend row during export. Older backend icon columns are ignored; app/frontend
icons remain part of the `frontends` table.

Missing optional string values become empty references. Missing boolean values
become zero.

## Locking And Atomic Writes

Writers coordinate using a sidecar file:

```text
registry.orwa.lock
```

Readers do not take a lock. They open `registry.orwa`, read the file they
opened, and validate bounds while parsing. A writer replacing the path with
`rename` does not affect readers that already opened the old file.

Writers take an exclusive lock, write a unique temp file, `fsync` it, rename it
over `registry.orwa`, then `fsync` the containing directory.

The write path is:

```text
registry.orwa.tmp.XXXXXX -> registry.orwa
```
