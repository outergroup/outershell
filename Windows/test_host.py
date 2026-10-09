"""Integration tests for native Windows Outer Shell and Files (no running same-user host)."""
import concurrent.futures
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import quote

exe = Path(sys.argv[1]).resolve()
pipe = subprocess.check_output([str(exe), '--pipe'], text=True).strip()
files = pipe.replace('org.outershell.outershell.', 'org.outershell.files.')
api = pipe.replace('org.outershell.outershell.', 'org.outershell.outershelld.')
children = []

for existing in (pipe, files):
    try:
        handle = open(existing, 'r+b', buffering=0)
    except OSError as error:
        if getattr(error, 'winerror', None) not in (None, 2, 3):
            raise
    else:
        handle.close()
        raise RuntimeError('Stop the existing same-user Outer Shell/Files before running this test.')

def fail_timeout():
    for process in children:
        if process.poll() is None:
            process.kill()
    os._exit(124)
watchdog = threading.Timer(120, fail_timeout)
watchdog.daemon = True
watchdog.start()

def connect(name):
    deadline = time.monotonic() + 10
    while True:
        try:
            return open(name, 'r+b', buffering=0)
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(.03)

def raw(name, data, head=False):
    with connect(name) as stream:
        stream.write(data)
        header = bytearray()
        while not header.endswith(b'\r\n\r\n'):
            block = stream.read(1)
            assert block, bytes(header)
            header += block
            assert len(header) < 65536
        status = int(header.split(b' ')[1])
        fields = dict(line.split(b': ', 1) for line in bytes(header).split(b'\r\n')[1:-2])
        length = int(fields[b'Content-Length'])
        body = bytearray()
        if not head:
            while len(body) < length:
                block = stream.read(length - len(body))
                assert block, (length, len(body))
                body += block
        return status, bytes(body)

def request(name, route, body=b'', method='GET', extra=b''):
    message = f'{method} {route} HTTP/1.1\r\nHost: localhost\r\nContent-Length: {len(body)}\r\n'.encode() + extra + b'\r\n' + body
    return raw(name, message, method == 'HEAD')

def ref(data, offset):
    start, length = struct.unpack_from('<II', data, offset)
    assert start <= len(data) and length <= len(data) - start
    return data[start:start + length]

def registry(kind, fixed, fields):
    message = bytearray(fixed)
    struct.pack_into('<H', message, 0, kind)
    for offset, value in fields:
        struct.pack_into('<II', message, offset, len(message), len(value))
        message += value
    with connect(api) as stream:
        stream.write(struct.pack('<I', len(message)) + message)
        prefix = stream.read(4)
        assert len(prefix) == 4
        length, = struct.unpack('<I', prefix)
        reply = bytearray()
        while len(reply) < length:
            block = stream.read(length - len(reply))
            assert block
            reply += block
    assert struct.unpack_from('<HI', reply) == (100, 0), reply
    return ref(reply, 6)

def control(operation):
    status, body = request(pipe, '/api/control?serviceID=org.outershell.Files&operation=' + operation, method='POST')
    assert status == 200 and struct.unpack_from('<I', body)[0] == 1, (status, body)

