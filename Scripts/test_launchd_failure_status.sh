#!/bin/sh
set -eu
repo_root="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
build_dir=$(mktemp -d /tmp/ol-launchd-test.XXXXXX)
trap 'rm -rf "$build_dir"' EXIT
cat > "$build_dir/launchctl" <<'LAUNCHCTL'
#!/bin/sh
printf '%s\n' "$*" >> "$OUTERSHELL_TEST_LAUNCHD_COMMANDS"
case "$1" in
  print) cat "$OUTERSHELL_TEST_LAUNCHD_OUTPUT" ;;
  bootout) test "${OUTERSHELL_TEST_BOOTOUT_FAILURE:-0}" != 1 ;;
  bootstrap) test "${OUTERSHELL_TEST_BOOTSTRAP_FAILURE:-0}" != 1 ;;
  *) exit 0 ;;
esac
LAUNCHCTL
export OUTERSHELL_TEST_LAUNCHD_COMMANDS="$build_dir/commands"
chmod +x "$build_dir/launchctl"
cc -std=gnu17 -O0 -o "$build_dir/test" \
  "$repo_root/Scripts/test_launchd_failure_status.c" \
  "$repo_root/Backend/OuterShellBuffer.c" "$repo_root/Backend/OuterShellAPI.c" \
  "$repo_root/Backend/OuterShellImage.c" "$repo_root/Backend/OuterShellPlatform.c" \
  "$repo_root/outershelld/OuterService.c" \
  -ldl -lpthread -lm -framework ImageIO -framework CoreGraphics -framework CoreFoundation
PATH="$build_dir:$PATH" "$build_dir/test" "$build_dir"
