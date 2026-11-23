#!/bin/sh

set -eu

repo_root="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
build_dir="${TMPDIR:-/tmp}/registry-list-response-test-$$"
api_socket="$build_dir/api.socket"
daemon_log="$build_dir/outershelld.log"
cc_command="${CC:-cc}"
cxx_command="${CXX:-c++}"
daemon_pid=""
image_libraries="-lpng -lz"
service_option="--systemd-unit"
state_dir="$build_dir/home/.local/state/outershell"
if [ "$(uname -s)" = "Darwin" ]; then
    image_libraries="-framework ImageIO -framework CoreGraphics -framework CoreFoundation"
    service_option="--launchd-plist"
    state_dir="$build_dir/home/Library/Application Support/outershell"
fi

cleanup() {
    if [ -n "$daemon_pid" ]; then
        kill -TERM "$daemon_pid" >/dev/null 2>&1 || true
        wait "$daemon_pid" >/dev/null 2>&1 || true
    fi
    rm -rf "$build_dir"
}
trap cleanup EXIT INT TERM

mkdir -p "$build_dir/home/services" "$state_dir"
"$cc_command" -std=gnu17 -O2 -o "$build_dir/outershelld" \
    "$repo_root/Backend/OuterShellBuffer.c" \
    "$repo_root/Backend/OuterShellAPI.c" \
    "$repo_root/Backend/OuterShellImage.c" \
    "$repo_root/Backend/OuterShellPlatform.c" \
    "$repo_root/outershelld/OuterService.c" \
    "$repo_root/outershelld/outershelld.c" \
    -ldl -lpthread -lm $image_libraries
"$cxx_command" -std=c++17 -O2 -I "$repo_root/Resources" \
    -o "$build_dir/outerctl" "$repo_root/Resources/outerctl.cpp"

python3 - "$state_dir/registry.orwa" <<'PYFIXTURE'
from pathlib import Path
import struct
import sys

sizes = [68, 80, 32, 32, 96, 56, 32, 104]
counts = [1, 1, 1, 0, 0, 0, 0, 1]
data = bytearray(b'ORWA' + struct.pack('<I', 1) + bytes(160))
offsets = []
for table, (size, count) in enumerate(zip(sizes, counts)):
    offsets.append(len(data))
    struct.pack_into('<QQI', data, 8 + table * 20, len(data), count, size)
    data.extend(bytes(size * count))

def ref(position, value, width=32):
    value = value.encode()
    struct.pack_into('<II' if width == 32 else '<QQ', data, position, len(data), len(value))
    data.extend(value)

backend, app, layout = offsets[:3]
ref(backend, 'legacy.backend', 64)
ref(backend + 16, 'Legacy backend', 64)
ref(app, 'legacy.app')
ref(app + 8, 'legacy.backend')
ref(app + 16, 'Legacy app')
ref(app + 24, '/icons/legacy.png')
ref(app + 32, 'Retired suggested group')
struct.pack_into('<HHHH', data, app + 40, 1, 0, 1, 0)
ref(app + 48, '/legacy')
ref(app + 56, '127.0.0.1')
struct.pack_into('<H', data, app + 64, 8123)
container = offsets[7]
ref(container, 'b1d7e6a4-0c55-4a51-9d0c-2f6f1f0e6a31', 64)
ref(container + 16, 'Legacy container', 64)
ref(container + 32, 'docker', 64)
ref(container + 48, 'legacy-runtime', 64)
ref(container + 64, 'containers/legacy/configuration', 64)
struct.pack_into('<QIII', data, container + 80, 1787616000000, 2, 3, 1)
ref(layout, 'legacy.app', 64)
ref(layout + 16, 'Retired user group', 64)
Path(sys.argv[1]).write_bytes(data)
PYFIXTURE

HOME="$build_dir/home" XDG_CONFIG_HOME="$build_dir/home/.config" XDG_STATE_HOME="$build_dir/home/.local/state" \
    OUTERSHELL_HOME="$state_dir" "$build_dir/outershelld" \
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
[ -S "$api_socket" ]

outerctl() {
    OUTERSHELLD_API_SOCKET="$api_socket" "$build_dir/outerctl" "$@"
}

