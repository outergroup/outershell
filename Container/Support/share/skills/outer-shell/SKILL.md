---
name: outer-shell
description: Make a web page, dashboard, or small web app that the user can open from Outer Loop on any of their devices, or add a terminal command they can launch. Also explains this Outer Shell container and how to make changes survive a rebuild.
metadata: { "openclaw": { "requires": { "bins": ["outerctl"] } } }
---

# Outer Shell container

You are running inside a container managed by Outer Shell. The user reaches
this container from Outer Loop, a browser on their Mac, iPhone, and other
devices. Web apps you publish here appear in Outer Shell next to the user's
other apps, and they open in Outer Loop from any of those devices.

Published apps are private to the user. Outer Loop reaches them through Unix
sockets over SSH, so they need no password, token, or open network port. Don't
publish anything on a network port, and don't add your own sign-in.

To make something the user can open:

1. Write a small web server. Keep its code with the container's build context
   (see below), not in `/tmp`, so it survives a rebuild.
2. Prefer a server that listens on a Unix socket; otherwise use a loopback TCP
   port.
3. Declare it with `outerctl image add-app` in the Dockerfile, then publish it
   in the running container too, as described in
   `/usr/local/share/outershell/OUTERCTL.md`.
4. Tell the user the app's name, so they can open it in Outer Shell.

## Preserve the result

First check whether `/var/lib/outershell/project` exists. If it does, this is a
self-modifying container: its build context is mounted there and you can edit
it. If it does not, this is a sealed container: its build context lives on the
host where you cannot see it. In a sealed container, make the change in the
running container, then give the user the exact Dockerfile lines (and any
files to place beside the Dockerfile) that would make it survive a rebuild,
and say that the live change will be lost on the next rebuild until they add
them. The rest of this section is for self-modifying containers.

The reproducible source of truth is the ordinary Docker build context mounted
at `/var/lib/outershell/project`. Its Dockerfile is:

```text
/var/lib/outershell/project/Dockerfile
```

When a requested change should survive a rebuild or move to another server,
edit that Dockerfile and any files beside it. Prefer portable, noninteractive,
idempotent Dockerfile instructions. A root process may edit this project
directly. Do not create a second build definition or initialization system.

Keep the line `COPY --from=outershell/support /outershell-rootfs/ /` directly
after the Dockerfile's final `FROM`. It installs Outer Shell's in-container
tools, which `outerctl image` lines and the container's startup depend on. You
may change the `FROM` image, but keep this line after it.

After recording a durable change, prefer to mirror it in the running container
so the user can use it immediately. For example, install the same packages,
write the same configuration, create the same directories, and start or
restart the affected service in the live instance. The Dockerfile remains the
source of truth; a live-only change is not complete.

You have two acceptable ways to finish a durable change:

1. Preferably, update the Dockerfile and mirror the result in the live
   container. Tell the user that the change is available now and will also be
   reproduced by future rebuilds.
2. Ask the user to rebuild the container when mirroring is impossible, unsafe,
   or significantly more difficult than rebuilding. Explain why the rebuild is
   needed.

Do not make the user rebuild merely because the Dockerfile changed. Build-only
instructions such as `COPY`, `ENV`, `USER`, and `VOLUME` may require a rebuild,
but mirror their intended effect when a safe runtime equivalent exists. Do not
run Docker, `container`, or another host container engine from inside this
container.

Folder mounts, persistent volumes, environment variables, and secrets are
runtime configuration owned by Outer Shell. Do not add, remove, or change host
mounts from inside the container. Treat explicitly mounted user folders as
user data and modify their contents only when the request calls for it. Never
put credentials or secret values in the Dockerfile or image.

## Publish tools in Outer Shell

Outer Shell support is installed in this image. Use build-time `outerctl image`
commands in the Dockerfile so published tools return after every rebuild:

- `outerctl image add-app` publishes a browser or Outerframe app backed by a
  managed service.
- `outerctl image add-command` publishes a command-line tool whose launch
  command can be copied from Outer Shell.

Read `/usr/local/share/outershell/OUTERCTL.md` before adding or changing a
published tool. Use stable, descriptive IDs. Reference icon files by absolute
path with `--icon-path`; there is no built-in icon-name registry.

After editing the Dockerfile, summarize both the durable change and anything
you mirrored into the live container. Ask the user to use Outer Shell's
**Rebuild** action only when the live result could not reasonably be produced.
Do not claim a newly declared app or command is available until it has been
published in the running container or by a rebuild.
