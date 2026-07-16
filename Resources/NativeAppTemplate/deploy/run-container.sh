#!/bin/sh
set -eu

runtime_dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
data_dir="${XDG_DATA_HOME:-${HOME}/.local/share}/outershell-app-data/@APP_ID@"
mkdir -p "${data_dir}"
docker rm -f "@CONTAINER_IMAGE@" >/dev/null 2>&1 || true

exec docker run --rm \
    --name "@CONTAINER_IMAGE@" \
    --user "$(id -u):$(id -g)" \
    --read-only \
    --tmpfs /tmp \
    -e APP_DATA_DIR=/data \
    -e OUTER_SOCKET_PATH="/run/outershell/@SOCKET_FILENAME@" \
    -v "${runtime_dir}:/run/outershell" \
    -v "${data_dir}:/data" \
    "@CONTAINER_IMAGE@"
