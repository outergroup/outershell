# Server-owned web layout

Outer Shell stores endpoint display names, pinned endpoints, endpoint list order,
and user/container card order on the server. This metadata is separate from the
registry's discovered service records, so discovery and container refreshes cannot
replace user choices. The record belongs to the daemon's user: all browsers using
that daemon share a layout, including its view of root services and containers.

## API

`GET /api/layout` reads the record. `POST /api/layout` replaces it using a
compare-and-swap revision. Both are proxied to outershelld (UI route IDs 11 and 12).
The content type is `application/octet-stream`; the format is:

- Bytes 0–7: ASCII `OSLAY001`.
- Bytes 8–15: unsigned 64-bit little-endian revision.
- Remaining bytes: a UTF-8 JSON object, maximum total record size 256 KiB.

GET on an uninitialized server returns revision 0 and `{}`. POST must supply the
revision it read. A successful write increments the revision and returns the new
record. A stale write returns HTTP 409 without modifying storage. Invalid framing,
JSON, encoding, excessive nesting, or size returns 400. Storage failures and
corrupted existing records return 500; corrupt records are never reset silently.

The web layout schema (version 1) is:

```json
{
  "version": 1,
  "pins": { "user": ["endpoint-key"] },
  "order": { "user": ["another-endpoint-key"] },
  "groups": ["container:workspace-id", "user", "root"],
  "names": { "endpoint-key": "Display name" }
}
```

Group IDs are `user`, `root`, and `container:<workspace-id>`. Endpoint keys reuse
the stable JSON-encoded identities in `hostBookmarkKey` and
`containerBookmarkKey`; container keys use internal frontend identities, not
transient host relay addresses. These historical helper names do not imply a
favorites feature.

Array position is the ordinal. A missing pin list uses the Files/Top defaults;
an empty list explicitly pins nothing. Unlisted endpoints/cards follow saved
entries in discovery order. The lists are small, so replacing an ordered array
is simpler than floating-point ranks, which eventually require rebalancing.
Missing endpoints are omitted from display without discarding saved preferences.

The daemon validates framing, encoding, and JSON syntax; the web client validates
the versioned layout schema. Other API clients must preserve the same schema.
The daemon writes `<registry-path>.layout` under an advisory lock, with mode 0600,
a temporary file, fsync, atomic rename, and directory fsync. No SQLite dependency
is introduced. Include this sidecar in backups of the registry.

## Web synchronization

The web client holds an in-memory cache only. Changes render optimistically;
failed writes restore the previous layout. Conflicts reload the server's layout
and ask the user to retry, rather than overwriting another client's changes.
Other open pages refresh the layout every two seconds while visible. Refreshes
are deferred during dragging or saving; older revisions never replace newer ones.

An uninitialized server uses the default layout until the first edit is saved.
The web client does not read or write browser storage. The one-time legacy layout
migration has been removed; existing server records remain unchanged.

## Verification

Compile outershelld using the same sources and libraries as
`Scripts/test_registry_list_responses.sh`, then run:

```sh
python3 Scripts/test_layout_storage.py /path/to/outershelld [/path/to/OuterShellBackend]
node Scripts/test_web_layout.cjs .
```

The storage test uses temporary state and sockets and covers revisions, concurrent
writers, malformed inputs, restart persistence, permissions, and corrupt-record
protection. Supplying the HTTP backend also tests GET/POST through `/api/layout`.
The web test covers empty-server defaults, cross-client updates, failed writes,
conflicts, stale responses, and the absence of browser storage access.
