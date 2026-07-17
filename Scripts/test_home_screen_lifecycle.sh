#!/bin/sh

set -eu

if [ "$(uname -s)" != "Linux" ]; then
    echo "test_home_screen_lifecycle.sh must run on Linux" >&2
    exit 1
fi

repo_root="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
build_dir="${TMPDIR:-/tmp}/home-screen-lifecycle-test-$$"
cc_command="${CC:-cc}"

cleanup() {
    rm -rf "$build_dir"
}
trap cleanup EXIT INT TERM

mkdir -p "$build_dir/bin"
cat > "$build_dir/installer.sh" <<'EOF'
#!/bin/sh
printf '%s\n' "$1" > "$OUTER_SHELL_LIFECYCLE_MARKER"
EOF
ln -s "$(command -v sh)" "$build_dir/bin/sh"
ln -s "$(command -v sleep)" "$build_dir/bin/sleep"

"$cc_command" -std=gnu17 -O2 \
    -DOUTER_SHELL_BACKEND_LIBRARY \
    -DOUTER_SHELL_LIFECYCLE_TESTING \
    -o "$build_dir/test" \
    "$repo_root/Tests/HomeScreenLifecycleTest.c" \
    "$repo_root/Backend/OuterShellBuffer.c" \
    "$repo_root/Backend/OuterShellAPI.c" \
    "$repo_root/Backend/OuterShellImage.c" \
    "$repo_root/Backend/OuterShellPlatform.c" \
    "$repo_root/outershelld/OuterService.c" \
    "$repo_root/outershelld/outershelld.c" \
    -ldl -lpthread -lm -lpng -lz

PATH="$build_dir/bin" "$build_dir/test" "$build_dir/installer.sh" "$build_dir/marker"
[ "$(cat "$build_dir/marker")" = "uninstall" ]
