#!/bin/bash
# Runs ON THE SERVER, streamed over ssh by `./app uninstall`:
#
#   APP_ID=... bash -s
#
# Reverses everything remote-install.sh did. Safe to run when partially
# installed or not installed at all.

set -euo pipefail

: "${APP_ID:?APP_ID must be set}"
: "${SOCKET_FILENAME:?SOCKET_FILENAME must be set}"

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=${XDG_RUNTIME_DIR}/bus}"

APP_DIR="${HOME}/.local/share/outershell-apps/${APP_ID}"
UNIT_NAME="${APP_ID}.service"
UNIT_PATH="${HOME}/.config/systemd/user/${UNIT_NAME}"
SOCKET_PATH="${XDG_RUNTIME_DIR}/${SOCKET_FILENAME}"
LOG_DIR="${XDG_STATE_HOME:-${HOME}/.local/state}/outershell/apps/${APP_ID}"
LOG_PATH="${LOG_DIR}/backend.log"

OUTERCTL="${XDG_STATE_HOME:-${HOME}/.local/state}/outershell/bin/outerctl"
if [[ ! -x "${OUTERCTL}" ]] && command -v outerctl >/dev/null 2>&1; then
    OUTERCTL="$(command -v outerctl)"
fi

if [[ -x "${OUTERCTL}" ]]; then
    echo "==> Deregistering from Outer Shell"
    "${OUTERCTL}" app remove --backend "${APP_ID}" --frontend-id "${APP_ID}:main" || true
    "${OUTERCTL}" log remove --backend "${APP_ID}" --path "${LOG_PATH}" || true
    # Removing the backend also removes remaining app/log/opener/content-type
    # rows owned by it.
    "${OUTERCTL}" backend remove --backend "${APP_ID}" || true
else
    echo "warning: outerctl not found; skipping deregistration" >&2
fi

echo "==> Removing systemd user unit"
systemctl --user disable --now "${UNIT_NAME}" 2>/dev/null || true
rm -f "${UNIT_PATH}"
systemctl --user daemon-reload

echo "==> Removing ${APP_DIR}"
rm -rf "${APP_DIR}" "${APP_DIR}.staging" "${APP_DIR}.old"
rm -f "${LOG_PATH}"
rmdir "${LOG_DIR}" 2>/dev/null || true

echo "==> Uninstalled ${APP_ID}."
