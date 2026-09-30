#!/usr/bin/env python3
"""Slow provider work must not block registry requests or other clients."""
import concurrent.futures
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
sys.dont_write_bytecode = True
from test_layout_storage import request

with tempfile.TemporaryDirectory(prefix='shell-provider-') as temporary:
    root = Path(temporary)
    (root / 'services').mkdir()
    api = root / 'api'
    provider = root / 'provider'
    provider.write_text('''#!''' + sys.executable + '''
import json, os, socket, struct, sys, time
value = json.load(sys.stdin)
# Make a nested registry request before delaying the discovery response.
s = socket.socket(socket.AF_UNIX)
s.connect(os.environ['OUTERSHELLD_API_SOCKET'])
m = struct.pack('<HHIIIII', 26, 11, 0, 24, 0, 24, 0)
s.sendall(struct.pack('<I', len(m)) + m)
assert s.recv(4096)
s.close()
open(os.environ['TEST_PROVIDER_STARTED'], 'w').close()
time.sleep(1)
print(json.dumps({'requestID': value['requestID'], 'workspaces': [], 'padding': 'x' * 300000}))
sys.exit(value.get('exit', 0))
''')
    provider.chmod(0o700)
    started = root / 'started'
    env = dict(os.environ, OUTERSHELL_HOME=str(root), OUTER_SHELL_CONTAINER_PROVIDER=str(provider), TEST_PROVIDER_STARTED=str(started))
    with open(root / 'log', 'wb') as log:
        daemon = subprocess.Popen([sys.argv[1], '--api-socket-path', str(api), '--service-manager', 'internal', '--services-dir', str(root / 'services'), '--stay-alive'], env=env, stdout=log, stderr=log)
        web = None
        try:
            for _ in range(100):
                try:
                    request(api, 11)
                    break
                except (OSError, RuntimeError):
                    time.sleep(.02)
            if len(sys.argv) > 2:
                import http.client
                import socket
                web_socket = root / 'web'
                web = subprocess.Popen([sys.argv[2], '--socket-path', str(web_socket), '--api-socket-path', str(api), '--web-root', str(root)], env=env, stdout=log, stderr=log)
                for _ in range(100):
                    if web_socket.exists(): break
                    time.sleep(.02)
                def request(path, route, body=b''):
                    connection = http.client.HTTPConnection('localhost', timeout=5)
                    connection.sock = socket.socket(socket.AF_UNIX)
                    connection.sock.settimeout(5)
                    connection.sock.connect(str(web_socket))
                    connection.request('POST' if route == 9 else 'GET', '/api/safe-spaces' if route == 9 else '/api/layout', body=body)
                    response = connection.getresponse()
                    result = response.status, response.read()
                    connection.close()
                    return result
            with concurrent.futures.ThreadPoolExecutor() as pool:
                futures = [pool.submit(request, api, 9, json.dumps({'requestID': str(i), 'exit': i}).encode()) for i in range(2)]
                for _ in range(200):
                    if started.exists(): break
                    time.sleep(.01)
                assert started.exists(), (root / 'log').read_text()
                assert not any(f.done() for f in futures)
                begin = time.monotonic()
                for _ in range(10): assert request(api, 11)[0] == 200
                elapsed = time.monotonic() - begin
                assert elapsed < .5, elapsed
                for i, future in enumerate(futures):
                    status, body = future.result()
                    assert status == (200 if i == 0 else 500)
                    assert json.loads(body)['requestID'] == str(i)
                    assert len(json.loads(body)['padding']) == 300000
            print(f'PASS: nested registry access, concurrent providers, large responses, error status; ten reads in {elapsed * 1000:.1f} ms during discovery')
        finally:
            if web is not None:
                web.terminate()
                web.wait(timeout=5)
            daemon.terminate()
            daemon.wait(timeout=5)
