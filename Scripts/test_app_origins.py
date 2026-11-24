#!/usr/bin/env python3
"""OUTERSHELL_APP_ORIGIN(S): the encoder matches Outer Loop's, relay names match the
providers', and services started by outershelld receive the variables."""
import base64
import os
import pathlib
import platform
import runpy
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUTER_LOOP_HEADER = ROOT.parent / "Outer-Loop" / "OuterLoop" / "OuterSSH" / "include" / "OuterSSH" / "SSHProxyAddress.h"
PATHS = [
    "/private/tmp/outershell-501/container-relays/2395ecf9/3d81d72531c0d1ae.sock",
    "/run/user/1000/outershell-relays/46a57781/061dbbf5f9e8ae34.sock",
    "/a",
    "/run/user/0/org.outershell.image.openclaw",
    "/" + "x" * 139,
]

ENCODER_HARNESS = r"""
#include <stdio.h>
#include "OuterShellAppOrigin.h"
int main(int argc, char **argv) {
    char origin[512], relay[1024];
    if (argc == 4) {
        if (!outer_shell_container_relay_path(argv[2], argv[3], relay, sizeof(relay))) return 2;
        puts(relay);
        return 0;
    }
    for (int index = 1; index < argc; index++) {
        if (!outer_shell_unix_socket_origin(argv[index], false, origin, sizeof(origin))) return 1;
        puts(origin);
    }
    return 0;
}
"""

OUTER_LOOP_HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include "SSHProxyAddress.h"
int main(int argc, char **argv) {
    char host[1024];
    for (int index = 1; index < argc; index++) {
        size_t length = 0;
        if (!outerssh_proxy_encode_unix_socket_host(argv[index], strlen(argv[index]), host, sizeof(host), &length)) return 1;
        printf("http://%s\n", host);
    }
    return 0;
}
"""


def expected_origin(path):
    encoded = base64.b32encode(path.encode()).decode().rstrip("=").lower()
    return "http://" + ".".join(encoded[i:i + 50] for i in range(0, len(encoded), 50)) + ".unix.outerssh.localhost"


def build(directory):
    libraries = ["-framework", "ImageIO", "-framework", "CoreGraphics", "-framework", "CoreFoundation"] \
        if platform.system() == "Darwin" else ["-lpng", "-lz"]
    subprocess.run(["cc", "-std=gnu17", "-O1", "-w", "-o", str(directory / "outershelld"),
                    *(str(ROOT / name) for name in ("Backend/OuterShellBuffer.c", "Backend/OuterShellAPI.c",
                                                    "Backend/OuterShellImage.c", "Backend/OuterShellPlatform.c",
                                                    "outershelld/OuterService.c", "outershelld/outershelld.c")),
                    "-ldl", "-lpthread", "-lm", *libraries], check=True)
    subprocess.run(["c++", "-std=c++17", "-O1", "-I", str(ROOT / "Resources"), "-o", str(directory / "outerctl"),
                    str(ROOT / "Resources" / "outerctl.cpp")], check=True)
    (directory / "encoder.c").write_text(ENCODER_HARNESS)
    subprocess.run(["cc", "-std=gnu17", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "Backend"),
                    "-o", str(directory / "encoder"), str(directory / "encoder.c")], check=True)


def check_encoder(directory):
    ours = subprocess.run([str(directory / "encoder"), *PATHS], capture_output=True, text=True, check=True).stdout.split()
    assert ours == [expected_origin(path) for path in PATHS], ours
    assert subprocess.run([str(directory / "encoder"), "/" + "x" * 140]).returncode != 0
    assert ours[0].startswith("http://f5yhe2lwmf2gkl3unvyc633vorsxe43imvwgy")
    if OUTER_LOOP_HEADER.exists():
        (directory / "outer-loop.c").write_text(OUTER_LOOP_HARNESS)
        subprocess.run(["cc", "-std=gnu17", "-w", "-I", str(OUTER_LOOP_HEADER.parent),
                        "-I", str(OUTER_LOOP_HEADER.parent.parent), "-o", str(directory / "outer-loop"),
                        str(directory / "outer-loop.c")], check=True)
        theirs = subprocess.run([str(directory / "outer-loop"), *PATHS], capture_output=True, text=True,
                                check=True).stdout.split()
        assert theirs == ours, (theirs, ours)
        print("PASS encoder matches Outer Loop's SSHProxyAddress.h")
    else:
        print("SKIP Outer Loop comparison: SSHProxyAddress.h not found next to this repository")
    provider = runpy.run_path(str(ROOT / "Resources" / "outershell-container-provider"))
    globals_ = provider["relay_path"].__globals__
    original = globals_["relay_directory"]
    try:
        globals_["relay_directory"] = lambda identifier: pathlib.Path("/run/user/1000/outershell-relays/46a57781")
        for inner in ("/run/user/0/org.outershell.image.openclaw", "/run/user/0/outershelld-api"):
            python = str(provider["relay_path"]("46a57781", inner))
            ours = subprocess.run([str(directory / "encoder"), "relay", "/run/user/1000/outershell-relays/46a57781", inner],
                                  capture_output=True, text=True, check=True).stdout.strip()
            assert python == ours, (python, ours)
    finally:
        globals_["relay_directory"] = original
    print("PASS encoder, length limit, and relay names shared with the container providers")


def wait_for(path):
    for _ in range(200):
        if path.exists() and path.read_text():
            return dict(line.split("=", 1) for line in path.read_text().splitlines() if "=" in line)
        time.sleep(0.05)
    raise AssertionError(f"{path} was not written")


def run_daemon(directory, name, environment):
    home = directory / name
    services = home / "services"
    services.mkdir(parents=True)
    api = directory / f"{name}.api"
    env = dict(os.environ, OUTERSHELL_HOME=str(home), HOME=str(home), **environment)
    log = open(directory / f"{name}.log", "w")
    process = subprocess.Popen([str(directory / "outershelld"), "--service-manager", "internal",
                                "--services-dir", str(services), "--api-socket-path", str(api), "--stay-alive",
                                "--database", str(home / "registry.orwa")],
                               env=env, stdout=log, stderr=log)
    for _ in range(200):
        if api.exists():
            break
        time.sleep(0.05)
    return process, api, services, dict(env, OUTERSHELLD_API_SOCKET=str(api))


def service_file(path, output, start):
    path.write_text(f"""[Service]
