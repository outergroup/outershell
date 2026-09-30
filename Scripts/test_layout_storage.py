#!/usr/bin/env python3
"""Exercise layout persistence through an isolated outershelld API socket."""
import concurrent.futures
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time


def exact(sock, count):
    data = b''
    while len(data) < count:
        block = sock.recv(count - len(data))
        if not block:
            raise RuntimeError('Unexpected API disconnect')
        data += block
    return data


def request(path, route, body=b''):
    message = struct.pack('<HHIIIII', 26, route, 0, 24, 0, 24, len(body)) + body
    with socket.socket(socket.AF_UNIX) as sock:
        sock.settimeout(5)
        sock.connect(str(path))
        sock.sendall(struct.pack('<I', len(message)) + message)
        reply = exact(sock, struct.unpack('<I', exact(sock, 4))[0])
    assert struct.unpack_from('<H', reply)[0] == 107
    status = struct.unpack_from('<I', reply, 2)[0]
    offset, length = struct.unpack_from('<II', reply, 16)
    return status, reply[offset:offset + length]


def document(revision, value):
    return b'OSLAY001' + struct.pack('<Q', revision) + json.dumps(value).encode()


def main():
    with tempfile.TemporaryDirectory(prefix='shell-layout-') as temporary:
        root = Path(temporary)
        home = root / 'home'
        (home / 'services').mkdir(parents=True)
        api = root / 'api'
        log = open(root / 'daemon.log', 'wb')
        env = dict(os.environ, OUTERSHELL_HOME=str(home))
        def start():
            process = subprocess.Popen([sys.argv[1], '--service-manager', 'internal', '--services-dir', str(home / 'services'), '--api-socket-path', str(api), '--stay-alive'], env=env, stdout=log, stderr=log)
            for _ in range(100):
                try:
                    request(api, 11)
                    return process
                except (OSError, RuntimeError):
                    if process.poll() is not None:
                        raise RuntimeError((root / 'daemon.log').read_text())
                    time.sleep(.05)
            process.terminate()
            process.wait()
            raise RuntimeError('Daemon did not start')
        process = start()
        web_process = None
        try:
            status, empty = request(api, 11)
            assert status == 200 and empty == b'OSLAY001' + bytes(8) + b'{}'
            value = {'version': 1, 'pins': {'user': ['one']}, 'order': {'user': ['two']}, 'groups': ['container:c', 'user', 'root'], 'names': {'one': 'Notebook 🌿'}}
            status, saved = request(api, 12, document(0, value))
            assert status == 200 and struct.unpack_from('<Q', saved, 8)[0] == 1
            assert json.loads(saved[16:]) == value
            assert request(api, 12, document(0, {}))[0] == 409
            for bad in [b'', b'OSLAY001' + bytes(8) + b'{bad}', document(1, []) , b'OSLAY001' + bytes(8) + b'{"x":1,}', b'OSLAY001' + bytes(8) + b'{"x":"\\q"}', b'OSLAY001' + bytes(8) + b'{"x":' + b'[' * 40 + b'0' + b']' * 40 + b'}']:
                assert request(api, 12, bad)[0] == 400
            assert request(api, 12, document(1, {'large': 'x' * (256 * 1024)}))[0] == 400
            assert request(api, 11)[1] == saved
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                statuses = list(pool.map(lambda name: request(api, 12, document(1, dict(value, names={'one': name})))[0], ['A', 'B']))
            assert sorted(statuses) == [200, 409]
            latest = request(api, 11)[1]
            if len(sys.argv) > 2:
                import http.client
                web_socket = root / 'web'
                web_process = subprocess.Popen([sys.argv[2], '--socket-path', str(web_socket), '--api-socket-path', str(api), '--web-root', str(root)], env=env, stdout=log, stderr=log)
                for _ in range(100):
                    if web_socket.exists():
                        break
                    time.sleep(.05)
                def http_request(method, body=None):
                    connection = http.client.HTTPConnection('localhost', timeout=5)
                    connection.sock = socket.socket(socket.AF_UNIX)
                    connection.sock.settimeout(5)
                    connection.sock.connect(str(web_socket))
                    connection.request(method, '/api/layout', body=body)
                    response = connection.getresponse()
                    result = response.status, response.read()
                    connection.close()
                    return result
                assert http_request('GET') == (200, latest)
                status, updated = http_request('POST', document(2, value))
                assert status == 200 and struct.unpack_from('<Q', updated, 8)[0] == 3
                assert http_request('POST', document(2, value))[0] == 409
                latest = updated
            process.terminate(); process.wait(timeout=10)
            process = start()
            assert request(api, 11)[1] == latest
            layout = next(home.rglob('*.layout'))
            assert layout.stat().st_mode & 0o777 == 0o600
            layout.write_bytes(b'corrupted')
            assert request(api, 11)[0] == 500
            assert request(api, 12, document(0, {}))[0] == 500
            assert layout.read_bytes() == b'corrupted'
            print('PASS: read/write, revision conflicts, concurrent clients, validation, restart persistence, permissions, corruption protection')
        finally:
            if web_process:
                web_process.terminate(); web_process.wait(timeout=10)
            process.terminate(); process.wait(timeout=10)
            log.close()


if __name__ == '__main__':
    main()
