---
name: outer-shell
description: Install, register, run, and troubleshoot apps and web backends in Outer Shell on the current server or a user-specified SSH server.
---
# Outer Shell

Outer Shell lists and manages services on a server. Outer Loop opens their web interfaces locally or through SSH forwarding. The requested outcome is a working app in Outer Shell, including its launcher and logs.

Read https://outershell.org/api/ for the current outerctl commands and platform service setup. Discover the target OS and existing registrations before changing anything. If running through Outer Shell's OpenCode launcher, you are already on the target server; do not SSH elsewhere unless requested.

When `/var/lib/outershell/project/Dockerfile` exists, you are inside an Outer Shell container. Follow `/usr/local/share/outershell/container-agent.md` and read `/usr/local/share/outershell/OUTERCTL.md`. Keep durable package installs, service definitions, and app registrations in that Dockerfile and its build context. Mirror changes into the running container when practical so the user can use them immediately. Use `outerctl image add-app` in Dockerfile RUN instructions for web apps, or `outerctl image add-command` for terminal tools. Persist experiments, datasets, notebooks, logs, and model-provider state in declared volumes or existing mounted folders. Do not put credentials into the image. Outer Shell owns the runtime, mounts, and rebuild operation; do not invoke the host container engine from inside the container. Verify both the running app and its appearance under the container in Outer Shell.

Find outerctl on PATH, or on macOS at "$HOME/Library/Application Support/outershell/bin/outerctl", or on Linux at "${XDG_STATE_HOME:-$HOME/.local/state}/outershell/bin/outerctl". Run it as the account that owns the apps. Use `outerctl backend list` and `outerctl app list` to inspect existing registrations. Use outerctl to update the registry, never edit registry.orwa directly.

Reuse existing tools and environments when appropriate. For a requested experiment or project, locate its actual files; ask when the intended directory is ambiguous. Respect requested ports, check for conflicts, and preserve unrelated services. Prefer Unix sockets when the tool supports them; otherwise bind to 127.0.0.1. Outer Loop supplies forwarding, so public binding is unnecessary.

Make the registered app open ready to use. For a private, per-user registration, when the app supports URL-token authentication, register its authenticated launch URL (including the token) instead of asking the user to retrieve and paste a token. Keep authentication enabled for TCP listeners and bind them to loopback; prefer a permission-restricted Unix socket when supported. Use an app-specific persistent token in a private file, or have the launcher refresh the registration when the token changes. URL-encode query values and preserve the app's existing path and query parameters. This applies to the app's launch token, not unrelated account credentials or system-wide registrations. Never bake tokens into container images or print them in responses. See the authenticated launch URL example at https://outershell.org/api/#authenticated-launch-urls.

Manage persistent processes with launchd on macOS, systemd where available on Linux, or Outer Shell's internal manager on init-less hosts. Consult the documentation for Windows service integration. Register the backend, app endpoint, and logs. Backend IDs are stable; icons belong to app entries.

Verify both that the process starts and that its HTTP endpoint responds. Confirm registration with outerctl, then verify the registered launch URL in a fresh session without existing login cookies. An HTTP 200 login page is not a successful authenticated launch. Tell the user which app to open without printing its token. Inspect service logs on failure. Do not report an app as ready merely because installation or registration succeeded. When asked to remove an app, preserve project data unless deletion was requested.
