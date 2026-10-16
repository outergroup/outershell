#!/usr/bin/env python3
"""Test Linux root bootstrap readiness without installing anything or using sudo."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import time

repo = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='outer-shell-root-readiness-') as temporary:
    root = Path(temporary)
    binary = root / 'test'
    sources = [repo / 'Tests/RootSupportReadinessTest.c']
    sources += [repo / 'Backend' / name for name in ('OuterShellBuffer.c', 'OuterShellAPI.c', 'OuterShellPlatform.c')]
    sources += [repo / 'Tests/OuterShellImageStubs.c', repo / 'outershelld/OuterService.c']
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu17', '-O0', '-o', str(binary)] +
                   list(map(str, sources)) + ['-ldl', '-lpthread', '-lm'], check=True)
    mocks = {
        'sudo': '''#!/bin/sh
if [ "$OUTERSHELL_TEST_MODE" = password ]; then echo 'sudo: a password is required' >&2; exit 1; fi
[ "$1" = -n ] || exit 99
shift
exec "$@"
''',
        'systemctl': '''#!/bin/sh
printf '%s\\n' "$*" >> "$OUTERSHELL_TEST_CALLS"
case "$2" in
  show)
    case "$OUTERSHELL_TEST_MODE" in
      exec) printf 'ActiveState=failed\\nResult=exit-code\\nExecMainStatus=203\\n';;
      *) printf 'ActiveState=active\\nResult=success\\nExecMainStatus=0\\n';;
    esac;;
  start)
    if [ "$OUTERSHELL_TEST_MODE" = socket ]; then echo 'Failed to listen on system API socket: Permission denied' >&2; exit 1; fi;;
esac
''',
        'journalctl': '''#!/bin/sh
printf '%s\\n' "$*" >> "$OUTERSHELL_TEST_CALLS"
if [ "$OUTERSHELL_TEST_MODE" = nojournal ]; then echo 'Journal unavailable' >&2; exit 1; fi
if [ "$OUTERSHELL_TEST_MODE" = exec ]; then
  echo 'outershelld.service: Failed at step EXEC spawning /opt/outershell/libexec/outershelld: Permission denied'
else
  echo 'outershelld.service: authentication initialization failed'
fi
''',
        "outerctl with 'quote": '''#!/bin/sh
[ "$1 $2" = 'backend list' ] || exit 99
[ "$OUTERSHELLD_API_SOCKET" = /run/outershelld-api ] || exit 99
[ "$OUTERSHELL_HOME" = /var/lib/outershell ] || exit 99
printf '%s\\n' probe >> "$OUTERSHELL_TEST_CALLS"
case "$OUTERSHELL_TEST_MODE" in
  healthy) echo 'PRIVATE REGISTRY OUTPUT';;
  hang) sleep 100;;
  *) echo 'Failed to read outershelld API response header: Connection reset by peer' >&2; exit 1;;
esac
'''
    }
    for name, contents in mocks.items():
        path = root / name; path.write_text(contents); path.chmod(0o700)
    script = root / 'probe.sh'
    script.write_bytes(subprocess.check_output([str(binary), 'emit', str(root / "outerctl with 'quote")]))
    environment = dict(os.environ, PATH=str(root) + ':' + os.environ['PATH'], OUTERSHELL_TEST_CALLS=str(root / 'calls'),
                       OUTERSHELLD_API_SOCKET='/wrong/inherited/socket', OUTERSHELL_HOME='/wrong/inherited/state')
    for mode in ('healthy', 'exec', 'socket', 'hang', 'nojournal', 'password'):
        (root / 'calls').write_text('')
        started = time.monotonic()
        result = subprocess.run([str(binary), 'run', str(script)], env=dict(environment, OUTERSHELL_TEST_MODE=mode),
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=25)
        output = result.stdout
        if mode == 'healthy':
            assert result.returncode == 0 and output == 'APP_OPERATION_STARTED\n', output
            assert 'show' not in (root / 'calls').read_text()
        elif mode == 'password':
            assert result.returncode == 2 and output == 'Administrator password required.', output
        else:
            assert result.returncode == 1, output
            assert output.startswith('Root Outer Shell support could not start.'), output
            assert 'APP_OPERATION_STARTED' not in output and 'PRIVATE REGISTRY OUTPUT' not in output
            assert '--since=' in (root / 'calls').read_text() and '--lines=12' in (root / 'calls').read_text()
            if mode == 'exec':
                assert '203/EXEC' in output and 'Permission denied' in output and 'SELinux' in output
            elif mode == 'socket':
                assert 'The system API socket could not start.' in output and 'probe' not in (root / 'calls').read_text()
            elif mode == 'hang':
                assert 'within 15 seconds' in output and 14 <= time.monotonic() - started < 23
            elif mode == 'nojournal':
                assert 'Journal unavailable' in output and 'Connection reset by peer' in output
        print(f'PASS: root support readiness: {mode}')
