#!/bin/sh
set -eu
repo_root="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
build_dir=$(mktemp -d /tmp/outer-shell-inline-startup.XXXXXX)
trap 'rm -rf "$build_dir"' EXIT
image_source="$repo_root/Backend/OuterShellImage.c"
image_libraries="-lpng -lz"
rc_name=.bashrc
if [ "$(uname -s)" = Darwin ]; then
    image_libraries="-framework ImageIO -framework CoreGraphics -framework CoreFoundation"
    rc_name=.zshrc
elif ! printf '#include <png.h>\n' | "${CC:-cc}" -x c -fsyntax-only - >/dev/null 2>&1; then
    image_source="$repo_root/Tests/OuterShellImageStubs.c"
    image_libraries=""
fi
"${CC:-cc}" -std=gnu17 -O0 -w -o "$build_dir/test" \
    "$repo_root/Tests/InlineStartupTest.c" \
    "$repo_root/Backend/OuterShellBuffer.c" "$repo_root/Backend/OuterShellAPI.c" \
    "$image_source" "$repo_root/Backend/OuterShellPlatform.c" \
    "$repo_root/outershelld/OuterService.c" \
    -ldl -lpthread -lm $image_libraries

fail() {
    printf 'FAIL: %s\n--- stdout ---\n%s\n--- stderr ---\n%s\n' "$1" "$(cat "$build_dir/out")" "$(cat "$build_dir/err")" >&2
    exit 1
}

run_case() {
    rm -rf "$build_dir/home"
    mkdir -p "$build_dir/home"
    printf '%s' "$1" > "$build_dir/home/$rc_name"
    HOME="$build_dir/home" "$build_dir/test" "$2" > "$build_dir/launcher.sh"
    chmod +x "$build_dir/launcher.sh"
    status=0
    env -u OUTERSHELL_LOG_PATH HOME="$build_dir/home" "$build_dir/launcher.sh" > "$build_dir/out" 2> "$build_dir/err" || status=$?
}

run_case 'echo rc-start
echo "${OUTERSHELL_TEST_UNSET_VARIABLE:-}" > /dev/null
echo $OUTERSHELL_TEST_UNSET_VARIABLE > /dev/null
__conda_setup="$('"'"'/nonexistent/miniforge3/bin/conda'"'"' '"'"'shell.zsh'"'"' '"'"'hook'"'"' 2> /dev/null)"
echo rc-end
' 'echo APP_STARTED'
[ "$status" = 127 ] || fail "failing startup command should stop the launcher with 127, got $status"
grep -q APP_STARTED "$build_dir/out" && fail "app should not start after a failing startup command"
grep -q rc-end "$build_dir/out" && fail "startup should stop at the failing command"
grep -q 'exited with status 127' "$build_dir/err" || fail "missing exit status in failure message"
grep -q "~/$rc_name line 4: __conda_setup=" "$build_dir/err" || fail "missing startup file location in failure message"
grep -qF "$build_dir/launcher.sh" "$build_dir/err" || fail "missing launcher path in failure message"
[ "$(grep -c '^Outer Shell:' "$build_dir/err")" = 1 ] || fail "failure should be reported exactly once"

run_case 'outershell_test_init() {
    true
    false
}
echo rc-start
outershell_test_init
' 'echo APP_STARTED'
[ "$status" = 1 ] || fail "failing startup function should stop the launcher with 1, got $status"
grep -q "~/$rc_name line 3: *false" "$build_dir/err" || fail "missing location of failure inside startup function"

run_case 'echo rc-start
echo $OUTERSHELL_TEST_UNSET_VARIABLE > /dev/null
' 'echo APP_STARTED
false
echo NOT_REACHED'
[ "$status" = 1 ] || fail "app command failure should keep set -e behavior, got $status"
grep -q APP_STARTED "$build_dir/out" || fail "app should start after clean startup files"
grep -q NOT_REACHED "$build_dir/out" && fail "set -e should still apply to the app command"
grep -q '^Outer Shell:' "$build_dir/err" && fail "app command failures should not be reported as startup failures"

run_case '' 'echo "$OUTERSHELL_TEST_UNSET_VARIABLE"'
[ "$status" != 0 ] || fail "set -u should be restored after startup files"

echo "inline startup tests passed"
