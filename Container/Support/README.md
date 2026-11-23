# Outer Shell container support image

Managed containers get Outer Shell's in-container tools from a small local
image named `outershell/support`. Dockerfiles copy it in right after `FROM`:

```dockerfile
FROM debian:bookworm
COPY --from=outershell/support /outershell-rootfs/ /
```

Each container provider builds the image on its own machine from this folder,
with the static Linux `outershelld`, `outerctl`, and `outershell-socket-bridge`
for the container engine's architecture placed in `bin/`. The provider appends
`LABEL org.outershell.support-context=<sha256 of the build context>` and
rebuilds the image under the same tag whenever that hash changes, so the tools
inside a container always match the host's Outer Shell.

Providers also supply what a base image would otherwise provide, when they
create the container: the entrypoint `/usr/local/bin/outershell-container-init`,
user `0`, and the environment variables `XDG_RUNTIME_DIR=/run/user/0` and
`OUTERSHELLD_API_SOCKET=/run/user/0/outershelld-api`.
