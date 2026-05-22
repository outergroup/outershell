#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTERLOOP_RESOURCES_DIR="${OUTERLOOP_RESOURCES_DIR:-${REPO_ROOT}/../outerloop/OuterLoop/Resources}"
SQLITE_DIR="${OUTERLOOP_RESOURCES_DIR}/ThirdParty/sqlite"

case "$(uname -m)" in
    aarch64|arm64)
        ARCH="aarch64"
        ;;
    x86_64|amd64)
        ARCH="x86_64"
        ;;
    *)
        echo "error: unsupported Linux architecture: $(uname -m)" >&2
        exit 1
        ;;
esac

require_file() {
    if [[ ! -f "$1" ]]; then
        echo "error: missing $1" >&2
        exit 1
    fi
}

require_file "${SQLITE_DIR}/sqlite3.c"
require_file "${SQLITE_DIR}/sqlite3.h"

OUTPUT_DIR="${REPO_ROOT}/build/linux-package/RemoteLinuxBinaries/${ARCH}"
mkdir -p "${OUTPUT_DIR}"
cc -std=gnu17 -Os -ffunction-sections -fdata-sections -flto \
    -I"${SQLITE_DIR}" \
    -o "${OUTPUT_DIR}/HomeScreenBackend" \
    "${REPO_ROOT}/Backend/main.c" \
    "${SQLITE_DIR}/sqlite3.c" \
    -ldl -lpthread -lm

if command -v strip >/dev/null 2>&1; then
    strip --strip-unneeded "${OUTPUT_DIR}/HomeScreenBackend" || true
fi

echo "Built Home Screen Linux resource for ${ARCH}"
