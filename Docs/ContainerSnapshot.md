# Container snapshots

The web overview reads `GET /api/container-snapshot` (daemon UI route 13). The
reply is an ORWV value (see `outershell-registry.md`) with `providers` and
`workspaces` arrays. The daemon keeps the snapshot from the latest successful
container-provider `list` reply in memory. This is derived state, separate from the persistent layout and registry.
It is shared across browsers and is rebuilt after a daemon restart.

The reactor starts an asynchronous discovery at startup and refreshes it roughly
every two seconds while snapshot requests are active. Refreshing stops after a
minute without readers; a later read returns the existing snapshot immediately
and resumes background discovery. There is at most one snapshot discovery in
flight. Cold readers wait for that shared discovery rather than spawning their
own providers.

Container operations use `POST /api/containers` and receive their own result
(see "Container messages" below).
Starting and completing an operation advances a generation counter and schedules
a fresh snapshot. Discovery from an older generation cannot replace the cache.
Background discovery waits for active provider operations to finish.

Failed discovery, malformed replies, and replies carrying an error do not
replace the last successful snapshot. Without any successful snapshot, readers
receive an error and subsequent refreshes retry. No snapshot is stored in the
browser. Running indicators may lag the live container by a refresh interval.

Both the external Linux provider and the embedded macOS provider feed this cache.

Run `python3 Scripts/test_container_snapshot.py /path/to/outershelld` for isolated
API coverage, or add `/path/to/OuterShellBackend` to test through HTTP. Tests cover
shared cold discovery, immediate cached reads, background updates, retention on
failure, and mutation/discovery races.

## Container messages

Every container request is a binary message; no part of the container API uses
JSON. `POST /api/containers` carries one message as its body, and its reply body
is the matching reply message. The same messages work directly on the API socket
(`outerctl container rebuild` and the other action commands use them).

- `containerUpsertRequest` (31), `containerRemoveRequest` (32), and
  `containerListRequest` (33) change and read registry rows; see
  `outerctl-socket-messages.md`. The replies are `commandResponse` (100) and
  `containerListResponse` (108).
- `containerRequest` (35) asks the container provider to do something:

  ```text
  bytes 0..1:    UInt16 message type, 35
  bytes 2..3:    UInt16 operation code
  bytes 4..7:    UInt32 flags; 0x01 = overview (omit definitions, use icon URLs)
  bytes 8..15:   StringRef32 container identifier, empty for host-wide operations
  bytes 16..23:  DataRef32 ORWV dictionary of arguments, may be empty
  ```

- `containerSnapshotRequest` (36) returns the cached snapshot without running the
  provider. It has no fields beyond its type.
- `containerResponse` (109) answers 35 and 36:

  ```text
  bytes 0..1:    UInt16 message type, 109
  bytes 2..5:    UInt32 status; 0 = success
  bytes 6..13:   StringRef32 error message when the status is not 0
  bytes 14..21:  DataRef32 ORWV dictionary with the operation's result, may be empty
  bytes 22..29:  DataRef32 ORWV snapshot with `providers` and `workspaces`, may be empty
  ```

Operation codes, in order from 0: `list`, `checkRuntimes`, `containerStorage`,
`cleanContainerStorage`, `cleanContainerBuildCache`, `create`, `duplicate`,
`changeRuntime`, `rename`, `start`, `stop`, `restart`, `delete`, `mountFolder`,
`unmountFolder`, `chooseFolder`, `updateDockerfile`, `updateContainerConfiguration`,
`updateResources`, `rebuild`, `startApp`, `stopApp`, `restartApp`, `appLogs`,
`publishSocket`, `prepareShare`, `beginImport`, `cancelImport`, `finishImport`.
A provider answers an operation it does not support with an error. Arguments use
the same names on every provider (for example `updateDockerfile` takes
`dockerfile`). Clients merge the result and snapshot dictionaries when they
need both. Large provider payloads that are themselves binary, such as an app's
log, are carried as base64 strings inside the result for now.

`Scripts/test_container_messages.py` checks that the web UI, the native UI, and
the Linux provider encode and decode these messages identically.

## Browser notifications and assets

The web page no longer polls layout or snapshots on a timer. It includes
`sinceOverview` in the existing `/api/events` long-poll request. The response
adds an overview-changed flag (bit 3) and a 64-bit overview token at offset 24.
The token combines snapshot content with the layout file state, so unchanged
discovery does not wake readers, and changes survive daemon restarts. Existing
backend and log fields remain at their original offsets. When the overview flag is
set, a data reference at offset 32 (u32 offset, u32 length) holds the current
layout record in the `/api/layout` format; it is empty otherwise. A quiet watch times out
after 25 seconds and reconnects without downloading overview data. Resume and
reconnection recovery still reconcile state. Active overview watches keep the
server snapshot refresh alive.

The server still periodically checks container state. Docker events alone do
not cover all endpoint registrations or process changes inside containers.
This is one shared server-side check, not a data fetch per browser.

Overview snapshots include terminal and registered command launchers, but omit container definitions, mounts, and persistent-data details. Command icons use the same immutable asset URLs as endpoint icons.
The configuration sheet obtains the full provider response when opened.
Host web requests use `/api/backends?web=1`: frontend flag bit 1 indicates that
the icon reference contains a URL instead of inline PNG bytes. Native requests
retain the inline representation. Container overview endpoints use `iconURL`.

Icons are derived files in `web-icons` under the Outer Shell state directory.
`/api/icon?key=…` accepts only hexadecimal content keys and returns PNG bytes
with `Cache-Control: private, max-age=31536000, immutable`. Changed bytes get a
new URL; browser HTTP caching is only for these assets, not user preferences.
Old derived icon files are currently retained; automatic cache pruning is not
implemented.