python3 - "$build_dir/home" <<'PYUPGRADE'
from pathlib import Path
import sys
home = Path(sys.argv[1])
base = home / ("Library/Application Support/org.outerssh" if sys.platform == "darwin" else ".config/outerssh")
lines = (base / "endpoints.d/outershell").read_text().splitlines()
assert lines[0] == "outerssh-endpoints-v2"
assert ["tcp 127.0.0.1 8123", "", "Legacy app"] in [lines[i:i+3] for i in range(1, len(lines), 3)]
PYUPGRADE

python3 - "$build_dir/home" "$api_socket" <<'PYRENAME'
from pathlib import Path
import json
import socket
import struct
import sys

home, api = Path(sys.argv[1]), sys.argv[2]

def exact(sock, count):
    data = b''
    while len(data) < count:
        block = sock.recv(count - len(data))
        assert block, 'Unexpected API disconnect'
        data += block
    return data

def request(route, body=b''):
    message = struct.pack('<HHIIIII', 26, route, 0, 24, 0, 24, len(body)) + body
    with socket.socket(socket.AF_UNIX) as sock:
        sock.settimeout(5)
        sock.connect(api)
        sock.sendall(struct.pack('<I', len(message)) + message)
        reply = exact(sock, struct.unpack('<I', exact(sock, 4))[0])
    status = struct.unpack_from('<I', reply, 2)[0]
    offset, length = struct.unpack_from('<II', reply, 16)
    return status, reply[offset:offset + length]

def records():
    base = home / ("Library/Application Support/org.outerssh" if sys.platform == "darwin" else ".config/outerssh")
    lines = (base / "endpoints.d/outershell").read_text().splitlines()
    assert lines[0] == "outerssh-endpoints-v2"
    return [lines[i:i+3] for i in range(1, len(lines), 3)]

def save_names(names):
    status, current = request(11)
    assert status == 200, status
    body = current[:16] + json.dumps({"version": 1, "names": names}, ensure_ascii=False).encode()
    status, _ = request(12, body)
    assert status == 200, status

key = json.dumps(["host", "user", "legacy.backend", "legacy.app"], separators=(",", ":"))
save_names({key: "Renamed · app", json.dumps(["host", "user", "other", "other:main"]): "Unrelated"})
renamed = records()
for host in ("localhost", "127.0.0.1", "::1"):
    assert [f"tcp {host} 8123", "", "Renamed · app"] in renamed, renamed
assert not any(record[2] == "Legacy app" for record in renamed), renamed
save_names({})
assert ["tcp 127.0.0.1 8123", "", "Legacy app"] in records()
PYRENAME

outerctl app list --backend legacy.backend >"$build_dir/legacy.tsv"
awk -F '\t' '
    NR == 1 && NF != 12 { exit 1 }
    NR == 2 && NF == 12 && $1 == "legacy.app" && $7 == "8123" &&
        $10 == "/legacy" && $12 == "/icons/legacy.png" { found = 1 }
    END { exit !found }
' "$build_dir/legacy.tsv"
outerctl app upsert --backend legacy.backend --frontend-id legacy.app \
    --name "Legacy app" --host 127.0.0.1 --port 8123 --path /legacy
python3 - "$state_dir/registry.orwa" <<'PYCHECK'
from pathlib import Path
import struct
import sys

data = Path(sys.argv[1]).read_bytes()
app_offset, app_count, app_size = struct.unpack_from('<QQI', data, 28)
assert app_count == 1 and app_size == 80
assert data[app_offset + 32:app_offset + 40] == bytes(8)
assert struct.unpack_from('<Q', data, 56)[0] == 0
assert b'Retired suggested group' not in data
assert b'Retired user group' not in data
assert b'/icons/legacy.png' in data
PYCHECK
outerctl backend remove --backend legacy.backend

for number in 1 2 3; do
    outerctl backend upsert \
        --backend "test.backend.$number" \
        --name "Backend $number" \
        "$service_option" "$build_dir/service-$number"
    outerctl app upsert \
        --backend "test.backend.$number" \
        --frontend-id "test.app.$number" \
        --name "App $number" \
        --host 127.0.0.1 \
        --port "$((8000 + number))" \
        --path "/app-$number" \
        --icon-path "/icons/$number.png"
    outerctl log add \
        --backend "test.backend.$number" \
        --path "/tmp/test-$number.log"
    outerctl content-type add \
        --backend "test.backend.$number" \
        --content-type "com.example.test-$number" \
        --name "Test Type $number" \
        --conforms-to "public.data,public.content" \
        --extensions "t$number,tx$number" \
        --mime-types "application/x-test-$number,text/x-test-$number"
    outerctl opener upsert \
        --frontend-id "test.app.$number" \
        --content-type "com.example.test-$number" \
        --url-template "open?file={file}&type=$number" \
        --rank "$number" \
        --capabilities view,edit