Format=1
Name=Origin test
Executable=/bin/sh
Argument=-c
Argument=env > {output}; exec sleep 60
EnvironmentPolicy=clean
Start={start}
Restart=never
""")


def check_container_service(directory):
    relay_directory = "/private/tmp/outershell-501/container-relays/2395ecf9"
    process, api, services, env = run_daemon(directory, "guest", {"OUTERSHELL_RELAY_DIRECTORY": relay_directory})
    try:
        ctl = lambda *args: subprocess.run([str(directory / "outerctl"), *args], env=env, check=True,
                                           stdout=subprocess.DEVNULL)
        output = directory / "guest.env"
        service = services / "org.outershell.image.openclaw.outerservice"
        service_file(service, output, "eager")
        ctl("backend", "upsert", "--backend", "org.outershell.image.openclaw", "--name", "OpenClaw")
        ctl("app", "upsert", "--backend", "org.outershell.image.openclaw", "--frontend-id",
            "org.outershell.image.openclaw:main", "--socket-path", "/run/user/0/org.outershell.image.openclaw",
            "--name", "OpenClaw", "--url", "/")
        ctl("backend", "upsert", "--backend", "org.outershell.image.openclaw", "--name", "OpenClaw",
            "--service-file", str(service), "--outershell-owns", "true")
        values = wait_for(output)
        expected = expected_origin(relay_directory + "/3d81d72531c0d1ae.sock")
        assert values.get("OUTERSHELL_APP_ORIGIN") == expected, values
        assert values.get("OUTERSHELL_APP_ORIGINS") == expected, values
        print("PASS a container app started at boot gets the origin of its host relay socket")
    finally:
        process.terminate()
        process.wait(timeout=10)


def check_host_service(directory):
    process, api, services, env = run_daemon(directory, "host", {})
    try:
        ctl = lambda *args: subprocess.run([str(directory / "outerctl"), *args], env=env, check=True,
                                           stdout=subprocess.DEVNULL)
        output = directory / "host.env"
        service = services / "com.example.two.outerservice"
        service_file(service, output, "eager")
        ctl("backend", "upsert", "--backend", "com.example.two", "--name", "Two endpoints")
        for name in ("first", "second"):
            ctl("app", "upsert", "--backend", "com.example.two", "--frontend-id", f"com.example.two:{name}",
                "--socket-path", f"/tmp/example-{name}.sock", "--name", name, "--url", "/")
        ctl("backend", "upsert", "--backend", "com.example.two", "--name", "Two endpoints",
            "--service-file", str(service), "--outershell-owns", "true")
        values = wait_for(output)
        assert values.get("OUTERSHELL_APP_ORIGINS", "").split() == [
            expected_origin("/tmp/example-first.sock"), expected_origin("/tmp/example-second.sock")], values
        assert "OUTERSHELL_APP_ORIGIN" not in values, values
        print("PASS a host app with two endpoints gets both origins and no single origin")
    finally:
        process.terminate()
        process.wait(timeout=10)


def main():
    with tempfile.TemporaryDirectory(prefix="origins-", dir="/tmp") as temporary:
        directory = pathlib.Path(temporary)
        build(directory)
        check_encoder(directory)
        check_container_service(directory)
        check_host_service(directory)
    print("PASS app origins")


if __name__ == "__main__":
    sys.exit(main())
