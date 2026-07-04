#!/bin/bash
# Runs ON THE SERVER, streamed over ssh by `./app deploy`:
#
#   APP_ID=... APP_NAME=... bash -s
#
# Expects the payload to have been extracted to $APP_DIR.staging already.
# Idempotent: safe to run on every deploy.

set -euo pipefail

: "${APP_ID:?APP_ID must be set}"
: "${APP_NAME:?APP_NAME must be set}"
: "${SOCKET_FILENAME:?SOCKET_FILENAME must be set}"

# Non-interactive ssh sessions don't always export these, but pam_systemd has
# created the runtime dir and user bus; point at them explicitly so
# systemctl --user and outerctl work.
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=${XDG_RUNTIME_DIR}/bus}"

APPS_ROOT="${HOME}/.local/share/outershell-apps"
APP_DIR="${APPS_ROOT}/${APP_ID}"
STAGING_DIR="${APP_DIR}.staging"
OLD_DIR="${APP_DIR}.old"
UNIT_NAME="${APP_ID}.service"
SOCKET_PATH="${XDG_RUNTIME_DIR}/${SOCKET_FILENAME}"
LOG_DIR="${XDG_STATE_HOME:-${HOME}/.local/state}/outershell/apps/${APP_ID}"
LOG_PATH="${LOG_DIR}/backend.log"

if [[ ! -d "${STAGING_DIR}" ]]; then
    echo "error: expected payload at ${STAGING_DIR}; run this via ./app deploy" >&2
    exit 1
fi

find_outerctl() {
    local candidate
    candidate="${XDG_STATE_HOME:-${HOME}/.local/state}/outershell/bin/outerctl"
    if [[ -x "${candidate}" ]]; then
        printf '%s\n' "${candidate}"
        return 0
    fi
    if command -v outerctl >/dev/null 2>&1; then
        command -v outerctl
        return 0
    fi
    return 1
}

if ! OUTERCTL="$(find_outerctl)"; then
    echo "error: outerctl not found. Install Outer Shell on this machine first:" >&2
    echo "       https://outershell.org/install/" >&2
    exit 1
fi

echo "==> Swapping in new payload at ${APP_DIR}"
rm -rf "${OLD_DIR}"
if [[ -d "${APP_DIR}" ]]; then
    mv "${APP_DIR}" "${OLD_DIR}"
fi
mv "${STAGING_DIR}" "${APP_DIR}"
rm -rf "${OLD_DIR}"

echo "==> Installing systemd user unit ${UNIT_NAME}"
mkdir -p "${HOME}/.config/systemd/user" "${LOG_DIR}"
touch "${LOG_PATH}"
cp "${APP_DIR}/deploy/${UNIT_NAME}" "${HOME}/.config/systemd/user/${UNIT_NAME}"
systemctl --user daemon-reload
systemctl --user enable "${UNIT_NAME}"
systemctl --user restart "${UNIT_NAME}"

echo "==> Registering with Outer Shell"
"${OUTERCTL}" backend upsert \
    --backend "${APP_ID}" \
    --name "${APP_NAME}" \
    --systemd-unit "${UNIT_NAME}"

"${OUTERCTL}" app upsert \
    --backend "${APP_ID}" \
    --frontend-id "${APP_ID}:main" \
    --name "${APP_NAME}" \
    --scheme http \
    --socket-path "${SOCKET_PATH}" \
    --url / \
    --icon-path "${APP_DIR}/app-icon.png"

"${OUTERCTL}" log remove --backend "${APP_ID}" --path "${LOG_PATH}" || true
"${OUTERCTL}" log add --backend "${APP_ID}" --path "${LOG_PATH}"

# If the user's systemd instance only lives as long as their ssh sessions,
# the backend stops when the last session closes. Outer Shell restarts it on
# demand, so this is usually fine; enable lingering if you want it always up.
if command -v loginctl >/dev/null 2>&1; then
    linger="$(loginctl show-user "$(id -un)" --property=Linger --value 2>/dev/null || echo unknown)"
    if [[ "${linger}" == "no" ]]; then
        echo "note: lingering is disabled; the backend runs only while you (or"
        echo "      Outer Loop) have a session. 'loginctl enable-linger' to change."
    fi
fi

echo "==> Installed. ${APP_NAME} is registered; open this server in Outer Loop."
