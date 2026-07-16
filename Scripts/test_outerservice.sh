#!/bin/sh
set -eu

repo_root="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
build_dir="${TMPDIR:-/tmp}/outerservice-test-$$"
home_dir="$build_dir/home"
services_dir="$home_dir/services"
api_socket="$build_dir/api.socket"
http_socket="$build_dir/http.socket"
start_log="$build_dir/starts.log"
fail_once="$build_dir/fail-once"
daemon_log="$build_dir/outershelld.log"
cc_command="${CC:-cc}"
test_cflags="${OUTERSERVICE_TEST_CFLAGS:--O2}"

cleanup() {
    if [ -n "${daemon_pid:-}" ]; then
        kill -TERM "$daemon_pid" >/dev/null 2>&1 || true
        wait "$daemon_pid" >/dev/null 2>&1 || true
    fi
    rm -rf "$build_dir"
}
trap cleanup EXIT INT TERM

mkdir -p "$services_dir"
"$cc_command" -std=gnu17 $test_cflags -o "$build_dir/fixture" "$repo_root/Tests/OuterServiceFixture.c"
"$cc_command" -std=gnu17 $test_cflags -o "$build_dir/dynamic-test" \
    "$repo_root/Tests/OuterServiceDynamicTest.c" \
    "$repo_root/outershelld/OuterService.c" \
    -lpthread
"$build_dir/dynamic-test" "$build_dir/dynamic" "$build_dir/fixture"
"$cc_command" -std=gnu17 $test_cflags -o "$build_dir/outershelld" \
    "$repo_root/Backend/OuterShellBuffer.c" \
    "$repo_root/Backend/OuterShellAPI.c" \
    "$repo_root/Backend/OuterShellPlatform.c" \
    "$repo_root/outershelld/OuterService.c" \
    "$repo_root/outershelld/outershelld.c" \
    -ldl -lpthread -lm

cat > "$services_dir/test.http.outerservice" <<EOF
[Service]
Format=1
Name=Socket test
Executable=$build_dir/fixture
WorkingDirectory=$build_dir
Environment=FIXTURE_START_LOG=$start_log
Environment=FIXTURE_FAIL_ONCE=$fail_once
Start=socket
Restart=on-failure
RestartDelayMilliseconds=25
StopTimeoutMilliseconds=500
LogPath=$build_dir/fixture.log

[Socket.http]
Type=unix
Path=$http_socket
Mode=0600
Backlog=8
EOF

OUTERSHELL_HOME="$home_dir" "$build_dir/outershelld" \
    --service-manager internal \
    --services-dir "$services_dir" \
    --api-socket-path "$api_socket" \
    --stay-alive >"$daemon_log" 2>&1 &
daemon_pid=$!

attempts=100
while [ "$attempts" -gt 0 ] && { [ ! -S "$api_socket" ] || [ ! -S "$http_socket" ]; }; do
    sleep 0.05
    attempts=$((attempts - 1))
done
[ -S "$api_socket" ]
[ -S "$http_socket" ]
[ ! -e "$start_log" ]

response="$(curl --silent --show-error --max-time 5 --unix-socket "$http_socket" http://localhost/)"
[ "$response" = "outerservice works" ]
[ "$(wc -l < "$start_log" | tr -d ' ')" -eq 2 ]

kill -TERM "$daemon_pid"
wait "$daemon_pid"
daemon_pid=""
[ ! -e "$http_socket" ]

cat > "$services_dir/bad.outerservice" <<EOF
[Service]
Format=1
Executable=relative/path
EOF
if OUTERSHELL_HOME="$home_dir" "$build_dir/outershelld" \
    --service-manager internal \
    --services-dir "$services_dir" \
    --api-socket-path "$api_socket" \
    --stay-alive >"$daemon_log.bad" 2>&1; then
    echo "invalid service file was accepted" >&2
    exit 1
fi
grep -q "invalid, duplicate, or unknown key Executable" "$daemon_log.bad"

rm -f "$services_dir/bad.outerservice"
cat > "$services_dir/essential.outerservice" <<EOF
[Service]
Format=1
Executable=/bin/sh
Argument=-c
Argument=exit 7
Start=eager
Restart=never
Essential=true
EOF
set +e
OUTERSHELL_HOME="$home_dir" "$build_dir/outershelld" \
    --service-manager internal \
    --services-dir "$services_dir" \
    --api-socket-path "$api_socket" \
    --stay-alive >"$daemon_log.essential" 2>&1
essential_status=$?
set -e
[ "$essential_status" -eq 7 ]

printf 'outerservice parser, live add/remove, socket activation, restart, essential exit, and shutdown tests passed\n'
