#!/usr/bin/env python3
"""Internal service status must not launch systemd probes."""
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
from test_layout_storage import exact


def status(api):
    query = b'sinceBackends=0&sinceLog=0&sinceOverview=0'
    message = struct.pack('<HHIIIII', 26, 7, 0, 24, len(query), 24 + len(query), 0) + query
    with socket.socket(socket.AF_UNIX) as sock:
        sock.settimeout(5)
        sock.connect(str(api))
        sock.sendall(struct.pack('<I', len(message)) + message)
        reply = exact(sock, struct.unpack('<I', exact(sock, 4))[0])
        assert reply


expect_system_fallback = '--expect-system-fallback' in sys.argv[2:]
modes = ['internal']
if sys.platform.startswith('linux') and (expect_system_fallback or not Path('/run/systemd/system').exists()):
    modes.append('system')
for mode in modes:
    with tempfile.TemporaryDirectory(prefix='shell-systemd-') as directory:
        root = Path(directory)
        (root / 'services').mkdir()
        (root / 'bin').mkdir()
        probe = root / 'bin/timeout'
        probe.write_text('#!/bin/sh\nprintf probe >> "$PROBE_LOG"\nexit 127\n')
        probe.chmod(0o755)
        api = root / 'api'
        env = dict(os.environ, OUTERSHELL_HOME=str(root), XDG_RUNTIME_DIR=str(root),
                   PATH=str(root / 'bin') + ':' + os.environ['PATH'], PROBE_LOG=str(root / 'probes'))
        with (root / 'log').open('wb') as log:
            daemon = subprocess.Popen([sys.argv[1], '--api-socket-path', str(api),
                '--service-manager', mode, '--services-dir', str(root / 'services'), '--stay-alive'],
                env=env, stdout=log, stderr=log)
            try:
                for _ in range(100):
                    if api.exists(): break
                    if daemon.poll() is not None: raise RuntimeError((root / 'log').read_text())
                    time.sleep(.05)
                for _ in range(4):
                    status(api)
                    time.sleep(.6)
                if mode == 'system' and expect_system_fallback:
                    assert (root / 'probes').exists(), 'Available systemd scope lost its CLI fallback'
                    print('PASS: available systemd scope retains its CLI fallback')
                else:
                    assert not (root / 'probes').exists(), f'{mode} status launched systemctl probes'
                    print(f'PASS: {mode} status does not poll an unused systemd manager')
            finally:
                daemon.terminate()
                daemon.wait(timeout=5)
