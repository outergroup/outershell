#!/bin/bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$root/build/outerframe}"
xcodebuild -project "$root/outershell.xcodeproj" -scheme 'Outer Shell' \
    -configuration Release SYMROOT="$out/products" ARCHS='arm64 x86_64' \
    ONLY_ACTIVE_ARCH=NO CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO build
bash "$root/Scripts/archive_outershell_bundle.sh" \
    "$out/products/Release/Outer Shell.bundle" "$out/bundles" OuterShell.bundle
