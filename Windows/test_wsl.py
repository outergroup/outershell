"""Exercise a configured WSL host over the same SSH/named-pipe bridge as Outer Loop.

Usage: python3 test_wsl.py SSH_ALIAS WINDOWS_BRIDGE_EXE DISTRIBUTION
The selected distribution must have the standard Linux Outer Shell, Files, Top, and Plaintext installed. This test
stops and restarts that distribution, and restores any allowlist edits it makes.
"""
import base64
import json
import os
import selectors
import struct
import subprocess
import sys
import time
import uuid
from urllib.parse import urlencode

host, bridge_exe, distribution = sys.argv[1:]

def powershell(script):
    command = 'powershell.exe -NoProfile -EncodedCommand ' + base64.b64encode(script.encode('utf-16le')).decode()
    return ['ssh', '-T', '-o', 'ConnectTimeout=10', host, command]

def literal(text):
    return "'" + text.replace("'", "''") + "'"

def linux(code, root=False):
    encoded = base64.b64encode(code.encode()).decode()
    argument = f"import base64;exec(base64.b64decode('{encoded}'))"
    user = " --user root" if root else ""
    command = f'wsl --distribution {literal(distribution)}{user} --exec python3 -c {literal(argument)}; exit $LASTEXITCODE'
    return subprocess.run(powershell(command), capture_output=True, timeout=45, check=True).stdout

sid = subprocess.check_output(powershell('[Security.Principal.WindowsIdentity]::GetCurrent().User.Value'), timeout=20).decode().strip().lower()
shell_pipe = '\\\\.\\pipe\\org.outershell.outershell.' + sid

