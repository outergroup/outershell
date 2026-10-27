#!/usr/bin/env python3
"""Verify bundled app installation preserves executability without widening access."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    binary = root / 'test'
    sources = [repo / 'Tests/ResourcePermissionsTest.c']
    sources += [repo / 'Backend' / name for name in ('OuterShellBuffer.c', 'OuterShellAPI.c', 'OuterShellPlatform.c')]
    sources += [repo / 'Tests/OuterShellImageStubs.c', repo / 'outershelld/OuterService.c']
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu17', '-O0', '-o', str(binary)] +
                   list(map(str, sources)) + ['-ldl', '-lpthread', '-lm'], check=True)
    source = root / 'source'
    source.mkdir()
    for name, mode in [('helper', 0o755), ('data', 0o644), ('privileged', 0o6755)]:
        (source / name).write_text(name)
        (source / name).chmod(mode)
    for mode, expected in [('600', 0o700), ('644', 0o755)]:
        destination = root / mode
        subprocess.run([str(binary), str(source), str(destination), mode], check=True)
        for name in ['helper', 'privileged']:
            assert (destination / name).stat().st_mode & 0o7777 == expected
        assert (destination / 'data').stat().st_mode & 0o7777 == int(mode, 8)
        assert (destination / 'helper').read_text() == 'helper'
print('PASS: private and shared resource permissions')
