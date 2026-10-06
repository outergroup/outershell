#!/usr/bin/env python3
"""Exercise SecureSocketWithKey without modifying installed launchd jobs.

Socket discovery uses a one-shot job in the target launchd domain.
launchctl print parsing is used only for test assertions, not discovery.
Use --probe-binary to run a precompiled probe on a Mac without developer tools.
"""
import argparse
import os
from pathlib import Path
import plistlib
import re
import stat
import subprocess
import tempfile
import time
import uuid


def run(*args, check=True):
    return subprocess.run(args, check=check, text=True, capture_output=True)


def resolve_socket(directory, domain, key):
    label = 'org.outershell.socket-resolver.' + uuid.uuid4().hex
    output = directory / (label + '.out')
    plist = directory / (label + '.plist')
    plist.write_bytes(plistlib.dumps({
        'Label': label,
        'ProgramArguments': ['/usr/bin/printenv', key],
        'RunAtLoad': True,
        'StandardOutPath': str(output),
        'StandardErrorPath': str(directory / (label + '.err')),
    }))
    run('launchctl', 'bootstrap', domain, str(plist))
    try:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            value = output.read_text() if output.exists() else ''
            if value.endswith('\n'):
                return Path(value.strip())
            time.sleep(0.05)
        raise RuntimeError('Resolver did not publish a socket path')
    finally:
        run('launchctl', 'bootout', domain + '/' + label)


def exercise(directory, binary, app=None):
    token = uuid.uuid4().hex
    label = 'org.outershell.socket-prototype.' + token
    key = 'OUTERSHELL_PROTOTYPE_' + token.upper()
    domain = 'gui/' + str(os.getuid())
    target = domain + '/' + label
    log = directory / (token + '.log')
    arguments = [str(binary)]
    if app:
        wrapper = directory / (token + '.sh')
        command = 'exec "$1" --label "$2" --socket-path "$' + key + '" --launchd-socket-name Listener'
        if app == 'Files':
            command += ' --api-socket-path "$3"'
        wrapper.write_text('#!/bin/sh\n' + command + '\n')
        arguments = ['/bin/sh', str(wrapper), str(binary), label, str(directory / 'unused-api')]
    config = {
        'Label': label,
        'ProgramArguments': arguments,
        'Sockets': {'Listener': {'SecureSocketWithKey': key, 'SockPathMode': 0o600}},
        'StandardErrorPath': str(log),
        'StandardOutPath': str(log),
    }
    plist = directory / (token + '.plist')
    plist.write_bytes(plistlib.dumps(config))
    previous_path = None
    for cycle in range(2):
        socket_path = None
        loaded = False
        try:
            run('launchctl', 'bootstrap', domain, str(plist))
            loaded = True
            state = run('launchctl', 'print', target).stdout
            assert '\tstate = not running\n' in state, state
            paths = re.findall(r'^\t\t\tpath = (/.+)$', state, re.MULTILINE)
            assert len(paths) == 1, state
            socket_path = resolve_socket(directory, domain, key)
            assert socket_path == Path(paths[0])
            assert '\tstate = not running\n' in run('launchctl', 'print', target).stdout
            assert socket_path != previous_path, 'Reload reused path'
            previous_path = socket_path
            metadata = socket_path.stat()
            assert stat.S_ISSOCK(metadata.st_mode)
            assert metadata.st_uid == os.getuid()
            assert stat.S_IMODE(metadata.st_mode) == 0o600
            parent = socket_path.parent.stat()
            assert parent.st_uid == os.getuid()
            assert stat.S_IMODE(parent.st_mode) == 0o700
            inherited = run('launchctl', 'getenv', key, check=False).stdout.strip()
            print(f'{app or "Probe"}, cycle {cycle + 1}: {socket_path}; getenv={inherited or "<absent>"}', flush=True)
            for request in range(1 if app else 2):
                response = run('curl', '--silent', '--show-error', '--max-time', '25',
                               '--unix-socket', str(socket_path), '-o', '/dev/null',
                               '-w', '%{http_code}', 'http://localhost/')
                assert response.stdout == '200', response.stdout
                print(f'  request {request + 1}: HTTP 200', flush=True)
            if not app:
                entries = re.findall(r'pid=(\d+) launch_activate_socket=0 count=1', log.read_text())
                assert len(entries) == (cycle + 1) * 2, log.read_text()
                assert len(set(entries)) == len(entries), entries
            else:
                assert 'launch_activate_socket' not in log.read_text(), log.read_text()
        finally:
            if loaded:
                run('launchctl', 'bootout', target)
            if socket_path and socket_path.exists():
                print('  Socket file persisted after bootout; removing test socket.', flush=True)
                socket_path.unlink()
                try:
                    socket_path.parent.rmdir()
                except PermissionError:
                    print("  Empty launchd directory retained: parent directory is protected.", flush=True)
        assert run('launchctl', 'print', target, check=False).returncode != 0
        assert not run('launchctl', 'getenv', key, check=False).stdout.strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe-binary', type=Path)
    parser.add_argument('--installed-apps', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='outershell-secure-socket-') as temporary:
        directory = Path(temporary)
        binary = args.probe_binary
        if binary is None:
            binary = directory / 'probe'
            run('clang', str(Path(__file__).with_name('secure_socket_probe.c')), '-o', str(binary))
            run('codesign', '--force', '--sign', '-', str(binary))
        exercise(directory, binary.resolve())
        if args.installed_apps:
            root = Path.home() / 'Library/Application Support/outershell/apps'
            for app in ('Top', 'Files', 'Plaintext'):
                executable = root / ('org.outershell.' + app) / (app + '.app') / 'Contents/MacOS' / (app + 'Backend')
                exercise(directory, executable, app)
    print('PASS: activation, prelaunch discovery, reactivation, reload, permissions, cleanup.')


if __name__ == '__main__':
    main()
