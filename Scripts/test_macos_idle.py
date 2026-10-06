#!/usr/bin/env python3
"""Test an agent build in a disposable GUI launchd job without altering installation."""
import argparse
import os
from pathlib import Path
import plistlib
import re
import shlex
import socket
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', required=True, type=Path, help='Outer Shell.app bundle')
    parser.add_argument('--outerctl', required=True, type=Path)
    args = parser.parse_args()
    executable = args.app.resolve() / 'Contents/MacOS/Outer Shell'
    web_root = Path(__file__).resolve().parents[1] / 'Resources/OuterShellWeb'
    with tempfile.TemporaryDirectory(prefix='outershell-idle-') as directory:
        root = Path(directory)
        label = 'org.outershell.IdleTest.' + root.name.split('-')[-1]
        domain = 'gui/' + str(os.getuid())
        target = domain + '/' + label
        (root / 'home').mkdir()
        wrapper = root / 'run.sh'
        wrapper.write_text(
            '#!/bin/sh\nexport HOME=' + shlex.quote(str(root / 'home')) + '\n'
            'exec /usr/bin/env '
            '"OUTERSHELL_SOCKET_org.outershell.OuterShell=$OSB_TEST_HTTP" '
            '"OUTERSHELL_API_SOCKET_org.outershell.OuterShell=$OSB_TEST_API" '
            '"OUTERSHELLD_API_SOCKET=$OSB_TEST_API" ' + shlex.quote(str(executable)) +
            ' --socket-path "$OSB_TEST_HTTP" --api-socket-path "$OSB_TEST_API" '
            '--launchd-socket-name Listener --database ' + shlex.quote(str(root / 'registry.orwa')) +
            ' --system-database ' + shlex.quote(str(root / 'system.orwa')) +
            ' --web-root ' + shlex.quote(str(web_root)) + '\n'
        )
        plist = root / 'job.plist'
        plist.write_bytes(plistlib.dumps({
            'Label': label,
            'RunAtLoad': True,
            'ProgramArguments': ['/bin/sh', str(wrapper)],
            'Sockets': {
                'Listener': {'SecureSocketWithKey': 'OSB_TEST_HTTP', 'SockPathMode': 384},
                'API': {'SecureSocketWithKey': 'OSB_TEST_API', 'SockPathMode': 384},
            },
            'StandardOutPath': str(root / 'log'),
            'StandardErrorPath': str(root / 'log'),
        }))

        def status():
            return subprocess.run(['launchctl', 'print', target], capture_output=True, text=True).stdout

        def pid():
            match = re.search(r'^\s*pid = (\d+)$', status(), re.M)
            return int(match.group(1)) if match else None

        subprocess.run(['launchctl', 'bootstrap', domain, str(plist)], check=True)
        paths = []
        try:
            job = status()
            for name in ['Listener', 'API']:
                match = re.search(r'\n\s*"?' + name + r'"? = \{.*?path = ([^\n]+)', job, re.S)
                assert match, job
                paths.append(Path(match.group(1)))
            http, api = paths
            deadline = time.monotonic() + 20
            while pid() is None and time.monotonic() < deadline:
                time.sleep(0.1)
            assert pid() is not None
            print('PASS: launchd started agent when job loaded', flush=True)
            with socket.socket(socket.AF_UNIX) as connection:
                connection.connect(str(http))
                connection.sendall(b'GET / HTTP/1.1\r\nHost: localhost\r\nX-Idle-Test: ')
                for _ in range(40):
                    time.sleep(2)
                    connection.sendall(b'x')
                    assert pid() is not None
                first = pid()
            print('PASS: open HTTP connection kept process alive beyond idle interval', flush=True)
            deadline = time.monotonic() + 190
            while pid() is not None and time.monotonic() < deadline:
                time.sleep(2)
            assert pid() is None, status()
            assert all(path.exists() for path in paths)
            print('PASS: whole process exited idle; launchd preserved both socket files', flush=True)
            result = subprocess.run(
                [str(args.outerctl.resolve()), 'backend', 'list'],
                env=dict(os.environ, OUTERSHELLD_API_SOCKET=str(api)),
                capture_output=True, text=True, timeout=20,
            )
            assert result.returncode == 0, (result.stdout, result.stderr)
            assert pid() is not None and pid() != first
            print('PASS: outerctl API connection woke a new process', flush=True)
        finally:
            subprocess.run(['launchctl', 'bootout', target], capture_output=True)
            if (root / 'log').exists():
                print((root / 'log').read_text())
            for path in paths:
                try:
                    path.unlink(missing_ok=True)
                    path.parent.rmdir()
                except OSError:
                    pass


if __name__ == '__main__':
    main()
