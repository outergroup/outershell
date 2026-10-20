USER root
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl git bash socat unzip && rm -rf /var/lib/apt/lists/*
RUN <<'OPENCODE_INSTALL'
set -eu
version=1.19.3
app_root=/opt/outershell-opencode
config_root="${XDG_CONFIG_HOME:-$HOME/.config}/opencode"
mkdir -p "$app_root" "$config_root/skills/outer-shell"
case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) asset=opencode-darwin-arm64.zip; checksum=c2d0fecd27acf6e08be02c1f5d384614d07495587766bcc4a6bff3971749bb64 ;;
    Darwin-x86_64) asset=opencode-darwin-x64-baseline.zip; checksum=03f8c32f81a2c35429afedfb8f63978af32329febcebf800d52486f6ba568151 ;;
    Linux-aarch64) asset=opencode-linux-arm64.tar.gz; checksum=7900818a6a98881edbbadab4b7525940eff3b9750040c200481f9f56cacee405 ;;
    Linux-x86_64) asset=opencode-linux-x64-baseline.tar.gz; checksum=531cff2441129c30f0edd5878af494108ad07f5feb46d7ee29a2a5b830a5765d ;;
    *) echo "This OpenCode installer supports macOS and Linux on ARM64 or x86-64." >&2; exit 1 ;;
esac
if [ "$(uname -s)" = Linux ] && ls /lib/ld-musl-*.so.1 >/dev/null 2>&1; then
    case "$(uname -m)" in
        aarch64) asset=opencode-linux-arm64-musl.tar.gz; checksum=39c9ff1ea47fd59752613079f97bef40a2d14c30bc6bb5772a91d9d708e2d0b3 ;;
        x86_64) asset=opencode-linux-x64-baseline-musl.tar.gz; checksum=738506562666cc67e3512f04ecac50c0bae833c5bf44e1a59c99e5827a851778 ;;
    esac
fi
binary="$app_root/$version/opencode"
if [ ! -x "$binary" ]; then
    echo "Downloading OpenCode $version…"
    stage=$(mktemp -d "$app_root/download.XXXXXX")
    trap 'rm -rf "$stage"' EXIT
    curl --fail --location --retry 2 --connect-timeout 20 --max-time 600 "https://github.com/anomalyco/opencode/releases/download/v$version/$asset" -o "$stage/$asset"
    if command -v shasum >/dev/null 2>&1; then
        actual=$(shasum -a 256 "$stage/$asset" | awk '{print $1}')
    else
        actual=$(sha256sum "$stage/$asset" | awk '{print $1}')
    fi
    [ "$actual" = "$checksum" ] || { echo "OpenCode checksum verification failed." >&2; exit 1; }
    case "$asset" in
        *.zip) unzip -q "$stage/$asset" -d "$stage" ;;
        *) tar -xzf "$stage/$asset" -C "$stage" ;;
    esac
    chmod 755 "$stage/opencode"
    "$stage/opencode" --version
    mkdir -p "$app_root/$version"
    mv "$stage/opencode" "$binary"
    rm -rf "$stage"
    trap - EXIT
fi
ln -s "$binary" /usr/local/bin/opencode
OPENCODE_INSTALL
RUN mkdir -p /usr/local/share/outershell \
    && printf '%s' '__CONTAINER_AGENT__' | base64 -d | gzip -d > /usr/local/share/outershell/container-agent.md \
    && printf '%s' '__CONTAINER_OUTERCTL__' | base64 -d | gzip -d > /usr/local/share/outershell/OUTERCTL.md \
    && printf '%s' '__OUTERSHELL_SKILL__' | base64 -d | gzip -d > /usr/local/share/outershell/outer-shell-skill.md
COPY --chmod=0755 <<'OPENCODE_START' /usr/local/bin/outershell-opencode
#!/bin/sh
set -eu
export XDG_CONFIG_HOME=/var/lib/opencode/config
export XDG_DATA_HOME=/var/lib/opencode/data
export XDG_STATE_HOME=/var/lib/opencode/state
export XDG_CACHE_HOME=/var/cache/opencode
export OUTERSHELLD_API_SOCKET=/run/user/0/outershelld-api
mkdir -p "$XDG_CONFIG_HOME/opencode/skills/outer-shell" /work
cp /usr/local/share/outershell/outer-shell-skill.md "$XDG_CONFIG_HOME/opencode/skills/outer-shell/SKILL.md"
if [ ! -e /work/AGENTS.md ]; then cp /usr/local/share/outershell/container-agent.md /work/AGENTS.md; fi
export OPENCODE_CONFIG_CONTENT='__OPENCODE_CONFIG__'
workspace=/work
app_root=/var/lib/opencode
binary=/usr/local/bin/opencode
PORT=4096
cd "$workspace"
if command -v git >/dev/null 2>&1 && [ ! -e .git ] && [ "$(git rev-parse --show-toplevel 2>/dev/null || true)" != "$workspace" ]; then
    git init -q
    git -c user.name='Outer Shell' -c user.email='outershell@localhost' -c commit.gpgsign=false commit --allow-empty -qm 'Initialize container workspace'
fi
"$binary" serve --hostname 127.0.0.1 --port "$PORT" &
server_pid=$!
trap 'kill "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true' EXIT
trap 'exit 0' INT TERM
base="http://127.0.0.1:$PORT"
ready=false
for attempt in $(seq 1 120); do
    kill -0 "$server_pid" 2>/dev/null || { echo 'OpenCode exited during startup.' >&2; exit 1; }
    if curl --fail --silent --max-time 2 "$base/global/health" >/dev/null; then ready=true; break; fi
    sleep 1
 done
[ "$ready" = true ] || { echo 'OpenCode startup timed out.' >&2; exit 1; }
encoded=$(printf %s "$workspace" | od -An -tx1 | tr -s ' \n' ' ' | sed 's/^ //;s/ $//;s/ /%/g;s/^/%/')
state_file="$app_root/workspace-session"
session_id=$(cat "$state_file" 2>/dev/null || true)
case "$session_id" in ses_[a-zA-Z0-9]*) ;; *) session_id= ;; esac
if [ -z "$session_id" ] || ! curl --fail --silent --max-time 10 "$base/session/$session_id?directory=$encoded" >/dev/null; then
    result=$(curl --fail --silent --show-error --max-time 30 -H 'Content-Type: application/json' -d '{"title":"Container workspace"}' "$base/session?directory=$encoded")
    session_id=$(printf %s "$result" | sed -n 's/.*"id":"\(ses_[a-zA-Z0-9]*\)".*/\1/p')
    [ -n "$session_id" ] || { echo 'Could not create the container workspace session.' >&2; exit 1; }
    printf %s "$session_id" > "$state_file.tmp"
    mv "$state_file.tmp" "$state_file"
fi
directory_id=$(printf %s "$workspace" | base64 | tr '+/' '-_' | tr -d '=\r\n')
outerctl app upsert --backend org.outershell.image.opencode --frontend-id org.outershell.image.opencode:main --name OpenCode --socket-path /run/user/0/org.outershell.image.opencode --url "/$directory_id/session/$session_id"
echo 'OpenCode container workspace is ready.'
wait "$server_pid"
OPENCODE_START
RUN mkdir -p /work /var/lib/opencode && outerctl image add-app --id opencode --name OpenCode --working-directory /work --tcp-port 4096 -- /usr/local/bin/outershell-opencode
VOLUME ["/work", "/var/lib/opencode"]
WORKDIR /work
