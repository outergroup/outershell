#!/bin/sh
set -eu

repo_root="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
build_dir="${TMPDIR:-/tmp}/outer-shell-update-query-test-$$"
cc_command="${CC:-cc}"

cleanup() {
    rm -rf "$build_dir"
}
trap cleanup EXIT INT TERM

mkdir -p "$build_dir"
"$cc_command" -std=gnu17 -O2 -o "$build_dir/update-query-test" \
    "$repo_root/Tests/OuterShellPlatformTest.c" \
    "$repo_root/Backend/OuterShellBuffer.c" \
    "$repo_root/Backend/OuterShellPlatform.c"

query="$("$build_dir/update-query-test" internal)"
case "$query" in
    heartbeat=day%20bucket*'&serviceManager=internal'*) ;;
    *) echo "missing internal service manager in update query: $query" >&2; exit 1 ;;
esac
case "$query" in
    *'&appVersion=0.2.2%2Bdev'*) ;;
    *) echo "missing encoded app version in update query: $query" >&2; exit 1 ;;
esac

unknown_query="$("$build_dir/update-query-test" invalid)"
systemd_query="$("$build_dir/update-query-test" systemd)"
launchd_query="$("$build_dir/update-query-test" launchd)"
case "$systemd_query" in
    *'&serviceManager=systemd'*) ;;
    *) echo "missing systemd service manager in update query: $systemd_query" >&2; exit 1 ;;
esac
case "$launchd_query" in
    *'&serviceManager=launchd'*) ;;
    *) echo "missing launchd service manager in update query: $launchd_query" >&2; exit 1 ;;
esac
case "$(uname -s)" in
    Darwin)
        case "$query" in
            *'&os=macos&'*'&libc='*)
                echo "macOS update query unexpectedly included libc: $query" >&2
                exit 1
                ;;
            *'&os=macos&'*) ;;
            *) echo "missing macOS platform in update query: $query" >&2; exit 1 ;;
        esac
        case "$unknown_query" in
            *'&serviceManager=launchd'*) ;;
            *) echo "invalid macOS manager did not fall back to launchd: $unknown_query" >&2; exit 1 ;;
        esac
        ;;
    Linux)
        if ldd --version 2>&1 | grep -qi musl; then
            expected_libc=musl
        else
            expected_libc=glibc
        fi
        case "$query" in
            *'&os=linux&'*"&libc=$expected_libc"*) ;;
            *) echo "missing Linux libc $expected_libc in update query: $query" >&2; exit 1 ;;
        esac
        case "$unknown_query" in
            *'&serviceManager=unknown'*) ;;
            *) echo "invalid Linux manager did not fall back to unknown: $unknown_query" >&2; exit 1 ;;
        esac
        ;;
    *)
        echo "unsupported test platform: $(uname -s)" >&2
        exit 1
        ;;
esac

printf 'Outer Shell update query platform fields passed: %s\n' "$query"
