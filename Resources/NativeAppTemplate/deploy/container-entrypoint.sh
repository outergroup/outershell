#!/bin/sh
set -eu

if [ -n "${OUTER_SOCKET_PATH:-}" ]; then
    exec /app/server --socket "${OUTER_SOCKET_PATH}" --root /app
fi

exec /app/server --host 0.0.0.0 --port "${PORT:-8080}" --root /app