done

outerctl app upsert --backend test.backend.1 --frontend-id test.app.1 --name "App 1" --socket-path /tmp/test-app-1.sock --path /app-1 --icon-path /icons/1.png
python3 - "$build_dir/home" <<'PYNAMES'
from pathlib import Path
import sys
home = Path(sys.argv[1])
base = home / ("Library/Application Support/org.outerssh" if sys.platform == "darwin" else ".config/outerssh")
lines = (base / "endpoints.d/outershell").read_text().splitlines()
assert lines.pop(0) == "outerssh-endpoints-v2"
assert len(lines) % 3 == 0
records = [lines[i:i+3] for i in range(0, len(lines), 3)]
assert not any(r[2] in ("App 1", "App 2", "App 3") for r in records), records
assert not any(r[2] == "Legacy app" for r in records)
PYNAMES
outerctl app list --backend test.backend.1 >"$build_dir/before-external.tsv"
outerctl app upsert --backend test.backend.1 --frontend-id test.app.1 \
    --external-socket-path '\\.\pipe\wslc.test'
outerctl app list --backend test.backend.1 >"$build_dir/after-external.tsv"
python3 - "$build_dir/before-external.tsv" "$build_dir/after-external.tsv" <<'PYCHECK'
import csv,sys
with open(sys.argv[1]) as f: before=list(csv.DictReader(f,delimiter="\t"))[0]
with open(sys.argv[2]) as f: after=list(csv.DictReader(f,delimiter="\t"))[0]
assert after.pop("external_socket_path").replace("\\\\", "\\") == r"\\.\pipe\wslc.test"
before.pop("external_socket_path")
assert before == after, (before, after)
PYCHECK
if outerctl app upsert --backend test.backend.2 --frontend-id test.app.1 --external-socket-path /tmp/other; then
    echo "Cross-backend endpoint update accepted" >&2; exit 1
fi
if outerctl app upsert --backend test.backend.1 --frontend-id test.app.1 --external-socket-path relative; then
    echo "Relative external endpoint accepted" >&2; exit 1
fi

assert_three_rows() {
    resource="$1"
    output="$build_dir/$resource.tsv"
    outerctl "$resource" list >"$output"
    line_count="$(wc -l <"$output" | tr -d ' ')"
    if [ "$line_count" -ne 4 ]; then
        printf '%s list returned %s data rows instead of 3\n' "$resource" "$((line_count - 1))" >&2
        exit 1
    fi
}

assert_three_rows backend
assert_three_rows app
assert_three_rows log
assert_three_rows opener

if outerctl app upsert --backend test.backend.1 --frontend-id test.app.1 --list obsolete >"$build_dir/obsolete-option.log" 2>&1; then
    echo "Removed --list option was accepted" >&2
    exit 1
fi
grep -q 'Unknown argument: --list' "$build_dir/obsolete-option.log"

outerctl app list --icons >"$build_dir/app-with-deprecated-icons.tsv"
cmp "$build_dir/app.tsv" "$build_dir/app-with-deprecated-icons.tsv"

awk -F '\t' '
    NR == 2 && $1 == "test.backend.1" && $2 == "Backend 1" { first = 1 }
    NR == 4 && $1 == "test.backend.3" && $2 == "Backend 3" { last = 1 }
    END { exit !(first && last) }
' "$build_dir/backend.tsv"

awk -F '\t' '
    NR == 2 && $1 == "test.app.1" && $3 == "App 1" && NF == 12 && $12 == "/icons/1.png" { first = 1 }
    NR == 4 && $1 == "test.app.3" && $3 == "App 3" && NF == 12 && $12 == "/icons/3.png" { last = 1 }
    END { exit !(first && last) }
' "$build_dir/app.tsv"

awk -F '\t' '
    NR == 2 && $1 == "/tmp/test-1.log" && $2 == "test.backend.1" { first = 1 }
    NR == 4 && $1 == "/tmp/test-3.log" && $2 == "test.backend.3" { last = 1 }
    END { exit !(first && last) }
' "$build_dir/log.tsv"

awk -F '\t' '
    NR == 2 && $1 == "com.example.test-1" && $2 == "test.app.1" && $4 == "1" { first = 1 }
    NR == 4 && $1 == "com.example.test-3" && $2 == "test.app.3" && $4 == "3" { last = 1 }
    END { exit !(first && last) }
