#!/bin/sh
set -eu

repo_root="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
build_dir="${TMPDIR:-/tmp}/outer-shell-png-resize-test-$$"
cc_command="${CC:-cc}"
image_libraries="-lpng -lz"
if [ "$(uname -s)" = "Darwin" ]; then
    image_libraries="-framework ImageIO -framework CoreGraphics -framework CoreFoundation"
fi

cleanup() {
    rm -rf "$build_dir"
}
trap cleanup EXIT INT TERM

mkdir -p "$build_dir"
$cc_command -std=gnu17 -O2 \
    -o "$build_dir/test" \
    "$repo_root/Tests/PNGResizeTest.c" \
    "$repo_root/Backend/OuterShellImage.c" \
    $image_libraries

[ "$("$build_dir/test" "$repo_root/app-icon.png" "$build_dir/grid.png" 92)" = "92x92" ]
[ "$("$build_dir/test" "$repo_root/app-icon.png" "$build_dir/list.png" 60)" = "60x60" ]
[ -s "$build_dir/grid.png" ]
[ -s "$build_dir/list.png" ]
