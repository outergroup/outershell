# Outer Shell Container Cookbook

These snippets are ordinary Dockerfile instructions. They can be used in Outer
Shell, `docker build`, or any OCI builder that supports Dockerfile syntax.

## Choose a base image

Outer Shell's prepared image is the shortest path:

```dockerfile
FROM outershell/container-base:7
```

An ordinary Linux image works too:

```dockerfile
FROM debian:bookworm
```

When an ordinary image is selected, Outer Shell offers to add this portable
project fragment immediately after `FROM`:

```dockerfile
COPY --chmod=0755 .outershell/bootstrap/ /tmp/outershell-bootstrap/
RUN /tmp/outershell-bootstrap/install.sh && rm -rf /tmp/outershell-bootstrap
```

The `.outershell/bootstrap` folder contains the small Linux binaries and startup
script required to announce apps to Outer Shell. The prepared base image contains
the same support already, so it does not need this fragment.

If a custom image already provides compatible `outershelld`, `outerctl`, and the
Outer Shell container-init entry point, turn off **Install Outer Shell support**.
The generated Dockerfile will then rely on the base image's installation.

## Install an Outer Shell app

Bundled apps are distributed as small OCI images whose filesystem payload lives
under `/outershell-rootfs`. For example, install Files and Plaintext with
ordinary multi-stage copies:

```dockerfile
COPY --from=ghcr.io/outergroup/outershell-app-files:4 /outershell-rootfs/ /
COPY --from=ghcr.io/outergroup/outershell-app-plaintext:4 /outershell-rootfs/ /
```

This is standard Dockerfile syntax rather than an Outer Shell-only installer.
The same fragment works with Apple `container`, Docker, and compatible builders.
An app image can contain its backend, service declaration, and registration
script without becoming the base image for the rest of the container.

Third-party app publishers can use the same contract: assemble every path that
the app needs beneath `/outershell-rootfs` in a `scratch` output stage, publish
that stage as an OCI image, and provide a `COPY --from` snippet like the one
above.

## Give container-configuration tools their instructions

Codex CLI reads `AGENTS.md`, and Claude Code reads `CLAUDE.md`, from their
working directory. Outer Shell provides matching instruction files that explain
the container boundary, identify the mounted Dockerfile as the reproducible
source of truth, and document how tools are published with `outerctl`.

Copy the files from `ContainerAgentInstructions` into the Dockerfile build
context, then include them in the image:

```dockerfile
RUN mkdir -p /work /usr/local/share/outershell
COPY AGENTS.md CLAUDE.md /work/
COPY OUTERCTL.md /usr/local/share/outershell/OUTERCTL.md
```

Use `/work` as the working directory for command-line tools that should act as
container configuration assistants. Both tools receive the same contract,
while `OUTERCTL.md` remains available as detailed, versionable documentation.
The instruction files direct root-capable tools to edit
`/var/lib/outershell/project/Dockerfile`, which Outer Shell mounts from the
server as the container's ordinary OCI build context.

## Install and publish Codex CLI

Install the standalone Codex CLI. The native installer does not require Node.js
or npm. Its build-time `CODEX_HOME` keeps the installed package in the image,
while the runtime `CODEX_HOME` points Codex at separately persisted state:

```dockerfile
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl \
    && curl -fsSL https://chatgpt.com/codex/install.sh \
        | CODEX_HOME=/opt/codex-cli \
          CODEX_NON_INTERACTIVE=1 \
          CODEX_INSTALL_DIR=/usr/local/bin sh \
    && rm -rf /var/lib/apt/lists/*

ENV CODEX_HOME=/var/lib/codex
VOLUME ["/var/lib/codex"]
```

Then publish an interactive command launcher. This is distinct from
`image add-app`: the command runs only after someone copies and invokes it in a
terminal. Put any PNG, JPEG, GIF, TIFF, or BMP icon in the Dockerfile's build
context, copy it into the image, and register its absolute path. The example
asset is available at `ContainerCookbookAssets/codex-cli.png`:

