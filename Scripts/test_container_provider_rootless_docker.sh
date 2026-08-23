#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(dirname "$script_dir")
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT INT TERM

mkdir -p "$temporary/bin" "$temporary/runtime"
cat > "$temporary/bin/docker" <<'EOF'
#!/bin/sh
printf '%s' "$DOCKER_HOST"
EOF
chmod 0755 "$temporary/bin/docker"

PATH="$temporary/bin:/usr/bin:/bin" \
XDG_RUNTIME_DIR="$temporary/runtime" \
OUTER_SHELL_DOCKER_HOST= \
python3 - "$repository_root/Resources/outershell-container-provider" <<'PY'
import os
import pathlib
import runpy
import socket
import sys

provider = runpy.run_path(sys.argv[1])
dictionary = provider["provider_dictionary"]()
assert not dictionary["isAvailable"]
assert "Rootless Docker is not running" in dictionary["detail"]

socket_path = pathlib.Path(os.environ["XDG_RUNTIME_DIR"]) / "docker.sock"
listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
listener.bind(str(socket_path))
try:
    dictionary = provider["provider_dictionary"]()
    assert dictionary["isAvailable"]
    assert "without host-root privileges" in dictionary["detail"]
    expected_host = "unix://" + str(socket_path)
    assert provider["docker_host"]() == expected_host
    assert provider["docker_cli_prefix"]() == [
        "docker", "--host", expected_host,
    ]
    result = provider["docker"](["version"])
    assert result.returncode == 0
    assert result.stdout == expected_host
finally:
    listener.close()
PY
