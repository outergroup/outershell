set -eu
export PATH="$HOME/.local/bin:/opt/homebrew/bin:/usr/local/bin:$PATH"
version=1.7.7
app_root="$HOME/.local/share/outershell-ttyd"
case "$(uname -s)-$(uname -m)" in
    Darwin-*) asset= ;;
    Linux-aarch64) asset=ttyd.aarch64; checksum=b38acadd89d1d396a0f5649aa52c539edbad07f4bc7348b27b4f4b7219dd4165 ;;
    Linux-x86_64) asset=ttyd.x86_64; checksum=8a217c968aba172e0dbf3f34447218dc015bc4d5e59bf51db2f2cd12b7be4f55 ;;
    *) echo 'ttyd supports macOS and Linux on ARM64 or x86-64.' >&2; exit 1 ;;
esac
if [ -n "$asset" ]; then
    binary="$app_root/$version/ttyd"
    if [ ! -x "$binary" ]; then
        echo "Downloading ttyd ${version}…"
        mkdir -p "$app_root"
        stage=$(mktemp -d "$app_root/download.XXXXXX")
        trap 'rm -rf "$stage"' EXIT
        curl --fail --location --retry 2 --connect-timeout 20 --max-time 300 "https://github.com/tsl0922/ttyd/releases/download/$version/$asset" -o "$stage/ttyd"
        actual=$(sha256sum "$stage/ttyd" | awk '{print $1}')
        [ "$actual" = "$checksum" ] || { echo 'ttyd checksum verification failed.' >&2; exit 1; }
        chmod 755 "$stage/ttyd"
        "$stage/ttyd" --version
        mkdir -p "$(dirname "$binary")"
        mv "$stage/ttyd" "$binary"
        rm -rf "$stage"
        trap - EXIT
    fi
else
    binary=$(command -v ttyd || true)
    [ -n "$binary" ] || { echo 'ttyd is not installed. Install it with Homebrew (brew install ttyd), then start ttyd again from Outer Shell.' >&2; exit 1; }
fi
mkdir -p "$app_root"
icon="$app_root/icon.png"
printf '%s' '__TTYD_ICON__' | base64 --decode > "$icon.tmp"
mv "$icon.tmp" "$icon"
case "$(uname -s)" in
    Darwin) runtime_dir=$(getconf DARWIN_USER_TEMP_DIR) ;;
    *) runtime_dir=${XDG_RUNTIME_DIR:-/run/user/$(id -u)} ;;
esac
socket_dir="${runtime_dir%/}/outershell-ttyd"
socket_path="$socket_dir/ttyd.sock"
mkdir -p "$socket_dir"
chmod 700 "$socket_dir"
rm -f "$socket_path"
"$OUTERCTL_PATH" app upsert --backend "$BACKEND_ID" --frontend-id "$BACKEND_ID:main" --name "$DISPLAY_NAME" --socket-path "$socket_path" --url / --icon-path "$icon" ||
    { echo 'Could not register ttyd with Outer Shell.' >&2; exit 1; }
if command -v tmux >/dev/null 2>&1; then
    exec "$binary" --writable --interface "$socket_path" --cwd "$HOME" --client-option disableLeaveAlert=true tmux new-session -A -s outershell
fi
exec "$binary" --writable --interface "$socket_path" --cwd "$HOME" "${SHELL:-/bin/sh}" -l
