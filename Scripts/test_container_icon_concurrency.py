#!/usr/bin/env python3
"""Force icon storage to contend with discovery that calls back into the daemon."""
import base64
import concurrent.futures
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
from test_layout_storage import exact

PNG = base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jRZkAAAAASUVORK5CYII=')


def request(api, route, body=b'', query=b''):
    message = struct.pack('<HHIIIII', 26, route, 0, 24, len(query), 24 + len(query), len(body)) + query + body
    with socket.socket(socket.AF_UNIX) as sock:
        sock.settimeout(5)
        sock.connect(str(api))
        sock.sendall(struct.pack('<I', len(message)) + message)
        reply = exact(sock, struct.unpack('<I', exact(sock, 4))[0])
    status = struct.unpack_from('<I', reply, 2)[0]
    offset, length = struct.unpack_from('<II', reply, 16)
    return status, reply[offset:offset + length]


def wait_file(path):
    deadline = time.monotonic() + 5
    while not path.exists():
        if time.monotonic() > deadline:
            raise AssertionError('Timed out waiting for ' + str(path))
        time.sleep(.01)


with tempfile.TemporaryDirectory(prefix='shell-icon-race-') as directory:
    root = Path(directory)
    (root / 'services').mkdir()
    provider = root / 'provider'
    provider.write_text('#!' + sys.executable + '''
import fcntl, json, os, socket, struct, sys, time
from pathlib import Path
root = Path(os.environ['TEST_ROOT'])
data = sys.stdin.buffer.read()
icon = data.startswith(b'OSCI')
if icon: (root / 'icon-waiting').touch()
with (root / 'records.lock').open('w') as lock:
    fcntl.flock(lock, fcntl.LOCK_EX)
    if icon:
        print((root / 'icon-status').read_text(), end='')
    else:
        (root / 'discovery-locked').touch()
        deadline = time.monotonic() + 8
        while not (root / 'icon-waiting').exists():
            if time.monotonic() > deadline: sys.exit(1)
            time.sleep(.01)
        with socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(8)
            sock.connect(os.environ['OUTERSHELLD_API_SOCKET'])
            message = struct.pack('<HHIIIII', 26, 11, 0, 24, 0, 24, 0)
            sock.sendall(struct.pack('<I', len(message)) + message)
            assert sock.recv(4)
        print(json.dumps({'workspaces': [], 'providers': []}))
''')
    provider.chmod(0o700)
    (root / 'icon-status').write_text('200')
    api = root / 'api'
    env = dict(os.environ, OUTERSHELL_HOME=str(root), TEST_ROOT=str(root), OUTER_SHELL_CONTAINER_PROVIDER=str(provider))
    with (root / 'log').open('wb') as log:
        daemon = subprocess.Popen([sys.argv[1], '--api-socket-path', str(api), '--service-manager', 'internal', '--services-dir', str(root / 'services'), '--stay-alive'], env=env, stdout=log, stderr=log, start_new_session=True)
        try:
            wait_file(api)
            with concurrent.futures.ThreadPoolExecutor() as pool:
                snapshot = pool.submit(request, api, 13)
                wait_file(root / 'discovery-locked')
                icon = pool.submit(request, api, 8, PNG, b'token=' + b'a' * 32)
                assert icon.result(timeout=6)[0] == 200
                assert snapshot.result(timeout=6)[0] == 200
            assert request(api, 11)[0] == 200
            for route in (8, 10):
                for status in (200, 400, 403, 500):
                    (root / 'icon-status').write_text(str(status))
                    code, body = request(api, route, PNG, b'token=' + b'a' * 32)
                    assert code == status, (code, body)
                    assert bool(struct.unpack_from('<I', body)[0] & 1) == (status == 200), body
                assert request(api, route, PNG)[0] == 400
                assert request(api, route, PNG, b'token=invalid')[0] == 403
                assert request(api, route, b'bad', b'token=' + b'a' * 32)[0] == 400
            print('PASS: discovery/icon lock contention, daemon callbacks, both icon routes, provider errors, invalid input')
        finally:
            os.killpg(daemon.pid, signal.SIGKILL)
            daemon.wait(timeout=5)
