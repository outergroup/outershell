#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_ROOT="${BUILD_ROOT:-${SCRIPT_DIR}/build/macos}"
RUN_ROOT="${RUN_ROOT:-${SCRIPT_DIR}/build/run}"
CONFIGURATION="${CONFIGURATION:-Release}"

require_tool() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "error: required tool '$1' was not found on PATH" >&2
        exit 1
    fi
}

require_tool /usr/bin/xcodebuild
require_tool cc
require_tool aa
require_tool lipo

rm -rf "${RUN_ROOT}"
mkdir -p "${BUILD_ROOT}" "${RUN_ROOT}/bundles"

echo "==> Building Backends.bundle"
/usr/bin/xcodebuild \
    -project "${SCRIPT_DIR}/Backends.xcodeproj" \
    -scheme Backends \
    -configuration "${CONFIGURATION}" \
    SYMROOT="${BUILD_ROOT}" \
    ARCHS="arm64 x86_64" \
    ONLY_ACTIVE_ARCH=NO \
    CODE_SIGNING_ALLOWED=NO \
    CODE_SIGNING_REQUIRED=NO \
    build

echo "==> Archiving BackendsContent bundles"
"${SCRIPT_DIR}/Scripts/archive_backends_bundle.sh" \
    "${BUILD_ROOT}/${CONFIGURATION}/Backends.bundle" \
    "${RUN_ROOT}/bundles" \
    BackendsContent.bundle

echo "==> Building NavigatorBackend"
cc -std=gnu17 -Wall -Wextra -O2 \
    -o "${BUILD_ROOT}/${CONFIGURATION}/NavigatorBackend" \
    "${SCRIPT_DIR}/Backend/main.c" \
    -lsqlite3
ln -sf NavigatorBackend "${BUILD_ROOT}/${CONFIGURATION}/BackendsBackend"

echo "Built:"
echo "  ${BUILD_ROOT}/${CONFIGURATION}/NavigatorBackend"
echo "  ${RUN_ROOT}/bundles"
echo
echo "Run:"
echo "  \"${BUILD_ROOT}/${CONFIGURATION}/NavigatorBackend\" --port 7354 --bundles-dir \"${RUN_ROOT}/bundles\""
