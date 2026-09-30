#!/usr/bin/env python3
"""Unchanged overview watches wait; layout/container changes wake them."""
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
sys.dont_write_bytecode = True
from test_layout_storage import exact, request, document

def events(path, versions=(0, 0, 0)):
    query = f'sinceBackends={versions[0]}&sinceLog={versions[1]}&sinceOverview={versions[2]}'.encode()
    message = struct.pack('<HHIIIII', 26, 7, 0, 24, len(query), 24 + len(query), 0) + query
    with socket.socket(socket.AF_UNIX) as sock:
        sock.settimeout(6)
        sock.connect(str(path))
        sock.sendall(struct.pack('<I', len(message)) + message)
        reply = exact(sock, struct.unpack('<I', exact(sock, 4))[0])
    offset, length = struct.unpack_from('<II', reply, 16)
    body = reply[offset:offset+length]
    assert len(body) == 32
    return struct.unpack_from('<I', body)[0], struct.unpack_from('<QQQ', body, 8)

with tempfile.TemporaryDirectory(prefix='shell-events-') as directory:
    root = Path(directory)
    (root / 'services').mkdir()
    (root / 'version').write_text('1')
    provider = root / 'provider'
    provider.write_text('#!' + sys.executable + '''
import json, os
from pathlib import Path
print(json.dumps({'workspaces':[{'id':(Path(os.environ['TEST_ROOT'])/'version').read_text()}]}))
''')
    provider.chmod(0o700)
    api = root / 'api'
    env = dict(os.environ, OUTERSHELL_HOME=str(root), TEST_ROOT=str(root), OUTER_SHELL_CONTAINER_PROVIDER=str(provider))
    with (root / 'log').open('wb') as log:
        daemon = subprocess.Popen([sys.argv[1], '--api-socket-path', str(api), '--service-manager', 'internal', '--services-dir', str(root/'services'), '--stay-alive'], env=env, stdout=log, stderr=log)
        try:
            for _ in range(100):
                try:
                    request(api, 13)
                    break
                except (OSError, RuntimeError): time.sleep(.02)
            flags, versions = events(api)
            assert flags & 8
            with concurrent.futures.ThreadPoolExecutor() as pool:
                pending = pool.submit(events, api, versions)
                time.sleep(3)
                assert not pending.done(), 'Unchanged snapshot generated a notification'
                assert request(api, 12, document(0, {'version': 1}))[0] == 200
                flags, versions = pending.result()
                assert flags & 8
                pending = pool.submit(events, api, versions)
                (root / 'version').write_text('2')
                flags, versions = pending.result()
                assert flags & 8
                assert json.loads(request(api, 13)[1])['workspaces'][0]['id'] == '2'
            print('PASS: unchanged discovery stays quiet; layout and container changes notify')
        finally:
            daemon.terminate()
            daemon.wait(timeout=5)