class Bridge:
    def __init__(self, pipe):
        command = f'& {literal(bridge_exe)} bridge --socket {literal(pipe)}; exit $LASTEXITCODE'
        self.process = subprocess.Popen(powershell(command), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        self.buffer = bytearray()
        self.stream = 0
        assert self.frame()[0] == 1
    def read(self, length):
        deadline = time.monotonic() + 45
        while len(self.buffer) < length:
            ready = self.selector.select(max(0, deadline - time.monotonic()))
            if not ready: raise TimeoutError('Bridge read timed out')
            block = os.read(self.process.stdout.fileno(), 65536)
            if not block:
                self.process.kill()
                _, error = self.process.communicate(timeout=5)
                raise RuntimeError('Bridge closed: ' + error.decode(errors='replace'))
            self.buffer += block
        result = bytes(self.buffer[:length]); del self.buffer[:length]; return result
    def frame(self):
        magic, version, kind, stream, length = struct.unpack('<IHHII', self.read(16))
        assert magic == 0x3142524f and version == 1 and length <= 1024 * 1024
        return kind, stream, self.read(length)
    def send(self, kind, body=b''):
        self.process.stdin.write(struct.pack('<IHHII', 0x3142524f, 1, kind, self.stream, len(body)) + body)
        self.process.stdin.flush()
    def request(self, route, body=b'', method='GET', native=False):
        self.stream += 1
        self.send(2)
        while True:
            kind, stream, data = self.frame()
            if stream == self.stream: break
        assert kind == 3, (kind, data)
        accept = 'Outerframe-Accept: application/vnd.outerframe\r\n' if native else ''
        request = f'{method} {route} HTTP/1.1\r\nHost: localhost\r\n{accept}Accept: application/vnd.outerframe, text/html\r\nConnection: close\r\nContent-Length: {len(body)}\r\n\r\n'.encode() + body
        for offset in range(0, len(request), 32768): self.send(5, request[offset:offset + 32768])
        self.send(6)
        result = bytearray()
        while True:
            kind, stream, data = self.frame()
            if stream != self.stream: continue
            if kind == 5: result += data
            elif kind in (6, 7): break
        self.send(7)
        if not result: return 0, b''
        header, body = bytes(result).split(b'\r\n\r\n', 1)
        status = int(header.split(b' ')[1])
        fields = {k.lower(): v.strip() for k, v in (line.split(b':', 1) for line in header.split(b'\r\n')[1:])}
        if method != 'HEAD' and b'content-length' in fields: assert len(body) == int(fields[b'content-length'])
        return status, body
    def close(self):
        self.process.stdin.close()
        try: self.process.wait(timeout=10)
        except subprocess.TimeoutExpired: self.process.kill(); self.process.wait()
        self.selector.close()
    def __enter__(self): return self
    def __exit__(self, *args): self.close()

def ref(data, offset):
    start, length = struct.unpack_from('<II', data, offset)
    assert start <= len(data) and length <= len(data) - start
    return data[start:start + length]

def expect(client, path, body=b'', method='GET'):
    status, result = client.request(path, body, method)
    assert status == 200, (status, result[:300])
    return result

def check_outerframe(client, shell=True):
    status, descriptor = client.request('/', native=True)
    assert status == 200 and descriptor[:4] == b'OUTR'
    version, url_offset, url_length, json_offset, json_length = struct.unpack_from('<IQQQQ', descriptor, 4)
    assert version == 1 and url_offset == 40 and json_offset == url_offset + url_length
    assert len(descriptor) == json_offset + json_length
    if shell: assert json.loads(descriptor[json_offset:])['backendsAPIPath'] == '/api/backends'
    url = descriptor[url_offset:json_offset].decode()
    assert client.request('/', method='HEAD', native=True) == (200, b'')
    assert expect(client, url) == b'macos-arm\nmacos-x86\n'
    for platform in ('macos-arm', 'macos-x86'):
        assert len(expect(client, url + '/' + platform)) > 1000


with Bridge(shell_pipe) as host_client:
    check_outerframe(host_client)
    def snapshot(): return json.loads(expect(host_client, '/api/container-snapshot'))
    def workspace(): return next((w for w in snapshot()['workspaces'] if w['name'] == distribution), None)
    def wait_for(predicate):
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            item = workspace()
            if item and predicate(item): return item
            time.sleep(1)
        raise AssertionError('WSL state did not converge: ' + repr({k: v for k, v in (item or {}).items() if k != 'apps'}))
    workspace_id = wait_for(lambda w: True)['id']
    def operation(name, **values):
        request_id = str(uuid.uuid4()).upper()
        result = json.loads(expect(host_client, '/api/safe-spaces', json.dumps(dict(requestID=request_id, operation=name, workspaceID=workspace_id.upper(), **values)).encode(), 'POST'))
        assert result['requestID'] == request_id
        return result
    operation('start')
    print('Requested distribution start; waiting for Linux app discovery', flush=True)
    item = wait_for(lambda w: w['state'] == 'running' and len(w['apps']) >= 2)
    for field in ('cpus', 'memoryInGB', 'shellCommand'): assert field in item
    for app in item['apps']:
        for field in ('iconPath', 'iconObservationToken', 'publishedPort', 'externalSocketPath'): assert field in app
    apps = {a['serviceID']: a for a in item['apps']}
    files = apps['org.outershell.Files']
    operation('startApp', serviceID='org.outershell.Files')
    wait_for(lambda w: any(a['serviceID'] == 'org.outershell.Files' and a['isRunning'] for a in w['apps']))
    print('PASS discovery, distinct WSL identity, and app start control', flush=True)
    api = '/api/wsl/' + workspace_id
    catalog = expect(host_client, api + '/backends?web=1')
    scopes = set()
    for i in range(struct.unpack_from('<I', catalog, 8)[0]):
        backend = ref(catalog, 12 + i * 8)
        scopes.add(ref(backend, 32).decode())
    assert {'user', 'system'} <= scopes
    assert len(expect(host_client, api + '/recipes')) > 100
    assert host_client.request(api + '/control')[0] == 405
    assert host_client.request(api + '/logs')[0] == 404
    status, result = host_client.request(api + '/control', urlencode(dict(serviceID='outer-test-missing', scope='user', operation='start')).encode(), 'POST')
    assert status == 404 and struct.unpack_from('<I', result)[0] & 1 == 0
    print('PASS flattened Linux catalogs, recipes, POST body and error forwarding', flush=True)

    with Bridge(apps['org.outershell.OuterShell']['externalSocketPath']) as guest_shell:
        check_outerframe(guest_shell)
        assert b'<!doctype html' in expect(guest_shell, '/').lower()
        data = expect(guest_shell, '/api/backends?web=1')
        mapped = []
        for i in range(struct.unpack_from('<I', data, 8)[0]):
            backend = ref(data, 12 + i * 8); frontends = ref(backend, 68)
            for j in range(struct.unpack_from('<I', frontends)[0]):
                app = ref(frontends, 4 + j * 8)
                mapped.append(ref(app, 16).decode())
        assert files['externalSocketPath'] in mapped, mapped
    print('PASS nested Linux homepage links map back to WSL pipes', flush=True)
    for name in ('Files', 'Top', 'Plaintext'):
        with Bridge(apps['org.outershell.' + name]['externalSocketPath']) as client:
            check_outerframe(client, shell=False)
    print('PASS standard Files, Top, and Plaintext native bundles', flush=True)
    root_apps = [app for app in item['apps'] if app['userName'] == 'root']
    if root_apps:
        root_app = root_apps[0]
        with Bridge(root_app['externalSocketPath']) as client:
            check_outerframe(client, shell=False)
        allow = linux("from pathlib import Path; import base64; print(base64.b64encode(Path('/etc/outerssh/http-unix.allow').read_bytes()).decode())", root=True).strip()
        try:
            linux("from pathlib import Path; p=Path('/etc/outerssh/http-unix.allow'); p.write_text(''.join(line for line in p.read_text().splitlines(True) if line.strip().replace('%%T', '/run')!=%r))" % root_app['socketPath'], root=True)
            with Bridge(root_app['externalSocketPath']) as client:
                assert client.request('/', native=True)[0] == 0
        finally:
            linux("from pathlib import Path; import base64; Path('/etc/outerssh/http-unix.allow').write_bytes(base64.b64decode(%r))" % allow.decode(), root=True)
        with Bridge(root_app['externalSocketPath']) as client:
            check_outerframe(client, shell=False)
        print('PASS root app transport and root allowlist revocation/restoration', flush=True)
        siblings = [app for app in item['apps'] if app['serviceID'] == root_app['serviceID']]
        if {app.get('serviceScope') for app in siblings} == {'user', 'system'}:
            assert len({app['userName'] for app in siblings}) == 2
            assert len({app['externalSocketPath'] for app in siblings}) == 2
            for app in siblings:
                with Bridge(app['externalSocketPath']) as client:
                    check_outerframe(client, shell=False)
            for app in siblings:
                scope = app['serviceScope']
                unit = app['serviceID'] + '.service'
                def active(system):
                    return linux("import subprocess; print(subprocess.run(['systemctl',%s'is-active',%r],capture_output=True,text=True).stdout.strip())" % ("" if system else "'--user',", unit), root=system).strip()
                try:
                    operation('stopApp', serviceID=app['serviceID'], serviceScope=scope)
                    assert active(scope == 'system') == b'inactive'
                    wait_for(lambda w: any(a['serviceID'] == app['serviceID'] and a.get('serviceScope') == scope and not a['isRunning'] for a in w['apps']))
                    assert active(scope != 'system') == b'active'
                finally:
                    operation('startApp', serviceID=app['serviceID'], serviceScope=scope)
                    with Bridge(app['externalSocketPath']) as client:
                        check_outerframe(client, shell=False)
            print('PASS separate user/root card metadata and independent app controls', flush=True)


    temporary = '/tmp/outer-wsl-test-' + uuid.uuid4().hex
    try:
        with Bridge(files['externalSocketPath']) as client:
            assert b'<!doctype html' in expect(client, '/').lower()
            listing = expect(client, '/api/files?path=~')
            assert listing[:4] == b'FFLS' and b'/home/' in listing
            expect(client, '/api/mkdir?' + urlencode(dict(directory='/tmp', name=temporary.rsplit('/', 1)[1])), method='POST')
            data = bytes(range(256)) * 1024
            name = 'WSL-雪-π.bin'
            expect(client, '/api/upload?' + urlencode(dict(directory=temporary, name=name)), data, 'POST')
            assert expect(client, '/api/download?' + urlencode(dict(path=temporary + '/' + name))) == data
            assert name.encode() in expect(client, '/api/files?' + urlencode(dict(path=temporary)))
        print('PASS 256 KiB binary transfer and Unicode filenames over both bridges', flush=True)
        allow = linux("from pathlib import Path; import base64; print(base64.b64encode((Path.home()/'.config/outerssh/http-unix.allow').read_bytes()).decode())").strip()
        try:
            linux("from pathlib import Path; p=Path.home()/'.config/outerssh/http-unix.allow'; p.write_text(''.join(line for line in p.read_text().splitlines(True) if line.strip().replace('%%t', '/run/user/'+str(__import__('os').getuid()))!=%r))" % files['socketPath'])
            with Bridge(files['externalSocketPath']) as client:
                status, _ = client.request('/api/files?path=~')
                assert status == 0, status
            print('PASS Linux allowlist revocation denies the next Windows-pipe connection', flush=True)
        finally:
            linux("from pathlib import Path; import base64; (Path.home()/'.config/outerssh/http-unix.allow').write_bytes(base64.b64decode(%r))" % allow.decode())
    finally:
        linux("import shutil; shutil.rmtree(%r, ignore_errors=True)" % temporary)
    operation('stop')
    wait_for(lambda w: w['state'] == 'stopped')
    for _ in range(3):
        time.sleep(3); assert workspace()['state'] == 'stopped'
    print('PASS snapshot discovery leaves stopped Ubuntu asleep', flush=True)
    expect(host_client, api + '/backends?web=1')
    print('PASS opening the Linux catalog starts the stopped distribution', flush=True)
    item = wait_for(lambda w: w['state'] == 'running' and w['apps'])
    with Bridge(files['externalSocketPath']) as client:
        assert expect(client, '/api/files?path=~')[:4] == b'FFLS'
    print('PASS distribution restart and stable endpoint reconnect', flush=True)
