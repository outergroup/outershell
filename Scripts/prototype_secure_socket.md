# SecureSocketWithKey prototype

Run on macOS with an active GUI login session:

```sh
python3 Scripts/prototype_secure_socket.py --installed-apps
```

Omit `--installed-apps` to test only the small C server. `--probe-binary PATH`
uses an already compiled server instead of invoking clang and codesign. Python 3
is required for this harness. No installed plist is changed. Temporary jobs use
unique labels and are unloaded in cleanup. Socket files are removed; empty
launchd-created directories under protected `/var/run` may remain.

## Results, October 6, 2026

- macOS 27.0.1 (26A434), localhost: the probe and installed Top, Files, and
  Plaintext backend binaries all served HTTP 200 using SecureSocketWithKey.
- Each job was unloaded and loaded again. The generated path changed and the
  second cycle passed. The small server exits after one request; two requests
  per cycle activated distinct processes successfully, with activation return 0.
- macOS 14.8.9 (23J631), SSH to MarcBook-Intel.local: the universal C probe passed
  the same two-cycle, two-request activation experiment. A shell equivalent of
  the harness was used because the remote machine has no developer tools.
- Socket ownership matched the user, socket permissions were 0600, and the
  generated containing directory was 0700. Launchd chose `/var/run` on macOS 27
  and `/private/tmp` on macOS 14; the client must not assume either prefix.
- Discovery succeeded before the target job ran. A one-shot job bootstrapped
  into `gui/501` ran `/usr/bin/printenv` for the SecureSocketWithKey variable and
  wrote the value to an output file in the test's private directory. This worked
  locally and when requested over SSH.
- On the SSH connection, plain `launchctl getenv` returned nothing and
  unprivileged `launchctl asuser` failed with an audit-session permission error.
  Neither is required by the one-shot resolver.
- Bootout removed the exported environment variable but left a socket file.
  File existence alone is therefore not service availability.

The harness parses `launchctl print` only to assert job state and cross-check
results. Socket discovery itself uses the one-shot resolver's inherited
environment, not diagnostic-output parsing.

For installed backends, a temporary shell wrapper passes the generated path
from the inherited variable through `--socket-path`, alongside
`--launchd-socket-name Listener`. Files gets an isolated, unused API socket path
so it cannot announce this test instance to the live Outer Shell registry.
HTTP readiness was tested, not the full browser/native UI.

## Production integration still needed

Use stable application identity to resolve an ephemeral endpoint. Backend code
should obtain the actual path from the activated descriptor (`getsockname`) or
the named environment variable so announcements and displayed paths are correct.

A shared resolver can batch endpoint lookups in the user's launchd domain. Its
output must be validated and fed into the same exact-path allowlist used by
localhost connections and SSH tunnels. Refresh after job reload/reinstall; do
not authorize all of `/var/run` or `/private/tmp`. Stable links should retain app
identity rather than a generated socket path.

Outer Shell's own initial discovery needs this resolver too, before its HTTP
API is available. The experiment used an existing GUI login on both machines;
logout, reboot, concurrent reloads, missing jobs, and SSH access without a GUI
login still need lifecycle coverage before production migration.

This establishes a viable socket-placement and discovery mechanism. It does
not establish the cause of the original top-level temporary-directory failure.
