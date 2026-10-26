#!/usr/bin/env python3
from pathlib import Path
import runpy
import subprocess
import unittest
from unittest.mock import patch

provider = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'Resources/outershell-container-provider'))
g = provider['app_snapshots'].__globals__

class TCPAppTests(unittest.TestCase):
    def test_declared_adapter_paths(self):
        output = "demo\t7860\t'/run/user/1000/demo'\nbad\t0\t'/tmp/bad'\nbad2\t80\t'relative'\n"
        with patch.dict(g, container_exec=lambda *a, **k: subprocess.CompletedProcess([], 0, output)):
            self.assertEqual(g['declared_app_tcp_endpoints']('lab'),
                             {'demo': {'port': 7860, 'socketPath': '/run/user/1000/demo'}})

    def snapshots(self, kind='1', host='127.0.0.1', port='7860', socket='', scheme='1'):
        columns = ['service_id', 'frontend_id', 'display_name', 'endpoint_kind', 'scheme', 'host', 'port', 'socket_path', 'url']
        row = ['demo', 'demo:main', 'Demo', kind, scheme, host, port, socket, '/app']
        output = '\t'.join(columns) + '\n' + '\t'.join(row) + '\n'
        with patch.dict(g, container_exec=lambda *a, **k: subprocess.CompletedProcess([], 0, output),
                        container_running_service_ids=lambda _: {'demo'},
                        declared_app_tcp_endpoints=lambda _: {'demo': {'port': 7860, 'socketPath': '/run/user/1000/demo'}},
                        runtime_published_ports=lambda _: {}, copy_icon=lambda *a: None):
            return g['app_snapshots']('lab', {})

    def test_native_tcp_uses_declared_adapter(self):
        app, = self.snapshots()
        self.assertEqual(app['socketPath'], '/run/user/1000/demo')
        self.assertEqual(app['frontendID'], 'demo:main')
        self.assertEqual(app['url'], '/app')
        self.assertTrue(app['isRunning'])

    def test_unrelated_tcp_endpoints_are_not_redirected(self):
        for kwargs in ({'port': '8888'}, {'host': 'remote.example'}, {'scheme': '2'}):
            self.assertEqual(self.snapshots(**kwargs), [])

    def test_native_unix_endpoint_is_preserved(self):
        app, = self.snapshots(kind='2', socket='/run/user/1000/native')
        self.assertEqual(app['socketPath'], '/run/user/1000/native')

if __name__ == '__main__':
    unittest.main()
