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
require_tool aa
require_tool lipo

rm -rf "${RUN_ROOT}"
mkdir -p \
    "${BUILD_ROOT}" \
    "${RUN_ROOT}/bundles"

echo "==> Building Outer Shell.bundle"
/usr/bin/xcodebuild \
    -project "${SCRIPT_DIR}/outershell.xcodeproj" \
    -scheme "Outer Shell" \
    -configuration "${CONFIGURATION}" \
    SYMROOT="${BUILD_ROOT}" \
    ARCHS="arm64 x86_64" \
    ONLY_ACTIVE_ARCH=NO \
    CODE_SIGNING_ALLOWED=NO \
    CODE_SIGNING_REQUIRED=NO \
    build

echo "==> Building outershelld"
/usr/bin/xcodebuild \
    -project "${SCRIPT_DIR}/outershell.xcodeproj" \
    -target outershelld \
    -configuration "${CONFIGURATION}" \
    SYMROOT="${BUILD_ROOT}" \
    ARCHS="arm64 x86_64" \
    ONLY_ACTIVE_ARCH=NO \
    build

echo "==> Building Outer Shell agent"
/usr/bin/xcodebuild \
    -project "${SCRIPT_DIR}/outershell.xcodeproj" \
    -target OuterShellAgent \
    -configuration "${CONFIGURATION}" \
    SYMROOT="${BUILD_ROOT}" \
    ONLY_ACTIVE_ARCH=YES \
    build

echo "==> Archiving OuterShell bundles"
"${SCRIPT_DIR}/Scripts/archive_outershell_bundle.sh" \
    "${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.bundle" \
    "${RUN_ROOT}/bundles" \
    OuterShell.bundle
rm -rf "${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app/Contents/Resources/bundles"
mkdir -p "${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app/Contents/Resources/bundles"
cp "${RUN_ROOT}/bundles"/OuterShell.bundle.*.aar \
    "${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app/Contents/Resources/bundles/"
cp "${SCRIPT_DIR}/app-icon.png" \
    "${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app/Contents/Resources/app-icon.png"
cp "${SCRIPT_DIR}/OuterShell.icns" \
    "${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app/Contents/Resources/OuterShell.icns"
rm -rf "${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app/Contents/Resources/bundled-apps"

echo "Built:"
echo "  ${BUILD_ROOT}/${CONFIGURATION}/outershelld"
echo "  ${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app"
echo "  ${RUN_ROOT}/bundles"
echo
echo "Run:"
echo "  API_SOCKET=\"$(getconf DARWIN_USER_TEMP_DIR)outershelld-api\""
echo "  \"${BUILD_ROOT}/${CONFIGURATION}/outershelld\" --api-socket-path \"\$API_SOCKET\" &"
echo "  clang -std=gnu17 -DOUTER_SHELL_BACKEND_STANDALONE=1 Backend/OuterShellBuffer.c Backend/OuterShellAPI.c Backend/OuterShellPlatform.c Backend/OuterShellDownloaderApple.m Backend/OuterShellBackend.c -framework Foundation -o /tmp/OuterShellBackend && /tmp/OuterShellBackend --port 7354 --api-socket-path \"\$API_SOCKET\" --bundles-dir \"${RUN_ROOT}/bundles\" --native-app-template-dir \"${SCRIPT_DIR}/Resources/NativeAppTemplate\""
echo "  \"${BUILD_ROOT}/${CONFIGURATION}/Outer Shell.app/Contents/MacOS/Outer Shell\" --socket-path \"$(getconf DARWIN_USER_TEMP_DIR)org.outershell.OuterShell\" --app-base-url \"https://outershell.org/outer-shell/apps\""