' "$build_dir/opener.tsv"

outerctl content-type list >"$build_dir/content-type.tsv"
awk -F '\t' '
    $2 == "com.example.test-1" &&
        $4 == "public.data,public.content" &&
        $5 == "t1,tx1" &&
        $6 == "application/x-test-1,text/x-test-1" { first = 1 }
    $2 == "com.example.test-3" &&
        $4 == "public.data,public.content" &&
        $5 == "t3,tx3" &&
        $6 == "application/x-test-3,text/x-test-3" { last = 1 }
    END { exit !(first && last) }
' "$build_dir/content-type.tsv"

set +e
invalid_response="$(outerctl content-type list --content-type invalid/type 2>&1)"
invalid_status=$?
set -e
if [ "$invalid_status" -eq 0 ] || [ "$invalid_response" != "Invalid content type." ]; then
    printf 'invalid list request returned status %s and response: %s\n' \
        "$invalid_status" "$invalid_response" >&2
    exit 1
fi

printf '\000\001ORWV\377' >"$build_dir/resource-input.bin"
outerctl resource set --key test/binary <"$build_dir/resource-input.bin"
outerctl resource get --key test/binary >"$build_dir/resource-output.bin"
cmp "$build_dir/resource-input.bin" "$build_dir/resource-output.bin"
outerctl resource remove --key test/binary
outerctl resource get --key test/binary >"$build_dir/resource-removed.bin"
[ ! -s "$build_dir/resource-removed.bin" ]

legacy_container_id="b1d7e6a4-0c55-4a51-9d0c-2f6f1f0e6a31"
python3 - "$state_dir/registry.orwa" <<'PYROWS'
import struct, sys
data = open(sys.argv[1], 'rb').read()
assert struct.unpack_from('<I', data, 8 + 7 * 20 + 16)[0] == 120, 'container rows were not rewritten'
PYROWS
outerctl container list >"$build_dir/legacy-container.tsv"
awk -F '\t' -v id="$legacy_container_id" '
    NR == 1 && $5 == "build_context" && $8 == "self_modifying" && $10 == "memory_mb" { header = 1 }
    $1 == id && $4 == "legacy-runtime" && $5 == "" && $6 == "1" && $7 == "1" && $8 == "1" &&
        $9 == "2" && $10 == "3072" && $11 == "1787616000000" { found = 1 }
    END { exit !(header && found) }
' "$build_dir/legacy-container.tsv"
outerctl container remove --container "$legacy_container_id"

container_id="54a0a9ae-71f2-4dfa-a69e-3b875ca20a0f"
outerctl container upsert \
    --container "$container_id" \
    --name Science \
    --provider docker \
    --runtime-name science-runtime \
    --created-at-milliseconds 1787616000000 \
    --cpus 6 \
    --memory-mb 1536 \
    --outershell-owns-runtime false
outerctl container list >"$build_dir/container.tsv"
awk -F '\t' -v id="$container_id" '
    NR == 2 && $1 == id && $2 == "Science" && $3 == "docker" && $4 == "science-runtime" && $5 == "" &&
        $6 == "0" && $7 == "0" && $8 == "0" && $9 == "6" && $10 == "1536" { found = 1 }
    END { exit !found }
' "$build_dir/container.tsv"

outerctl container upsert --container "$container_id" --name "Science renamed"
outerctl container list --container "$container_id" >"$build_dir/renamed.tsv"
awk -F '\t' 'NR == 2 && $2 == "Science renamed" && $4 == "science-runtime" && $9 == "6" { found = 1 } END { exit !found }' \
    "$build_dir/renamed.tsv"

mkdir -p "$build_dir/context"
context="$(cd "$build_dir/context" && pwd -P)"
outerctl container upsert --container "$container_id" --build-context "$build_dir/context"
outerctl container list --container "$container_id" >"$build_dir/sealed.tsv"
awk -F '\t' -v path="$context" 'NR == 2 && $5 == path && $7 == "0" && $8 == "0" { found = 1 } END { exit !found }' \
    "$build_dir/sealed.tsv"
outerctl container upsert --container "$container_id" --build-context "$build_dir/context" --self-modifying \
    2>"$build_dir/warning.txt"
