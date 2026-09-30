# Container snapshots

The web overview reads `GET /api/container-snapshot` (daemon UI route 13).
The daemon keeps the latest successful container-provider `list` response in
memory. This is derived state, separate from the persistent layout and registry.
It is shared across browsers and is rebuilt after a daemon restart.

The reactor starts an asynchronous discovery at startup and refreshes it roughly
every two seconds while snapshot requests are active. Refreshing stops after a
minute without readers; a later read returns the existing snapshot immediately
and resumes background discovery. There is at most one snapshot discovery in
flight. Cold readers wait for that shared discovery rather than spawning their
own providers.

Container operations still use `/api/safe-spaces` and receive their own result.
Starting and completing an operation advances a generation counter and schedules
a fresh snapshot. Discovery from an older generation cannot replace the cache.
Background discovery waits for active provider operations to finish.

Failed discovery, malformed JSON, and responses containing an error do not
replace the last successful snapshot. Without any successful snapshot, readers
receive an error and subsequent refreshes retry. No snapshot is stored in the
browser. Running indicators may lag the live container by a refresh interval.

The external-provider path uses this cache. The embedded macOS provider callback
retains its existing list behavior.

Run `python3 Scripts/test_container_snapshot.py /path/to/outershelld` for isolated
API coverage, or add `/path/to/OuterShellBackend` to test through HTTP. Tests cover
shared cold discovery, immediate cached reads, background updates, retention on
failure, and mutation/discovery races.
