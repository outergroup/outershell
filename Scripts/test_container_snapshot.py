#!/usr/bin/env python3
"""Verify shared snapshots, background refresh, failures, and mutation races."""
import concurrent.futures
import os
from pathlib import Path
import runpy
import struct
import subprocess
import sys
import tempfile
import time
sys.dont_write_bytecode = True
from test_layout_storage import request

provider_source = Path(__file__).resolve().parents[1] / 'Resources' / 'outershell-container-provider'
codec = runpy.run_path(str(provider_source))


def container_message(operation, arguments):
    return codec['api_message'](
        struct.pack('<HHI', 35, codec['CONTAINER_OPERATIONS'].index(operation), 0),
        ['', codec['offset_archive_encode'](arguments)],
    )

with tempfile.TemporaryDirectory(prefix='shell-snapshot-') as directory:
    root = Path(directory)
    (root / 'services').mkdir()
    (root / 'version').write_text('1')
    provider = root / 'provider'
    provider.write_text('#!' + sys.executable + '''
import os, runpy, struct, sys, time
from pathlib import Path
codec = runpy.run_path(os.environ['TEST_PROVIDER_CODEC'])
root = Path(os.environ['TEST_ROOT'])
message = sys.stdin.buffer.read()
operation = codec['CONTAINER_OPERATIONS'][struct.unpack_from('<H', message, 2)[0]]
encoded = codec['api_reference'](message, 16)
arguments = codec['offset_archive_decode'](encoded) if encoded else {}
v = (root / 'version').read_text()
with (root / 'calls').open('a') as f: f.write(operation + '\\n')
if operation == 'list':
    time.sleep(.5)
    if (root / 'fail').exists():
        sys.stdout.buffer.write(codec['container_response_message'](error='discovery failed'))
        sys.exit(0)
else:
    v = arguments['version']
    (root / 'version').write_text(v)
sys.stdout.buffer.write(codec['container_response_message'](
    snapshot={'workspaces': [{'id': v}], 'providers': [], 'padding': 'x' * 300000}))
''')
    provider.chmod(0o700)
    api = root / 'api'
    env = dict(os.environ, OUTERSHELL_HOME=str(root), TEST_ROOT=str(root), OUTER_SHELL_CONTAINER_PROVIDER=str(provider),
               TEST_PROVIDER_CODEC=str(provider_source))
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
                    c.request('POST' if route == 9 else 'GET', {9: '/api/containers', 11: '/api/layout', 13: '/api/container-snapshot'}[route], body=body)
                    response = c.getresponse()
                    result = response.status, response.read()
                    c.close()
                    return result
            def snapshot():
                status, body = request(api, 13)
                assert status == 200, body
                return codec['offset_archive_decode'](body)['workspaces'][0]['id']
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
            assert request(api, 9, container_message('rename', {'version': '4'}))[0] == 200
            wait_version('4')
            time.sleep(.6)
            assert snapshot() == '4'
            status, body = request(api, 9, codec['api_message'](struct.pack('<H', 33), ['']))
            assert status == 200 and struct.unpack_from('<HI', body) == (108, 0), (status, body[:40])
            import socket
            with socket.socket(socket.AF_UNIX) as connection:
                connection.settimeout(5)
                connection.connect(str(api))
                connection.sendall(struct.pack('<IH', 2, 36))
                def exact(count):
                    data = b''
                    while len(data) < count:
                        block = connection.recv(count - len(data))
                        assert block
                        data += block
                    return data
                reply = exact(struct.unpack('<I', exact(4))[0])
            assert struct.unpack_from('<HI', reply) == (109, 0), reply[:40]
            assert codec['offset_archive_decode'](codec['api_reference'](reply, 22))['workspaces'][0]['id'] == '4'
            print(f'PASS: shared cold load, cached reads ({elapsed * 1000:.1f} ms for ten), background updates, failure retention, mutation race')
        finally:
            if web:
                web.terminate()
                web.wait(timeout=5)
            daemon.terminate()
            daemon.wait(timeout=5)
