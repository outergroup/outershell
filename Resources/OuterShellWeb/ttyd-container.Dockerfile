USER root
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl socat tmux && rm -rf /var/lib/apt/lists/*
RUN <<'TTYD_INSTALL'
set -eu
version=1.7.7
case "$(uname -m)" in
    aarch64) asset=ttyd.aarch64; checksum=b38acadd89d1d396a0f5649aa52c539edbad07f4bc7348b27b4f4b7219dd4165 ;;
    x86_64) asset=ttyd.x86_64; checksum=8a217c968aba172e0dbf3f34447218dc015bc4d5e59bf51db2f2cd12b7be4f55 ;;
    *) echo 'ttyd supports Linux on ARM64 or x86-64.' >&2; exit 1 ;;
esac
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
curl --fail --location --retry 2 --connect-timeout 20 --max-time 300 "https://github.com/tsl0922/ttyd/releases/download/$version/$asset" -o "$stage/ttyd"
actual=$(sha256sum "$stage/ttyd" | awk '{print $1}')
[ "$actual" = "$checksum" ] || { echo 'ttyd checksum verification failed.' >&2; exit 1; }
install -m 0755 "$stage/ttyd" /usr/local/bin/ttyd
ttyd --version
TTYD_INSTALL
COPY --chmod=0755 <<'TTYD_START' /usr/local/bin/outershell-ttyd
#!/bin/bash
set -eu
case "${1:-}" in
    --socket=/*) socket=${1#--socket=} ;;
    *) echo 'Usage: outershell-ttyd --socket=PATH' >&2; exit 2 ;;
esac
ttyd_socket=/run/outershell-ttyd/ttyd.sock
mkdir -p "${ttyd_socket%/*}" "${socket%/*}"
chmod 700 "${ttyd_socket%/*}"
rm -f "$ttyd_socket" "$socket"
ttyd --writable --interface "$ttyd_socket" --cwd /root --client-option disableLeaveAlert=true tmux new-session -A -s outershell &
ttyd_pid=$!
until [ -S "$ttyd_socket" ]; do
    kill -0 "$ttyd_pid" 2>/dev/null || { wait "$ttyd_pid"; exit $?; }
    sleep 0.1
done
socat UNIX-LISTEN:"$socket",fork,mode=0600 UNIX-CONNECT:"$ttyd_socket" &
proxy_pid=$!
trap 'kill "$ttyd_pid" "$proxy_pid" 2>/dev/null || true' EXIT
trap 'exit 0' INT TERM
wait -n "$ttyd_pid" "$proxy_pid"
TTYD_START
RUN outerctl image add-app --id ttyd --name ttyd --socket-argument=--socket -- /usr/local/bin/outershell-ttyd
