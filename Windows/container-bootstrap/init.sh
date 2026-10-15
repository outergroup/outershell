#!/bin/sh
set -eu
umask 022
/bin/mkdir -p /run/user/0 /root/.config/outerssh /var/lib/outershell/services \
    /var/lib/outershell/apps /etc/outershell/apps.d /root/.local/share/jupyter \
    /root/.cache
/bin/chmod 0700 /run/user/0 /root/.config/outerssh
/usr/bin/printf '%s\n' '%t/outershelld-api' \
    >/root/.config/outerssh/http-unix.allow
/bin/chmod 0600 /root/.config/outerssh/http-unix.allow
export HOME=/root USER=root LOGNAME=root XDG_RUNTIME_DIR=/run/user/0
export OUTERSHELL_HOME=/var/lib/outershell
export OUTERSHELLD_API_SOCKET=/run/user/0/outershelld-api
/usr/local/bin/outershelld --service-manager internal \
    --services-dir /var/lib/outershell/services \
    --api-socket-path /run/user/0/outershelld-api --stay-alive &
daemon_pid=$!
/usr/bin/printf '%s\n' "${daemon_pid}" >/run/user/0/outershelld.pid
cleanup() {
    /bin/kill "${daemon_pid}" 2>/dev/null || true
    wait "${daemon_pid}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM HUP
attempts=0
while [ ! -S /run/user/0/outershelld-api ] && [ "${attempts}" -lt 120 ]; do
    attempts=$((attempts + 1))
    /bin/sleep 0.1
done
for registration in /etc/outershell/apps.d/*.sh; do
    [ -f "${registration}" ] || continue
    /bin/sh "${registration}" || \
        /usr/bin/printf 'App registration failed: %s\n' "${registration}" >&2
done
wait "${daemon_pid}"
