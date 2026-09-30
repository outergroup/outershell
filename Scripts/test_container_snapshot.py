#!/usr/bin/env python3
"""Verify shared snapshots, background refresh, failures, and mutation races."""
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

with tempfile.TemporaryDirectory(prefix='shell-snapshot-') as directory:
    root = Path(directory)
    (root / 'services').mkdir()
    (root / 'version').write_text('1')
    provider = root / 'provider'
    provider.write_text('#!' + sys.executable + '''
import json, os, sys, time
from pathlib import Path
root = Path(os.environ['TEST_ROOT'])
r = json.load(sys.stdin)
v = (root / 'version').read_text()
with (root / 'calls').open('a') as f: f.write(r['operation'] + '\\n')
if r['operation'] == 'list':
    time.sleep(.5)
    if (root / 'fail').exists():
        print('{"error":"discovery failed"}')
        sys.exit(0)
else:
    v = r['version']
    (root / 'version').write_text(v)
print(json.dumps({'workspaces': [{'id': v}], 'providers': [], 'padding': 'x' * 300000}))
''')
    provider.chmod(0o700)
    api = root / 'api'
    env = dict(os.environ, OUTERSHELL_HOME=str(root), TEST_ROOT=str(root), OUTER_SHELL_CONTAINER_PROVIDER=str(provider))
    with (root / 'log').open('wb') as log:
        daemon = subprocess.Popen([sys.argv[1], '--api-socket-path', str(api), '--service-manager', 'internal', '--services-dir', str(root / 'services'), '--stay-alive'], env=env, stdout=log, stderr=log)
        web = None
        try:
            for _ in range(100):
                try:
                    request(api, 11)
                    break
                except (OSError, RuntimeError): time.sleep(.02)
            if len(sys.argv) > 2:
                import http.client
                import socket
                web_path = root / 'web'
                web = subprocess.Popen([sys.argv[2], '--socket-path', str(web_path), '--api-socket-path', str(api), '--web-root', str(root)], env=env, stdout=log, stderr=log)
                for _ in range(100):
                    if web_path.exists(): break
                    time.sleep(.02)
                def request(path, route, body=b''):
                    c = http.client.HTTPConnection('localhost', timeout=5)
                    c.sock = socket.socket(socket.AF_UNIX)
                    c.sock.settimeout(5)
                    c.sock.connect(str(web_path))
                    c.request('POST' if route == 9 else 'GET', {9: '/api/safe-spaces', 11: '/api/layout', 13: '/api/container-snapshot'}[route], body=body)
                    response = c.getresponse()
                    result = response.status, response.read()
                    c.close()
                    return result
            def snapshot():
                status, body = request(api, 13)
                assert status == 200, body
                return json.loads(body)['workspaces'][0]['id']
            def wait_version(version):
                deadline = time.monotonic() + 6
                while time.monotonic() < deadline:
                    if snapshot() == version: return
                    time.sleep(.05)
                raise AssertionError('Snapshot did not reach ' + version)
            with concurrent.futures.ThreadPoolExecutor() as pool:
                assert list(pool.map(lambda _: snapshot(), range(6))) == ['1'] * 6
            assert (root / 'calls').read_text().splitlines() == ['list']
            with concurrent.futures.ThreadPoolExecutor() as pool:
                assert all(pool.map(lambda i: request(api, 13 if i % 2 else 11)[0] == 200, range(20)))
            start = time.monotonic()
            for _ in range(10): assert snapshot() == '1'
            elapsed = time.monotonic() - start
            assert elapsed < .25, elapsed
            (root / 'version').write_text('2')
            wait_version('2')
            (root / 'fail').touch()
            (root / 'version').write_text('3')
            time.sleep(3.5)
            assert snapshot() == '2'
            (root / 'fail').unlink()
            wait_version('3')
            # Start a mutation while the next discovery is in flight.
            calls = len((root / 'calls').read_text().splitlines())
            deadline = time.monotonic() + 5
            while len((root / 'calls').read_text().splitlines()) == calls:
                assert time.monotonic() < deadline
                time.sleep(.02)
            assert request(api, 9, json.dumps({'operation': 'rename', 'version': '4'}).encode())[0] == 200
            wait_version('4')
            time.sleep(.6)
            assert snapshot() == '4'
            print(f'PASS: shared cold load, cached reads ({elapsed * 1000:.1f} ms for ten), background updates, failure retention, mutation race')
        finally:
            if web:
                web.terminate()
                web.wait(timeout=5)
            daemon.terminate()
            daemon.wait(timeout=5)
