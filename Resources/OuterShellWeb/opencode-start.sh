set -eu
export PATH="$HOME/.local/bin:/opt/homebrew/bin:/usr/local/bin:$PATH"
version=1.19.3
app_root="$HOME/.local/share/outershell-opencode"
config_root="${XDG_CONFIG_HOME:-$HOME/.config}/opencode"
mkdir -p "$app_root" "$config_root/skills/outer-shell"
if curl --silent --max-time 2 --output /dev/null "http://127.0.0.1:$PORT/"; then
    echo "Port $PORT is already in use. Choose another port in Outer Shell." >&2
    exit 1
fi
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
if [ ! -e "$config_root/skills/outer-shell/SKILL.md" ]; then
    cat > "$config_root/skills/outer-shell/SKILL.md" <<'OUTERSHELL_SKILL'
__OUTERSHELL_SKILL__
OUTERSHELL_SKILL
fi
rules="$config_root/AGENTS.md"
if ! grep -q 'Outer Shell integration' "$rules" 2>/dev/null; then
    cat >> "$rules" <<'OUTERSHELL_RULES'

## Outer Shell integration
When asked to install, register, run, or troubleshoot apps in Outer Shell, use the outer-shell skill. When using the Outer Shell launcher, you are already on the target server. Read https://outershell.org/api/ for service and launcher registration.
OUTERSHELL_RULES
fi
workspace="$HOME/server-admin"
mkdir -p "$workspace"
if [ ! -e "$workspace/AGENTS.md" ]; then
    cat > "$workspace/AGENTS.md" <<'OUTERSHELL_ADMIN'
__OUTERSHELL_ADMIN__
OUTERSHELL_ADMIN
fi
cd "$workspace"
if command -v git >/dev/null 2>&1 && [ ! -e .git ] && [ "$(git rev-parse --show-toplevel 2>/dev/null || true)" != "$workspace" ]; then
    git init -q
    git -c user.name='Outer Shell' -c user.email='outershell@localhost' -c commit.gpgsign=false commit --allow-empty -qm 'Initialize server administration workspace'
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
state_file="$app_root/server-admin-session"
session_id=$(cat "$state_file" 2>/dev/null || true)
case "$session_id" in ses_[a-zA-Z0-9]*) ;; *) session_id= ;; esac
if [ -z "$session_id" ] || ! curl --fail --silent --max-time 10 "$base/session/$session_id?directory=$encoded" >/dev/null; then
    result=$(curl --fail --silent --show-error --max-time 30 -H 'Content-Type: application/json' -d '{"title":"Server administration"}' "$base/session?directory=$encoded")
    session_id=$(printf %s "$result" | sed -n 's/.*"id":"\(ses_[a-zA-Z0-9]*\)".*/\1/p')
    [ -n "$session_id" ] || { echo 'Could not create the server-admin session.' >&2; exit 1; }
    printf %s "$session_id" > "$state_file.tmp"
    mv "$state_file.tmp" "$state_file"
fi
directory_id=$(printf %s "$workspace" | base64 | tr '+/' '-_' | tr -d '=\r\n')
"$OUTERCTL_PATH" app upsert --backend "$BACKEND_ID" --frontend-id "$BACKEND_ID:main" --name "$DISPLAY_NAME" --url "$base/$directory_id/session/$session_id"
echo 'OpenCode server-admin workspace is ready.'
wait "$server_pid"
