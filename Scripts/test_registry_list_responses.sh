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
if [ "$(uname -s)" = "Darwin" ]; then
    image_libraries="-framework ImageIO -framework CoreGraphics -framework CoreFoundation"
    service_option="--launchd-plist"
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
    "$repo_root/Backend/OuterShellImage.c" \
    "$repo_root/Backend/OuterShellPlatform.c" \
    "$repo_root/outershelld/OuterService.c" \
    "$repo_root/outershelld/outershelld.c" \
    -ldl -lpthread -lm $image_libraries
"$cxx_command" -std=c++17 -O2 -I "$repo_root/Resources" \
    -o "$build_dir/outerctl" "$repo_root/Resources/outerctl.cpp"

python3 - "$build_dir/home/registry.orwa" <<'PYFIXTURE'
from pathlib import Path
import struct
import sys

sizes = [68, 80, 32, 32, 96, 56, 32, 104]
counts = [1, 1, 1, 0, 0, 0, 0, 0]
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
ref(layout, 'legacy.app', 64)
ref(layout + 16, 'Retired user group', 64)
Path(sys.argv[1]).write_bytes(data)
PYFIXTURE

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
[ -S "$api_socket" ]

outerctl() {
    OUTERSHELLD_API_SOCKET="$api_socket" "$build_dir/outerctl" "$@"
}

outerctl app list --backend legacy.backend >"$build_dir/legacy.tsv"
awk -F '\t' '
    NR == 1 && NF != 12 { exit 1 }
    NR == 2 && NF == 12 && $1 == "legacy.app" && $7 == "8123" &&
        $10 == "/legacy" && $12 == "/icons/legacy.png" { found = 1 }
    END { exit !found }
' "$build_dir/legacy.tsv"
outerctl app upsert --backend legacy.backend --frontend-id legacy.app \
    --name "Legacy app" --host 127.0.0.1 --port 8123 --path /legacy
python3 - "$build_dir/home/registry.orwa" <<'PYCHECK'
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

container_id="54a0a9ae-71f2-4dfa-a69e-3b875ca20a0f"
outerctl container upsert \
    --container "$container_id" \
    --name Science \
    --provider docker \
    --runtime-name science-runtime \
    --project-key containers/science/recipe \
    --created-at-milliseconds 1787616000000 \
    --cpus 6 \
    --memory-gb 12 \
    --outershell-owns false
outerctl container list >"$build_dir/container.tsv"
awk -F '\t' -v id="$container_id" '
    NR == 2 && $1 == id && $2 == "Science" && $3 == "docker" &&
        $4 == "science-runtime" && $5 == "containers/science/recipe" &&
        $7 == "6" && $8 == "12" && $9 == "0" { found = 1 }
    END { exit !found }
' "$build_dir/container.tsv"
outerctl container remove --container "$container_id"
[ "$(outerctl container list | wc -l | tr -d ' ')" -eq 1 ]

printf 'multi-row registry list response round-trip test passed\n'
