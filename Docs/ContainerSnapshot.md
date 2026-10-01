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

## Browser notifications and assets

The web page no longer polls layout or snapshots on a timer. It includes
`sinceOverview` in the existing `/api/events` long-poll request. The response
adds an overview-changed flag (bit 3) and a 64-bit overview token at offset 24.
The token combines snapshot content with the layout file state, so unchanged
discovery does not wake readers, and changes survive daemon restarts. Existing
backend and log fields remain at their original offsets. A quiet watch times out
after 25 seconds and reconnects without downloading overview data. Resume and
reconnection recovery still reconcile state. Active overview watches keep the
server snapshot refresh alive.

The server still periodically checks container state. Docker events alone do
not cover all endpoint registrations or process changes inside containers.
This is one shared server-side check, not a data fetch per browser.

Overview snapshots include terminal and registered command launchers, but omit recipes, mounts, and persistent-data details. Command icons use the same immutable asset URLs as endpoint icons.
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
