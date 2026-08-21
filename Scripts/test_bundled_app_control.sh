#!/bin/sh

set -eu

repo_root="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
build_dir="${TMPDIR:-/tmp}/bundled-app-control-test-$$"
api_socket="$build_dir/api.socket"
daemon_log="$build_dir/outershelld.log"
cc_command="${CC:-cc}"
cxx_command="${CXX:-c++}"
daemon_pid=""
image_source="$repo_root/Backend/OuterShellImage.c"
image_libraries="-lpng -lz"
if [ "$(uname -s)" = "Darwin" ]; then
    image_libraries="-framework ImageIO -framework CoreGraphics -framework CoreFoundation"
elif ! printf '#include <png.h>\n' | "$cc_command" -x c -fsyntax-only - >/dev/null 2>&1; then
    image_source="$repo_root/Tests/OuterShellImageStubs.c"
    image_libraries=""
fi

cleanup() {
    if [ -n "$daemon_pid" ]; then
        kill -TERM "$daemon_pid" >/dev/null 2>&1 || true
        wait "$daemon_pid" >/dev/null 2>&1 || true
    fi
    rm -rf "$build_dir"
}
trap cleanup EXIT INT TERM

mkdir -p "$build_dir/home/services"
"$cc_command" -std=gnu17 -O2 -o "$build_dir/outershelld" \
    "$repo_root/Backend/OuterShellBuffer.c" \
    "$repo_root/Backend/OuterShellAPI.c" \
    "$image_source" \
    "$repo_root/Backend/OuterShellPlatform.c" \
    "$repo_root/outershelld/OuterService.c" \
    "$repo_root/outershelld/outershelld.c" \
    -ldl -lpthread -lm $image_libraries
"$cxx_command" -std=c++17 -O2 -I "$repo_root/Resources" \
    -o "$build_dir/outerctl" "$repo_root/Resources/outerctl.cpp"

OUTERSHELL_HOME="$build_dir/home" "$build_dir/outershelld" \
    --service-manager internal \
    --services-dir "$build_dir/home/services" \
    --api-socket-path "$api_socket" \
    --stay-alive >"$daemon_log" 2>&1 &
daemon_pid=$!

attempts=100
while [ "$attempts" -gt 0 ] && [ ! -S "$api_socket" ]; do
    sleep 0.05
    attempts=$((attempts - 1))
done
if [ ! -S "$api_socket" ]; then
    printf 'outershelld did not create its test API socket\n' >&2
    if [ -f "$daemon_log" ]; then
        printf '%s\n' '--- outershelld test log ---' >&2
        cat "$daemon_log" >&2
    fi
    exit 1
fi

set +e
response="$(OUTERSHELLD_API_SOCKET="$api_socket" "$build_dir/outerctl" \
    bundled-app install \
    --backend org.outershell.Top \
    --scope user \
    --stage-root "$build_dir/missing" 2>&1)"
status=$?
set -e

[ "$status" -ne 0 ]
case "$response" in
    *"Staged Top payload is incomplete"*) ;;
    *)
        printf 'unexpected bundled-app response: %s\n' "$response" >&2
        exit 1
        ;;
esac

set +e
response="$(OUTERSHELLD_API_SOCKET="$api_socket" "$build_dir/outerctl" \
    bundled-app install \
    --backend org.outershell.Top \
    --scope system \
    --stage-root "$build_dir/missing" 2>&1)"
status=$?
set -e

[ "$status" -ne 0 ]
case "$response" in
    *"Root-supported apps are not available with this service manager"*) ;;
    *)
        printf 'unexpected internal root-install response: %s\n' "$response" >&2
        exit 1
        ;;
esac

if [ "$(uname -s)" = "Linux" ]; then
    case "$(uname -m)" in
        aarch64|arm64) architecture=aarch64 ;;
        x86_64|amd64) architecture=x86_64 ;;
        *) printf 'unsupported test architecture\n' >&2; exit 1 ;;
    esac
    stage_root="$build_dir/Top"
    mkdir -p "$stage_root/RemoteLinuxBinaries/$architecture" "$stage_root/bundles" "$stage_root/web/assets"
    printf '#!/bin/sh\nexit 0\n' > "$stage_root/RemoteLinuxBinaries/$architecture/TopBackend"
    chmod 0755 "$stage_root/RemoteLinuxBinaries/$architecture/TopBackend"
    : > "$stage_root/bundles/TopContent.bundle.macos-arm.aar"
    : > "$stage_root/bundles/TopContent.bundle.macos-x86.aar"
    : > "$stage_root/app-icon.png"
    printf 'bundled web resource\n' > "$stage_root/web/index.html"
    printf 'nested web resource\n' > "$stage_root/web/assets/example.txt"

    OUTERSHELLD_API_SOCKET="$api_socket" "$build_dir/outerctl" \
        bundled-app install \
        --backend org.outershell.Top \
        --scope user \
        --stage-root "$stage_root" >/dev/null

    cmp "$stage_root/web/index.html" "$build_dir/home/apps/org.outershell.Top/web/index.html"
    cmp "$stage_root/web/assets/example.txt" "$build_dir/home/apps/org.outershell.Top/web/assets/example.txt"

    printf 'stale web resource\n' > "$build_dir/home/apps/org.outershell.Top/web/stale.txt"
    rm "$stage_root/web/assets/example.txt"
    rmdir "$stage_root/web/assets"
    printf 'updated bundled web resource\n' > "$stage_root/web/index.html"
    OUTERSHELLD_API_SOCKET="$api_socket" "$build_dir/outerctl" \
        bundled-app install \
        --backend org.outershell.Top \
        --scope user \
        --stage-root "$stage_root" >/dev/null
    cmp "$stage_root/web/index.html" "$build_dir/home/apps/org.outershell.Top/web/index.html"
    [ ! -e "$build_dir/home/apps/org.outershell.Top/web/stale.txt" ]
    [ ! -e "$build_dir/home/apps/org.outershell.Top/web/assets/example.txt" ]

    rm -rf "$stage_root/web"
    OUTERSHELLD_API_SOCKET="$api_socket" "$build_dir/outerctl" \
        bundled-app install \
        --backend org.outershell.Top \
        --scope user \
        --stage-root "$stage_root" >/dev/null
    [ ! -e "$build_dir/home/apps/org.outershell.Top/web" ]

    mkdir -p "$stage_root/web"
    printf 'bundled web resource\n' > "$stage_root/web/index.html"
    ln -s /etc/passwd "$stage_root/web/unsupported-link"
    set +e
    response="$(OUTERSHELLD_API_SOCKET="$api_socket" "$build_dir/outerctl" \
        bundled-app install \
        --backend org.outershell.Top \
        --scope user \
        --stage-root "$stage_root" 2>&1)"
    status=$?
    set -e
    [ "$status" -ne 0 ]
    case "$response" in
        *"regular files and directories"*) ;;
        *)
            printf 'unexpected unsupported-web-resource response: %s\n' "$response" >&2
            exit 1
            ;;
    esac
fi

printf 'bundled-app control request round-trip test passed\n'