with tempfile.TemporaryDirectory(prefix='outer-c-test-') as temporary:
    state = Path(temporary) / 'state'
    before_allow = Path(os.environ['LOCALAPPDATA']) / 'OuterSSH/http-pipe.allow'
    original_allow = before_allow.read_bytes() if before_allow.exists() else None
    process = subprocess.Popen([str(exe), '--state', str(state)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    children.append(process)
    try:
        status, body = request(pipe, '/api/backends')
        assert status == 200 and struct.unpack_from('<I', body, 8)[0] >= 2
        registry(29, 18, [(2, b'native.test'), (10, b'\x00binary\xff')])
        assert registry(28, 10, [(2, b'native.test')]) == b'\x00binary\xff'
        assert request(pipe, '/')[0] == 200
        native_header = b'Outerframe-Accept: application/vnd.outerframe\r\n'
        status, descriptor = request(pipe, '/', extra=native_header)
        assert status == 200 and descriptor[:4] == b'OUTR'
        version, offset, length, data_offset, data_length = struct.unpack_from('<IQQQQ', descriptor, 4)
        assert version == 1 and offset == 40 and data_offset == offset + length
        assert len(descriptor) == data_offset + data_length
        bundle = descriptor[offset:offset + length].decode()
        assert json.loads(descriptor[data_offset:])['backendsAPIPath'] == '/api/backends'
        assert json.loads(descriptor[data_offset:])['hostPlatform'] == 'windows'
        snapshot_status, snapshot = request(pipe, '/api/container-snapshot')
        assert snapshot_status == 200 and json.loads(snapshot)['hostPlatform'] == 'windows'
        assert request(pipe, '/', method='HEAD', extra=native_header) == (200, b'')
        assert request(pipe, '/backends.outer') == (200, descriptor)
        assert request(pipe, bundle) == (200, b'macos-arm\nmacos-x86\n')
        for platform in ('macos-arm', 'macos-x86'):
            expected = (exe.parent / 'bundles' / ('OuterShell.bundle.' + platform + '.aar')).read_bytes()
            assert request(pipe, bundle + '/' + platform) == (200, expected)
            assert request(pipe, bundle + '/' + platform, method='HEAD') == (200, b'')
        assert request(pipe, bundle + '/../../install.ps1')[0] == 404
        assert b'<html' in request(pipe, '/')[1].lower()
        assert request(files, '/')[0] == 200
        duplicate = subprocess.run([str(exe), '--state', str(state)], capture_output=True, timeout=10)
        assert duplicate.returncode != 0
        assert request(pipe, '/api/backends')[0] == 200
        status, layout = request(pipe, '/api/layout')
        assert status == 200 and layout.startswith(b'OSLAY001')
        document = layout[:16] + json.dumps({'version': 1, 'pins': {}, 'order': {}, 'groups': [], 'names': {'test': 'C'}}).encode()
        status, updated = request(pipe, '/api/layout', document, 'POST')
        assert status == 200 and struct.unpack_from('<Q', updated, 8)[0] == struct.unpack_from('<Q', layout, 8)[0] + 1
        assert request(pipe, '/api/layout', document, 'POST') == (409, updated)
        assert request(pipe, '/api/layout', b'OSLAY001' + bytes(8) + b'{broken}', 'POST')[0] == 400
        assert request(pipe, '/api/control?serviceID=org.outershell.Files&operation=stop', method='POST', extra=b'Origin: http://evil.invalid\r\n')[0] == 403
        assert request(pipe, '/api/control')[0] == 405
        assert raw(pipe, b'GET / HTTP/1.1\r\nHost: x\r\nHost: x\r\n\r\n')[0] == 400
        assert raw(pipe, b'POST / HTTP/1.1\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n')[0] == 400
        assert raw(pipe, b'POST / HTTP/1.1\r\nContent-Length: 16777217\r\n\r\n')[0] == 400
        assert request(files, '/web/../install.ps1')[0] == 404
        path = Path(temporary) / 'data'
        path.mkdir()
        route = '/api/mkdir?directory=' + quote(str(path)) + '&name=' + quote('Unicode-雪')
        assert request(files, route, method='POST')[0] == 200
        data = bytes(range(256)) * 1024
        upload = '/api/upload?directory=' + quote(str(path)) + '&name=roundtrip.bin'
        assert request(files, upload, data, 'POST')[0] == 200
        download = '/api/download?path=' + quote(str(path / 'roundtrip.bin'))
        assert request(files, download) == (200, data)
        assert request(files, download, method='HEAD') == (200, b'')
        chunks = f'POST {upload} HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n'.encode()
        for part in (data[:101], data[101:]):
            chunks += f'{len(part):x}\r\n'.encode() + part + b'\r\n'
        chunks += b'0\r\n\r\n'
        assert raw(files, chunks)[0] == 200
        assert request(files, download) == (200, data)
        status, listing = request(files, '/api/files?path=' + quote(str(path)))
        assert status == 200 and struct.unpack_from('<II', listing) == (0x534c4646, 2)
        count = struct.unpack_from('<I', listing, 8)[0]
        names = [ref(listing, 48 + 48*i).decode() for i in range(count)]
        assert names == ['Unicode-雪', 'roundtrip.bin'], names
        for bad in ('C:/CON', 'C:/NUL.txt', '//server/share', 'C:/file:stream'):
            assert request(files, '/api/files?path=' + quote(bad))[0] == 400
        control('stop')
        control('start')
        assert request(files, '/api/files?path=' + quote(str(path)))[0] == 200
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            replies = list(pool.map(lambda _: request(pipe, '/api/backends')[0], range(32)))
        assert replies == [200] * 32
        process.kill(); process.wait(timeout=10)
        time.sleep(.3)
        try:
            open(files, 'r+b', buffering=0).close()
        except OSError:
            pass
        else:
            raise AssertionError('Files survived its owning host')
        process = subprocess.Popen([str(exe), '--state', str(state)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        children.append(process)
        assert request(pipe, '/api/layout') == (200, updated)
        print('PASS native host: registry, web, layout CAS/persistence, framing, origin, Unicode Files, transfers, controls, concurrency, job cleanup')
    finally:
        if process.poll() is None:
            process.kill()
        process.wait(timeout=10)
        time.sleep(.2)
        if original_allow is not None:
            before_allow.write_bytes(original_allow)
        elif before_allow.exists():
            before_allow.unlink()
watchdog.cancel()