grep -q "will be able to modify $context" "$build_dir/warning.txt"
outerctl container list --container "$container_id" >"$build_dir/self-modifying.tsv"
awk -F '\t' -v path="$context" 'NR == 2 && $5 == path && $8 == "1" { found = 1 } END { exit !found }' \
    "$build_dir/self-modifying.tsv"

expect_refusal() {
    message="$1"
    shift
    if "$@" >"$build_dir/refusal.out" 2>&1; then
        printf 'expected refusal (%s) but the command succeeded\n' "$message" >&2
        exit 1
    fi
    grep -q "$message" "$build_dir/refusal.out" || {
        printf 'expected "%s", got:\n' "$message" >&2
        cat "$build_dir/refusal.out" >&2
        exit 1
    }
}
expect_refusal "cannot include or be inside Outer Shell" \
    outerctl container upsert --container "$container_id" --build-context "$build_dir/home" --self-modifying
expect_refusal "cannot include or be inside Outer Shell" \
    outerctl container upsert --container "$container_id" --build-context "$state_dir" --self-modifying
expect_refusal "not both" \
    outerctl container upsert --container "$container_id" --build-context "$build_dir/context" --outershell-build-context
expect_refusal "needs a provider and runtime" \
    outerctl container upsert --container 6f1c1a52-8a35-4f0e-9b43-2a9c1b7e5d10 --name Orphan
expect_refusal "must be a UUID" \
    outerctl container upsert --container not-a-uuid --name Bad --provider docker --runtime-name bad
outerctl container list --container "$container_id" >"$build_dir/after-refusals.tsv"
cmp "$build_dir/self-modifying.tsv" "$build_dir/after-refusals.tsv"

ln -s "$context" "$build_dir/context-link"
python3 - "$api_socket" "$container_id" "$build_dir/context-link" "$context" <<'PYRAW'
import socket, struct, sys
api, identifier, link, context = sys.argv[1:]

def upsert(flags, mask, path):
    refs = [identifier, "", "", "", path]
    head = struct.pack('<HHIIIIQ', 31, 0, flags, mask, 0, 0, 0)
    message = bytearray(head + bytes(8 * len(refs)))
    for index, value in enumerate(refs):
        value = value.encode()
        struct.pack_into('<II', message, len(head) + index * 8, len(message), len(value))
        message.extend(value)
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(5)
        connection.connect(api)
        connection.sendall(struct.pack('<I', len(message)) + message)
        def exact(count):
            data = b''
            while len(data) < count:
                block = connection.recv(count - len(data))
                assert block
                data += block
            return data
        reply = exact(struct.unpack('<I', exact(4))[0])
    offset, length = struct.unpack_from('<II', reply, 14)
    return struct.unpack_from('<I', reply, 2)[0], reply[offset:offset + length].decode()

status, error = upsert(0, 0x04, link)
assert status == 1 and 'canonical path' in error, (status, error)
status, error = upsert(0x04, 0x04, context)
assert status == 1 and 'Outer Shell chooses it' in error, (status, error)
status, error = upsert(0x08, 0x04, '')
assert status == 1 and 'needs a build context' in error, (status, error)
status, error = upsert(0, 0x80, '')
assert status == 1 and 'unknown fields' in error, (status, error)
PYRAW

outerctl container remove --container "$container_id"
expect_refusal "No container is registered" outerctl container remove --container "$container_id"
[ "$(outerctl container list | wc -l | tr -d ' ')" -eq 1 ]

guest_socket="$build_dir/guest.socket"
mkdir -p "$build_dir/guest/services"
HOME="$build_dir/guest" OUTERSHELL_HOME="$build_dir/guest" "$build_dir/outershelld" \
    --service-manager internal \
    --services-dir "$build_dir/guest/services" \
    --api-socket-path "$guest_socket" \
    --stay-alive --container-guest >"$build_dir/guest.log" 2>&1 &
guest_pid=$!
attempts=100
while [ "$attempts" -gt 0 ] && [ ! -S "$guest_socket" ]; do
    sleep 0.05
    attempts=$((attempts - 1))
done
guest_outerctl() {
    OUTERSHELLD_API_SOCKET="$guest_socket" "$build_dir/outerctl" "$@"
}
expect_refusal "only available on the host" \
    guest_outerctl container upsert --container "$container_id" --name Guest --provider docker --runtime-name guest
expect_refusal "only available on the host" guest_outerctl container list
kill -TERM "$guest_pid" >/dev/null 2>&1 || true
wait "$guest_pid" >/dev/null 2>&1 || true

printf 'multi-row registry list response round-trip test passed\n'