```dockerfile
COPY codex-cli.png /usr/local/share/outershell/icons/codex-cli.png

RUN outerctl image add-command \
    --id codex \
    --name "Codex CLI" \
    --working-directory /work \
    --user root \
    --icon-path /usr/local/share/outershell/icons/codex-cli.png \
    -- /usr/local/bin/codex --dangerously-bypass-approvals-and-sandbox
```

Outer Shell shows **Codex CLI (copy command)** on the container. The copied
command opens Codex interactively inside this container and starts it in
`/work`. Codex can change anything available to its container user without
additional approval prompts. Any explicitly mounted folders are also within
that boundary and remain accessible according to their mount permissions. The
standard `VOLUME` declaration tells Outer Shell to preserve Codex's login and
settings across rebuilds without putting credentials in the image. During the
build, the native installer puts its downloaded package under `/opt/codex-cli`
and creates `/usr/local/bin/codex` as its stable launcher. At runtime,
`CODEX_HOME=/var/lib/codex` gives Codex a distinct location for mutable login,
configuration, history, and session data. Persisting that directory therefore
does not hide any part of the installed program.

## Install and publish Claude Code

Install Claude Code with Anthropic's native installer. It does not require
Node.js or npm. The installer places its root-owned launcher at
`/root/.local/bin/claude`:

```dockerfile
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl \
    && curl -fsSL https://claude.ai/install.sh | bash \
    && rm -rf /var/lib/apt/lists/*

ENV CLAUDE_CONFIG_DIR=/root/.claude
VOLUME ["/root/.claude"]
```

Register its terminal command separately. The example asset is available at
`ContainerCookbookAssets/claude-code.png`:

```dockerfile
COPY claude-code.png /usr/local/share/outershell/icons/claude-code.png

RUN outerctl image add-command \
    --id claude \
    --name "Claude Code" \
    --working-directory /work \
    --user root \
    --icon-path /usr/local/share/outershell/icons/claude-code.png \
    -- /root/.local/bin/claude
```

Repeat `image add-command` with another ID, working directory, user, or command
arguments when one image should expose several distinct terminal workflows.
Outer Shell gives every declared `VOLUME` stable, container-specific storage,
so Claude Code's credentials and settings survive image rebuilds as well.
`CLAUDE_CONFIG_DIR` is Claude Code's supported configuration-directory
override. It keeps credentials, account state, settings, sessions, and plugins
together under the one persisted path instead of splitting state between
`~/.claude` and `~/.claude.json`.

## Install Python

These examples target Debian and Ubuntu images. Install the distribution's
Python runtime independently from any particular Python application:

```dockerfile
RUN apt-get update \
    && apt-get install -y --no-install-recommends python3 python3-venv \
    && rm -rf /var/lib/apt/lists/*
```

Create a project environment and choose its libraries in a separate, editable
fragment. JupyterLab is one package in this environment rather than the thing
that owns Python:

```dockerfile
RUN python3 -m venv /opt/venvs/default \
    && /opt/venvs/default/bin/pip install --no-cache-dir \
        jupyterlab \
        matplotlib \
        numpy \
        pandas \
        scipy
ENV PATH="/opt/venvs/default/bin:${PATH}"
```

Add, remove, and pin packages to match the project. A published or long-lived
image should generally pin the versions that matter to its results.

## Publish JupyterLab as an Outer Shell app

Installing JupyterLab does not decide how it should appear. This separate step
publishes one app rooted at `/work`:

```dockerfile
RUN outerctl image add-app \
    --id jupyter-work \
    --name "JupyterLab: work" \
    --url /lab \
    --working-directory /work \
    --socket-argument=--ServerApp.sock \
    -- /opt/venvs/default/bin/jupyter-lab \
        --allow-root \
        --no-browser \
        --ServerApp.sock_mode=0600 \
        --IdentityProvider.token= \
        --ServerApp.password= \
        --ServerApp.root_dir=/work
```

`outerctl image add-app` writes the portable service declaration and startup
registration into the image. It chooses the Unix socket and log paths, and it
does not require a running daemon during the build.

Create additional JupyterLab apps by repeating the step with another ID, name,
and working directory. They can share the same Python environment.

