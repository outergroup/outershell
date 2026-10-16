#!/usr/bin/env python3
"""Exercise generated SELinux setup without modifying system policy."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='outer-shell-selinux-test-') as temporary:
    root = Path(temporary)
    binary = root / 'test'
    sources = [repo / 'Tests/RootSupportReadinessTest.c']
    sources += [repo / 'Backend' / name for name in ('OuterShellBuffer.c', 'OuterShellAPI.c', 'OuterShellPlatform.c')]
    sources += [repo / 'Tests/OuterShellImageStubs.c', repo / 'outershelld/OuterService.c']
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu17', '-O0', '-o', str(binary)] +
                   list(map(str, sources)) + ['-ldl', '-lpthread', '-lm'], check=True)
    functions = subprocess.check_output([str(binary), 'selinux', 'unused'], text=True)
    package = (repo / 'Scripts/package_release.sh').read_text()
    packaged_functions = package.split("cat <<'OUTERSHELL_SELINUX_SH'\n", 1)[1].split('OUTERSHELL_SELINUX_SH\n', 1)[0]
    assert packaged_functions == functions, 'Public installer and root bootstrap labeling must agree'
    assert 'semanage' not in functions and 'chcon' not in functions and 'setenforce' not in functions
    subprocess.run(['/bin/sh', '-n'], input=functions, text=True, check=True)
    mocks = root / 'bin'
    mocks.mkdir()
    (mocks / 'selinuxenabled').write_text('#!/bin/sh\n[ "$TEST_MODE" != disabled ]\n')
    (mocks / 'selinuxenabled').chmod(0o700)
    tool_script = '''#!/bin/sh
printf '%s\\n' "$0" "$@" >> "$TEST_CALLS"
case "$0:$TEST_MODE" in
    */restorecon:label-failure)
        echo 'test policy failure' >&2
        exit 1;;
esac
'''
    target = "/opt/Outer Shell.v2/bin/[app]+(root){x}$^|?*/back\\end"
    script = root / 'setup.sh'
    script.write_text('set -eu\n' + functions + 'restore_selinux_context ' + shlex.quote(target) + "\nprintf 'READY\\n'\n")
    for mode in ('enabled', 'disabled', 'missing-restorecon', 'label-failure'):
        for tool in ('restorecon',):
            path = mocks / tool
            path.unlink(missing_ok=True)
            if mode != 'missing-' + tool:
                path.write_text(tool_script)
                path.chmod(0o700)
        calls = root / 'calls'
        calls.write_text('')
        env = dict(os.environ, PATH=str(mocks), TEST_MODE=mode, TEST_CALLS=str(calls))
        result = subprocess.run(['/bin/sh', str(script)], env=env, text=True, capture_output=True, timeout=10)
        if mode in ('enabled', 'disabled'):
            assert result.returncode == 0 and result.stdout == 'READY\n', result
        else:
            assert result.returncode == 70 and 'READY' not in result.stdout, result
            assert 'SELinux' in result.stderr, result.stderr
        entries = calls.read_text().splitlines()
        if mode == 'enabled':
            assert entries[1:] == ['-F', '--', target], entries
        elif mode == 'disabled' or mode.startswith('missing-'):
            assert not entries, entries
        print('PASS: SELinux setup:', mode)
    subprocess.run([str(binary), 'paths', 'unused'], check=True)
    print('PASS: system tools and state use separate roots')

    variable_setup = package.split('root_install=false\n', 1)[1].split('service_manager="${OUTERSHELL_SERVICE_MANAGER:-auto}"', 1)[0]
    variable_setup = 'root_install=false\n' + variable_setup
    runtime_setup = package.split('services_dir="$outershell_home/outershelld/services"', 1)[1].split('outershelld_pid_path="$outershell_home/outershelld/outershelld.pid"', 1)[0] + 'outershelld_pid_path="$outershell_home/outershelld/outershelld.pid"\n'
    runtime_setup = 'services_dir="$outershell_home/outershelld/services"' + runtime_setup
    (mocks / 'id').write_text('#!/bin/sh\nprintf "%s\\n" "$TEST_UID"\n')
    (mocks / 'id').chmod(0o700)
    state = str(root / 'custom-state')
    for uid in ('0', '1000'):
        result = subprocess.run(['/bin/sh'], input=variable_setup + runtime_setup + '''
printf '%s\\n' "$outershelld_path" "$outerctl_path" "$outer_shell_binary" "$services_dir" "$outershelld_pid_path"
''', env=dict(os.environ, PATH=str(mocks), TEST_UID=uid, OUTERSHELL_HOME=state, HOME=str(root)), text=True, capture_output=True, check=True)
        actual = result.stdout.splitlines()
        expected = ['/opt/outershell/libexec/outershelld', '/opt/outershell/bin/outerctl', '/opt/outershell/outer-shell/bin/OuterShellBackend'] if uid == '0' else [state + '/outershelld/outershelld', state + '/bin/outerctl', state + '/outer-shell/OuterShellBackend']
        assert actual == expected + [state + '/outershelld/services', state + '/outershelld/outershelld.pid'], actual
    print('PASS: public installer root/user layouts and state override')

    # Execute production cleanup in a disposable filesystem with systemctl stubbed.
    (mocks / 'systemctl').write_text('#!/bin/sh\nexit 0\n')
    (mocks / 'systemctl').chmod(0o700)
    for case in ('shared-user', 'installed-app', 'last-app'):
        fixture = root / case
        payload = fixture / 'opt/outershell'
        data = fixture / 'var/lib/outershell'
        for path in (payload / 'bin/outerctl', payload / 'libexec/outershelld', payload / 'libexec/version', data / 'registry.orwa'):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('test')
        markers = data / 'system-binary-users'
        markers.mkdir()
        if case == 'shared-user':
            (markers / ('uid-' + str(os.getuid()))).touch()
        elif case == 'installed-app':
            (payload / 'org.outershell.Top/bin').mkdir(parents=True)
        else:
            (markers / 'root-apps').touch()
        mode = 'cleanup-apps' if case == 'last-app' else 'cleanup'
        cleanup = subprocess.check_output([str(binary), mode, 'unused'], text=True)
        for prefix in ('/opt/outershell', '/var/lib/outershell', '/var/log/outershell', '/etc/systemd/system', '/run/outershelld-api'):
            cleanup = cleanup.replace(prefix, str(fixture) + prefix)
        subprocess.run(['/bin/sh'], input=cleanup, text=True, env=dict(os.environ, PATH=str(mocks) + ':' + os.environ['PATH']), check=True)
        if case == 'shared-user':
            assert (payload / 'libexec/outershelld').exists() and (data / 'registry.orwa').exists()
        elif case == 'installed-app':
            assert (payload / 'org.outershell.Top').is_dir() and (data / 'registry.orwa').exists()
        else:
            assert not (markers / 'root-apps').exists() and not (payload / 'libexec').exists() and not payload.exists()
        print('PASS: executable cleanup:', case)
