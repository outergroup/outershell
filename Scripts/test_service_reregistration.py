#!/usr/bin/env python3
"""Re-registering a service, as an image app's registration script does, keeps a
running service running under the new definition and leaves a stopped one stopped."""
import pathlib
import runpy
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parent
origins = runpy.run_path(str(ROOT / "test_app_origins.py"))


def control(api, service_id, operation):
    form = urllib.parse.urlencode({"serviceID": service_id, "operation": operation, "scope": "user"}).encode()
    message = bytearray(24)
    struct.pack_into("<HHI", message, 0, 26, 3, 0)
    struct.pack_into("<II", message, 8, len(message), 0)
    struct.pack_into("<II", message, 16, len(message), len(form))
    message.extend(form)
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(10)
        connection.connect(str(api))
        connection.sendall(struct.pack("<I", len(message)) + message)
        length = struct.unpack("<I", connection.recv(4, socket.MSG_WAITALL))[0]
        response = connection.recv(length, socket.MSG_WAITALL)
    assert struct.unpack_from("<H", response, 0)[0] == 107, response[:8]
    assert struct.unpack_from("<I", response, 2)[0] == 200, response


def starts(path):
    return path.read_text().split() if path.exists() else []


def wait_for_starts(path, count):
    for _ in range(200):
        if len(starts(path)) >= count:
            return starts(path)
        time.sleep(0.05)
    raise AssertionError(f"expected {count} starts, saw {starts(path)}")


def register(ctl, service_id, service):
    ctl("backend", "upsert", "--backend", service_id, "--name", service_id,
        "--service-file", str(service), "--outershell-owns", "true")


def write_service(path, log):
    path.write_text(f"""[Service]
Format=1
Name=Reregistration test
Executable=/bin/sh
Argument=-c
Argument=echo $$ >> {log}; exec sleep 60
EnvironmentPolicy=clean
Start=manual
Restart=on-failure
""")


def main():
    if sys.platform == "darwin":
        print("SKIP on macOS: the control route hands .outerservice files to launchd here; run this on Linux")
        return 0
    with tempfile.TemporaryDirectory(prefix="rereg-", dir="/tmp") as temporary:
        directory = pathlib.Path(temporary)
        origins["build"](directory)
        process, api, services, env = origins["run_daemon"](directory, "host", {})
        try:
            ctl = lambda *args: subprocess.run([str(directory / "outerctl"), *args], env=env, check=True,
                                               stdout=subprocess.DEVNULL)
            running_log, idle_log = directory / "running.log", directory / "idle.log"
            running, idle = services / "com.example.running.outerservice", services / "com.example.idle.outerservice"
            write_service(running, running_log)
            write_service(idle, idle_log)
            register(ctl, "com.example.running", running)
            register(ctl, "com.example.idle", idle)
            time.sleep(0.5)
            assert not starts(running_log) and not starts(idle_log), "manual services started on registration"

            control(api, "com.example.running", "start")
            first = wait_for_starts(running_log, 1)
            register(ctl, "com.example.running", running)
            register(ctl, "com.example.idle", idle)
            second = wait_for_starts(running_log, 2)
            assert second[0] == first[0] and second[1] != first[0], second
            time.sleep(0.5)
            assert not starts(idle_log), "re-registering a stopped service started it"
            print("PASS re-registering keeps a running service running and a stopped one stopped")
        finally:
            process.terminate()
            process.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
