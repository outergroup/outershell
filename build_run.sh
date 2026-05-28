#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_ROOT="${BUILD_ROOT:-${SCRIPT_DIR}/build/macos}"
RUN_ROOT="${RUN_ROOT:-${SCRIPT_DIR}/build/run}"
CONFIGURATION="${CONFIGURATION:-Release}"
TOP_PROJECT_DIR="${TOP_PROJECT_DIR:-${SCRIPT_DIR}/../Top}"
if [[ ! -d "${TOP_PROJECT_DIR}/Top.xcodeproj" && -d "${SCRIPT_DIR}/../outerloop/Top/Top.xcodeproj" ]]; then
    TOP_PROJECT_DIR="${SCRIPT_DIR}/../outerloop/Top"
fi
TOP_BUILD_ROOT="${TOP_BUILD_ROOT:-${RUN_ROOT}/top-build/macos}"
TOP_OBJ_ROOT="${TOP_OBJ_ROOT:-${RUN_ROOT}/top-build/intermediates}"
TOP_PAYLOAD_ROOT="${RUN_ROOT}/bundled-apps/Top"
TOP_ICON_PATH="${TOP_ICON_PATH:-${TOP_PROJECT_DIR}/app-icon.png}"

require_tool() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "error: required tool '$1' was not found on PATH" >&2
        exit 1
    fi
}

require_tool /usr/bin/xcodebuild
require_tool install
require_tool aa
require_tool lipo

if [[ ! -d "${TOP_PROJECT_DIR}/Top.xcodeproj" ]]; then
    echo "error: Top.xcodeproj was not found. Set TOP_PROJECT_DIR to the Top checkout." >&2
    exit 1
fi

rm -rf "${RUN_ROOT}"
mkdir -p "${BUILD_ROOT}" "${RUN_ROOT}/bundles" "${TOP_PAYLOAD_ROOT}/MacOS" "${TOP_PAYLOAD_ROOT}/bundles"

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

echo "==> Building outershelld"
/usr/bin/xcodebuild \
    -project "${SCRIPT_DIR}/Backends.xcodeproj" \
    -target outershelld \
    -configuration "${CONFIGURATION}" \
    SYMROOT="${BUILD_ROOT}" \
    ONLY_ACTIVE_ARCH=YES \
    build

echo "==> Building Outer Shell agent"
/usr/bin/xcodebuild \
    -project "${SCRIPT_DIR}/Backends.xcodeproj" \
    -target OuterShellAgent \
    -configuration "${CONFIGURATION}" \
    SYMROOT="${BUILD_ROOT}" \
    ONLY_ACTIVE_ARCH=YES \
    build

echo "==> Building bundled TopBackend"
/usr/bin/xcodebuild \
    -project "${TOP_PROJECT_DIR}/Top.xcodeproj" \
    -scheme TopBackend \
    -configuration "${CONFIGURATION}" \
    SYMROOT="${TOP_BUILD_ROOT}" \
    OBJROOT="${TOP_OBJ_ROOT}" \
    ONLY_ACTIVE_ARCH=YES \
    build

echo "==> Building bundled Top content"
/usr/bin/xcodebuild \
    -project "${TOP_PROJECT_DIR}/Top.xcodeproj" \
    -scheme Top \
    -configuration "${CONFIGURATION}" \
    SYMROOT="${TOP_BUILD_ROOT}" \
    OBJROOT="${TOP_OBJ_ROOT}" \
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
mkdir -p "${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app/Contents/Resources/bundles"
cp "${RUN_ROOT}/bundles"/BackendsContent.bundle.*.aar \
    "${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app/Contents/Resources/bundles/"

echo "==> Staging bundled Top"
if [[ ! -f "${TOP_ICON_PATH}" ]]; then
    echo "error: Top app icon was not found at ${TOP_ICON_PATH}. Set TOP_ICON_PATH to the owning Top repo icon." >&2
    exit 1
fi
install -m 0755 \
    "${TOP_BUILD_ROOT}/${CONFIGURATION}/TopBackend" \
    "${TOP_PAYLOAD_ROOT}/MacOS/TopBackend"
install -m 0644 \
    "${TOP_ICON_PATH}" \
    "${TOP_PAYLOAD_ROOT}/app-icon.png"
"${TOP_PROJECT_DIR}/Scripts/archive_top_bundle.sh" \
    "${TOP_BUILD_ROOT}/${CONFIGURATION}/Top.bundle" \
    "${TOP_PAYLOAD_ROOT}/bundles" \
    TopContent.bundle
mkdir -p "${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app/Contents/Resources"
rm -rf "${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app/Contents/Resources/bundled-apps"
cp -R "${RUN_ROOT}/bundled-apps" \
    "${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app/Contents/Resources/bundled-apps"

ln -sf outershelld "${BUILD_ROOT}/${CONFIGURATION}/BackendsBackend"
ln -sf outershelld "${BUILD_ROOT}/${CONFIGURATION}/NavigatorBackend"

echo "Built:"
echo "  ${BUILD_ROOT}/${CONFIGURATION}/outershelld"
echo "  ${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app"
echo "  ${RUN_ROOT}/bundles"
echo "  ${TOP_PAYLOAD_ROOT}"
echo
echo "Run:"
echo "  \"${BUILD_ROOT}/${CONFIGURATION}/outershelld\" --port 7354 --bundles-dir \"${RUN_ROOT}/bundles\" --bundled-apps-dir \"${RUN_ROOT}/bundled-apps\""
echo "  \"${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app/Contents/MacOS/Outer Shell\" --socket-path \"$(getconf DARWIN_USER_TEMP_DIR)org.outershell.OuterShell\" --bundles-dir \"${RUN_ROOT}/bundles\" --bundled-apps-dir \"${RUN_ROOT}/bundled-apps\""
