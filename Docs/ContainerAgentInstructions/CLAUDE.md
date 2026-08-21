# Outer Shell container

Read and follow `AGENTS.md` in this directory. It is the canonical instruction
file for configuring this container.

In particular, preserve requested environment changes in
`/var/lib/outershell/project/Dockerfile`, consult
`/usr/local/share/outershell/OUTERCTL.md` before publishing tools, and leave
host-managed mounts, environment settings, persistent volumes, and secrets to
Outer Shell.

Prefer to mirror durable Dockerfile changes into the running container so the
user can continue immediately. Ask for a rebuild only when that is necessary
or significantly easier than safely producing the same live result.
