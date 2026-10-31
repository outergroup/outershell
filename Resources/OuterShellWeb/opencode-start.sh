set -eu
mode=__OPENCODE_MODE__
export PATH="$HOME/.local/bin:/opt/homebrew/bin:/usr/local/bin:$PATH"
version=__OPENCODE_VERSION__
app_root="$HOME/.local/share/outershell-opencode"
config_root="${XDG_CONFIG_HOME:-$HOME/.config}/opencode"
mkdir -p "$app_root" "$config_root/skills/outer-shell"
case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) asset=opencode-darwin-arm64.zip; checksum=__OPENCODE_SHA256_opencode-darwin-arm64.zip__ ;;
    Darwin-x86_64) asset=opencode-darwin-x64-baseline.zip; checksum=__OPENCODE_SHA256_opencode-darwin-x64-baseline.zip__ ;;
    Linux-aarch64) asset=opencode-linux-arm64.tar.gz; checksum=__OPENCODE_SHA256_opencode-linux-arm64.tar.gz__ ;;
    Linux-x86_64) asset=opencode-linux-x64-baseline.tar.gz; checksum=__OPENCODE_SHA256_opencode-linux-x64-baseline.tar.gz__ ;;
    *) echo "This OpenCode installer supports macOS and Linux on ARM64 or x86-64." >&2; exit 1 ;;
esac
if [ "$(uname -s)" = Linux ] && ls /lib/ld-musl-*.so.1 >/dev/null 2>&1; then
    case "$(uname -m)" in
        aarch64) asset=opencode-linux-arm64-musl.tar.gz; checksum=__OPENCODE_SHA256_opencode-linux-arm64-musl.tar.gz__ ;;
        x86_64) asset=opencode-linux-x64-baseline-musl.tar.gz; checksum=__OPENCODE_SHA256_opencode-linux-x64-baseline-musl.tar.gz__ ;;
    esac
fi
binary="$app_root/$version/opencode"
if [ "$mode" = update ]; then
    binary=
    for candidate in "$app_root"/*/opencode; do
        [ -x "$candidate" ] || continue
        [ -z "$binary" ] || { echo 'Multiple OpenCode installations found; select the installation to update manually.' >&2; exit 1; }
        binary=$candidate
    done
    [ -n "$binary" ] || { echo 'No Outer Shell OpenCode installation found.' >&2; exit 1; }
fi
if [ "$mode" = update ] || [ ! -x "$binary" ]; then
    echo "Downloading OpenCode ${version}…"
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
    mkdir -p "$(dirname "$binary")"
    mv "$stage/opencode" "$binary"
    rm -rf "$stage"
    trap - EXIT
fi
if [ "$mode" = update ]; then
    echo "OpenCode $version installed. Start OpenCode from its app menu."
    exit 0
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
case "$(uname -s)" in
    Darwin) runtime_dir=$(getconf DARWIN_USER_TEMP_DIR) ;;
    *) runtime_dir=${XDG_RUNTIME_DIR:-/run/user/$(id -u)} ;;
esac
socket_dir="${runtime_dir%/}/outershell-opencode"
socket_path="$socket_dir/opencode.sock"
mkdir -p "$socket_dir"
chmod 700 "$socket_dir"
password=$(head -c 32 /dev/urandom | base64 | tr -d '/+=\n')
authorization="Basic $(printf %s "opencode:$password" | base64 | tr -d '\n')"
listen_log="$app_root/listen.log"
: > "$listen_log"
OPENCODE_SERVER_USERNAME=opencode OPENCODE_SERVER_PASSWORD="$password" "$binary" serve --hostname 127.0.0.1 --port 0 > >(tee "$listen_log") 2>&1 &
server_pid=$!
relay_pid=
trap 'kill $server_pid $relay_pid 2>/dev/null || true; wait 2>/dev/null || true' EXIT
trap 'exit 0' INT TERM
port=
for attempt in $(seq 1 120); do
    kill -0 "$server_pid" 2>/dev/null || { echo 'OpenCode exited during startup.' >&2; exit 1; }
    port=$(sed -n 's|.*listening on http://127\.0\.0\.1:\([0-9][0-9]*\).*|\1|p' "$listen_log" | head -n 1)
    [ -z "$port" ] || break
    sleep 1
done
[ -n "$port" ] || { echo 'OpenCode startup timed out.' >&2; exit 1; }
OUTERSHELL_RELAY_HEADER="Authorization: $authorization" "$OUTERCTL_PATH" relay --listen "$socket_path" --to "127.0.0.1:$port" --header-from-env OUTERSHELL_RELAY_HEADER &
relay_pid=$!
opencode_request() {
    printf 'header = "Authorization: %s"\n' "$authorization" | curl -K - "$@"
}
base="http://127.0.0.1:$port"
ready=false
for attempt in $(seq 1 120); do
    kill -0 "$server_pid" 2>/dev/null || { echo 'OpenCode exited during startup.' >&2; exit 1; }
    if opencode_request --fail --silent --max-time 2 "$base/global/health" >/dev/null; then ready=true; break; fi
    sleep 1
done
[ "$ready" = true ] || { echo 'OpenCode startup timed out.' >&2; exit 1; }
encoded=$(printf %s "$workspace" | od -An -tx1 | tr -s ' \n' ' ' | sed 's/^ //;s/ $//;s/ /%/g;s/^/%/')
state_file="$app_root/server-admin-session"
session_id=$(cat "$state_file" 2>/dev/null || true)
case "$session_id" in ses_[a-zA-Z0-9]*) ;; *) session_id= ;; esac
if [ -z "$session_id" ] || ! opencode_request --fail --silent --max-time 10 "$base/session/$session_id?directory=$encoded" >/dev/null; then
    result=$(opencode_request --fail --silent --show-error --max-time 30 -H 'Content-Type: application/json' -d '{"title":"Server administration"}' "$base/session?directory=$encoded")
    session_id=$(printf %s "$result" | sed -n 's/.*"id":"\(ses_[a-zA-Z0-9]*\)".*/\1/p')
    [ -n "$session_id" ] || { echo 'Could not create the server-admin session.' >&2; exit 1; }
    printf %s "$session_id" > "$state_file.tmp"
    mv "$state_file.tmp" "$state_file"
fi
directory_id=$(printf %s "$workspace" | base64 | tr '+/' '-_' | tr -d '=\r\n')
"$OUTERCTL_PATH" app upsert --backend "$BACKEND_ID" --frontend-id "$BACKEND_ID:main" --name "$DISPLAY_NAME" --socket-path "$socket_path" --url "/$directory_id/session/$session_id"
echo 'OpenCode server-admin workspace is ready.'
while kill -0 "$server_pid" 2>/dev/null && kill -0 "$relay_pid" 2>/dev/null; do sleep 1; done
echo 'OpenCode or its relay exited.' >&2
exit 1
