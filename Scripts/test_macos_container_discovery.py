#!/usr/bin/env python3
"""Exercise macOS container discovery against an isolated agent and fake runtime.

Usage: test_macos_container_discovery.py "/path/to/Outer Shell.app/Contents/MacOS/Outer Shell"
"""
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

repo = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="shell-discovery-", dir="/tmp") as directory:
    root = Path(directory)
    api = root / "api"
    web = root / "web"
    (root / "bin").mkdir()
    (root / "home").mkdir()
    docker = root / "bin/docker"
    docker.write_text('''#!/bin/sh
if [ "$1" = info ]; then echo 1; exit 0; fi
case "$5" in
    broken)
        if [ ! -f "$TEST_ROOT/recovered" ]; then
            echo 'Cannot connect to the Docker daemon' >&2
            exit 1
        fi
        echo exited ;;
    healthy) echo exited ;;
    missing) echo 'No such container: missing' >&2; exit 1 ;;
    *) echo 'Unexpected Docker command' >&2; exit 1 ;;
esac
''')
    docker.chmod(0o700)
    apple = root / "bin/container"
    apple.write_text("#!/bin/sh\necho 'container system start' >&2\nexit 1\n")
    apple.chmod(0o700)
    cli = root / "outerctl"
    subprocess.run(["c++", "-std=c++17", "-O0", str(repo / "Resources/outerctl.cpp"),
                    "-o", str(cli)], check=True)
    env = dict(os.environ, PATH=str(root / "bin") + ":/usr/bin:/bin:/usr/sbin:/sbin",
               TEST_ROOT=str(root), OUTERSHELL_HOME=str(root / "home"),
               OUTERSHELLD_API_SOCKET=str(api))

    def request(path, body=None):
        connection = http.client.HTTPConnection("localhost", timeout=10)
        connection.sock = socket.socket(socket.AF_UNIX)
        connection.sock.settimeout(10)
        connection.sock.connect(str(web))
        try:
            connection.request("POST" if body else "GET", path,
                               body=json.dumps(body) if body else None,
                               headers={"Content-Type": "application/json"})
            response = connection.getresponse()
            return response.status, response.read()
        finally:
            connection.close()

    with (root / "agent.log").open("w+") as log:
        agent = subprocess.Popen([sys.argv[1], "--socket-path", str(web),
                                  "--api-socket-path", str(api),
                                  "--database", str(root / "home/registry.orwa"),
                                  "--system-database", str(root / "system.orwa"),
                                  "--web-root", str(repo / "Resources/OuterShellWeb"),
                                  "--container-transfers-dir", str(root / "transfers")],
                                 env=env, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 10
            while not api.exists() or not web.exists():
                if agent.poll() is not None or time.monotonic() > deadline:
                    raise AssertionError("Agent did not start")
                time.sleep(.05)
            for index, name in enumerate(["broken", "healthy", "missing"], 1):
                subprocess.run([str(cli), "container", "upsert", "--container",
                                f"00000000-0000-4000-8000-{index:012d}", "--name", name,
                                "--provider", "docker", "--runtime-name", name,
                                "--created-at-milliseconds", str(index),
                                "--outershell-owns", "false"], env=env, check=True,
                               stdout=subprocess.DEVNULL)
            status, body = request("/api/safe-spaces", {"operation": "list", "overview": True})
            result = json.loads(body)
            assert status == 200 and not result.get("error"), result
            entries = {item["name"]: item for item in result["workspaces"]}
            assert entries["broken"]["state"] == "unavailable", entries
            assert "Cannot connect to the Docker daemon" in entries["broken"]["outerShellSupport"]["detail"]
            assert entries["healthy"]["state"] == "stopped", entries
            assert entries["missing"]["state"] == "absent", entries

            def wait_snapshot(expected):
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    status, body = request("/api/container-snapshot")
                    if status == 200:
                        entries = {item["name"]: item for item in json.loads(body)["workspaces"]}
                        if entries.get("broken", {}).get("state") == expected:
                            assert len(entries) == 3, entries
                            return
                    time.sleep(.1)
                raise AssertionError((status, body))

            wait_snapshot("unavailable")
            assert request("/api/backends")[0] == 200
            (root / "recovered").touch()
            wait_snapshot("stopped")
            print("PASS: unavailable Docker preserves containers, healthy discovery, valid snapshots, app API, and recovery")
        except BaseException:
            log.seek(0)
            print(log.read(), file=sys.stderr)
            raise
        finally:
            agent.terminate()
            try:
                agent.wait(timeout=5)
            except subprocess.TimeoutExpired:
                agent.kill()
                agent.wait()
