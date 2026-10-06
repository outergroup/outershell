#!/bin/sh
set -eu
repo_root="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
build_dir=$(mktemp -d /tmp/outer-shell-scopes.XXXXXX)
trap 'rm -rf "$build_dir"' EXIT
mkdir -p "$build_dir/home" "$build_dir/bin"
for command in systemctl launchctl; do
    printf '#!/bin/sh\nexit 0\n' > "$build_dir/bin/$command"
    chmod +x "$build_dir/bin/$command"
done
image_libraries="-lpng -lz"
image_source="$repo_root/Backend/OuterShellImage.c"
if [ "$(uname -s)" = Darwin ]; then
    image_libraries="-framework ImageIO -framework CoreGraphics -framework CoreFoundation"
elif ! printf '#include <png.h>\n' | "${CC:-cc}" -x c -fsyntax-only - >/dev/null 2>&1; then
    image_source="$repo_root/Tests/OuterShellImageStubs.c"
    image_libraries=""
fi
"${CC:-cc}" -std=gnu17 -O0 -o "$build_dir/test" \
    "$repo_root/Tests/BundledAppScopesTest.c" \
    "$repo_root/Backend/OuterShellBuffer.c" "$repo_root/Backend/OuterShellAPI.c" \
    "$image_source" "$repo_root/Backend/OuterShellPlatform.c" \
    "$repo_root/outershelld/OuterService.c" \
    -ldl -lpthread -lm $image_libraries
PATH="$build_dir/bin:$PATH" "$build_dir/test" "$build_dir/home"
