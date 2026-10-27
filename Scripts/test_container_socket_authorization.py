import pathlib
import runpy
import subprocess
import tempfile
import unittest
from unittest.mock import patch

provider = runpy.run_path(str(pathlib.Path(__file__).resolve().parents[1] / 'Resources/outershell-container-provider'))
authorize = provider['authorize_container_socket']


class AuthorizationTests(unittest.TestCase):
    def check_scope(self, owner, socket_path, scope):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            def execute(container, command, **options):
                self.assertEqual(container, 'container')
                self.assertTrue(options['check'])
                if command[0] == '/usr/bin/stat':
                    self.assertEqual(command, ['/usr/bin/stat', '-c', '%u', '--', socket_path])
                    return subprocess.CompletedProcess(command, 0, owner + '\n')
                script = command[2].replace('/root/.config/outerssh', str(root / 'user')).replace('/etc/outerssh', str(root / 'system'))
                return subprocess.run(['/bin/sh', '-c', script], check=True)
            with patch.dict(authorize.__globals__, container_exec=execute):
                authorize('container', socket_path)
                authorize('container', socket_path)
            allowlist = root / scope / 'http-unix.allow'
            self.assertEqual(allowlist.read_text().splitlines(), [socket_path])
            self.assertEqual(allowlist.stat().st_mode & 0o777, 0o600)
            self.assertFalse((root / ('system' if scope == 'user' else 'user')).exists())

    def test_root_runtime_socket(self):
        self.check_scope('0', '/run/user/0/org.outershell.Files', 'user')

    def test_nonroot_owner_in_root_runtime(self):
        self.check_scope('1000', '/run/user/0/org.outershell.Files', 'system')

    def test_nonroot_runtime_socket(self):
        self.check_scope('1000', '/run/user/1000/org.outershell.Files', 'system')

    def test_root_owned_socket_outside_root_runtime(self):
        self.check_scope('0', '/run/user/01/socket', 'system')

    def test_exact_path_and_shell_quoting(self):
        self.check_scope('1000', "/tmp/socket with 'quotes' $(false)", 'system')

    def test_stat_failure_does_not_authorize(self):
        with patch.dict(authorize.__globals__, container_exec=lambda *a, **k: (_ for _ in ()).throw(RuntimeError('missing socket'))):
            with self.assertRaisesRegex(RuntimeError, 'missing socket'):
                authorize('container', '/missing')


if __name__ == '__main__':
    unittest.main()