## Install R

R is likewise independent from RStudio Server:

```dockerfile
RUN apt-get update \
    && apt-get install -y --no-install-recommends r-base \
    && rm -rf /var/lib/apt/lists/*
```

Project-specific R packages can be another ordinary layer:

```dockerfile
RUN Rscript -e \
    'install.packages(c("data.table", "ggplot2"), repos="https://cloud.r-project.org")'
```

## Install RStudio Server

This fragment selects Posit's Debian package for the target architecture. It
expects R to have been installed by the preceding fragment:

```dockerfile
RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends bash ca-certificates curl socat; \
    case "$(dpkg --print-architecture)" in \
        arm64) package_url='https://s3.amazonaws.com/rstudio-ide-build/server/jammy/arm64/rstudio-server-2026.07.0-139-arm64.deb' ;; \
        amd64) package_url='https://download2.rstudio.org/server/jammy/amd64/rstudio-server-2026.07.1-147-amd64.deb' ;; \
        *) echo 'Unsupported architecture' >&2; exit 1 ;; \
    esac; \
    curl --fail --location --retry 4 --retry-delay 2 \
        --output /tmp/rstudio-server.deb "${package_url}"; \
    apt-get install -y --no-install-recommends /tmp/rstudio-server.deb; \
    rm -f /tmp/rstudio-server.deb; \
    rm -rf /var/lib/apt/lists/*
```

## Publish RStudio Server as an Outer Shell app

RStudio Server speaks TCP. `--tcp-port` tells Outer Shell to create the
loopback-to-Unix-socket adapter used by container apps:

```dockerfile
RUN outerctl image add-app \
    --id rstudio \
    --name "RStudio Server" \
    --tcp-port 8787 \
    -- /usr/lib/rstudio-server/bin/rserver \
        --server-user=root \
        --server-daemonize=0 \
        --server-working-dir=/root \
        --www-address=127.0.0.1 \
        --www-port=8787 \
        --www-thread-pool-size=2 \
        --auth-none=1 \
        --auth-minimum-user-id=0
```

The TCP adapter requires `bash`, `curl`, and `socat`; the installation fragment
above includes them.

## Publish a TCP port to this server

`outerctl image add-app --tcp-port` keeps an application available to Outer
Loop through its normal Unix-socket route. It does not publish that port from
the container to the server.

To also use the application in an ordinary browser, open **Edit container**, go
to **Ports**, and add one mapping per line:

```text
4000:4000
```

The first number is the port on the server and the second is the port inside
the container. Outer Shell binds the server port to `127.0.0.1`, so it is only
reachable from that server by default. The application inside the container
must listen on `0.0.0.0`, rather than `127.0.0.1`, for the runtime to forward
connections to it.

You may also include `EXPOSE 4000` in the Dockerfile to document the
application's container port. `EXPOSE` is OCI image metadata; it does not
create the server-to-container mapping by itself.

## Publish another app

For another application that can listen on a Unix socket:

```dockerfile
RUN outerctl image add-app \
    --id my-app \
    --name "My App" \
    --url / \
    --working-directory /srv/my-app \
    --socket-argument=--socket \
    -- /srv/my-app/server --no-browser
```

Use a distinct ID for each independently launchable app. IDs contain letters,
digits, periods, underscores, and hyphens.

## Add a non-root user

```dockerfile
USER root
RUN useradd --create-home --user-group --shell /bin/bash scientist
ENV HOME=/home/scientist USER=scientist LOGNAME=scientist
USER scientist
WORKDIR /home/scientist
```

Switch back to `USER root` before instructions that require administrative
access. The final `USER` instruction determines the default account used by the
container command unless the runtime overrides it.

## Mount working data

Mounts are container runtime configuration and do not belong in a Dockerfile:

```sh
docker run --mount \
  type=bind,source=/Users/me/data,target=/data,readonly \
  my-science-image
```

Outer Shell stores the equivalent mapping beside the Dockerfile and reconnects
it when creating the container. Moving the project to another server requires
choosing the corresponding folder on that server.
