# Outer Shell Uninstall Considerations

_(This is a quick analysis written by a coding agent, so it can use it as a reference in the future.)_

Outer Shell uninstall is more delicate than normal install because the code doing the uninstall may be part of the service being removed. This is most visible on Linux, where the UI request arrives in `OuterShellBackend`, which asks `outershelld` to run the public installer in `uninstall` mode. During that operation, the uninstall may stop and remove both `OuterShellBackend` and `outershelld`.

## Why It Is Delicate

The uninstall path has to cross several boundaries:

- The request starts in the browser-facing Outer Shell backend.
- `outershelld` launches the installer script.
- The installer edits the registry through `outerctl`.
- The installer disables/removes systemd or launchd service files.
- A final cleanup process removes files after the running services have exited.

This means uninstall cannot be treated as a normal synchronous backend operation. If `outershelld` waits for the installer to finish, and the installer calls `outerctl`, it can deadlock because `outerctl` talks back to the same `outershelld` process. If `outershelld` simply backgrounds a shell child, systemd may kill that child when the service command exits.

## Linux Strategy

On Linux, `outershelld` starts update and uninstall work with a transient systemd unit:

- Wrapper unit: `org.outershell.OuterShell-installer-update` or `org.outershell.OuterShell-installer-uninstall`
- Cleanup unit: `org.outershell.OuterShell-uninstall`

The wrapper unit runs the installer script outside the lifetime of the request-handling process. The installer can then use `outerctl`, stop services, and arrange a cleanup unit without being killed by systemd when the original backend request completes.

The wrapper and cleanup unit names must remain distinct. If they collide, systemd may reject the cleanup unit because the wrapper unit is still active, causing only a partial uninstall.

## User, Root, And Shared Root Support

Outer Shell can be installed in user scope, direct-root scope, or user scope with shared root support. Linux root support installs shared copies of `outershelld` and `outerctl` under `/var/lib/outershell`, tracked by marker files in:

```text
/var/lib/outershell/system-binary-users/
```

These marker files let uninstall decide whether shared root binaries are still needed. A complete machine cleanup requires uninstalling bundled/root apps first, then uninstalling Outer Shell. Otherwise, shared root support may remain because apps still depend on it.

Direct-root sessions need special care. They should use the system registry and system units, not `/root/.local/state/outershell` or `/root/.config/systemd/user`. Installer code should not assume `HOME` is present in root systemd services; it should derive a home directory when needed.

## Registry And Cache Cleanup

Before uninstalling Outer Shell, the UI/backend should determine whether any registered backends remain while `outershelld` is still available. That result is passed into the uninstall script so the script can decide whether it is safe to remove registry state.

The uninstall script should remove:

- Outer Shell service files and sockets.
- Outer Shell app payload files.
- `outershelld` and `outerctl` files when no marker still depends on them.
- registry files when no backends remain.
- installer and bundled-app caches that belong to Outer Shell.

Runtime sockets may survive briefly after service removal, so cleanup should explicitly unlink known socket paths where possible.

## Failure Modes To Watch For

- Background shell launched from a systemd service is killed before cleanup finishes.
- Synchronous uninstall deadlocks because `outerctl` calls back into `outershelld`.
- Wrapper and cleanup transient systemd units use the same unit name.
- Root service environment lacks `HOME`.
- Direct-root install accidentally writes user-scope root files under `/root`.
- Shared root binaries are removed while root-installed apps still need `outerctl`.
- Registry state is removed before uninstall code knows whether other backends remain.

The safe shape is: collect needed state first, launch uninstall outside the current service lifetime, keep `outerctl` reentrancy possible, then let a separate cleanup unit remove files after services have stopped.
