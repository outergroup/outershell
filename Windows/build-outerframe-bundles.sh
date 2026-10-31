#!/bin/bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$root/build/outerframe}"
xcodebuild -project "$root/outershell.xcodeproj" -scheme 'Outer Shell' \
    -configuration Release SYMROOT="$out/products" ARCHS='arm64 x86_64' \
    ONLY_ACTIVE_ARCH=NO CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO build
bash "$root/Scripts/archive_outershell_bundle.sh" \
    "$out/products/Release/Outer Shell.bundle" "$out/bundles" OuterShell.bundle

for app in Files Plaintext Top Trace Profile; do
    repo="$root/../$app"
    name="$app"
    script="$(printf '%s' "$app" | tr '[:upper:]' '[:lower:]')"
    if [[ "$app" == Trace ]]; then name=Firehose; fi
    xcodebuild -project "$repo/$app.xcodeproj" -scheme "$app" \
        -configuration Release SYMROOT="$out/$app-products" ARCHS='arm64 x86_64' \
        ONLY_ACTIVE_ARCH=NO CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO build
    bash "$repo/Scripts/archive_${script}_bundle.sh" \
        "$out/$app-products/Release/$app.bundle" "$out/bundles" "${name}Content.bundle"
done
