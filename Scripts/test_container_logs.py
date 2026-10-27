import base64
import pathlib
import runpy
import unittest
import urllib.parse
from unittest.mock import patch

provider = runpy.run_path(str(pathlib.Path(__file__).resolve().parents[1] / 'Resources/outershell-container-provider'))
logs = provider['operation_app_logs']


class ContainerLogsTests(unittest.TestCase):
    def test_registered_logs_are_read_through_daemon(self):
        calls = []
        def api(container, route, query=b''):
            calls.append((container, route, query))
            return 200, b'configured log metadata' if route == 1 else b'bounded log response'
        with patch.dict(logs.__globals__, container_ui_api_request=api):
            result = logs({'workspaceID': 'lab', 'serviceID': 'app&other=value', 'logIndex': 2})['appLog']
        self.assertEqual(base64.b64decode(result['backendsPayload']), b'configured log metadata')
        self.assertEqual(base64.b64decode(result['logPayload']), b'bounded log response')
        self.assertEqual(calls[0], ('lab', 1, b''))
        self.assertEqual(calls[1][:2], ('lab', 2))
        self.assertEqual(urllib.parse.parse_qs(calls[1][2].decode()), {'serviceID': ['app&other=value'], 'logIndex': ['2'], 'bytes': ['524288']})

    def test_invalid_requests(self):
        for value in (-1, '1', True, 0.5):
            with self.assertRaises(ValueError):
                logs({'workspaceID': 'lab', 'serviceID': 'app', 'logIndex': value})
        with self.assertRaises(ValueError):
            logs({'workspaceID': 'lab'})

    def test_daemon_errors(self):
        for failing_route in (1, 2):
            with patch.dict(logs.__globals__, container_ui_api_request=lambda c, r, q=b'': (503 if r == failing_route else 200, b'')):
                with self.assertRaises(RuntimeError):
                    logs({'workspaceID': 'lab', 'serviceID': 'app'})

    def test_log_requests_skip_discovery(self):
        handle = provider['handle_request']
        with patch.dict(handle.__globals__, operation_app_logs=lambda request: {'appLog': {}}, list_workspaces=lambda **kwargs: self.fail('Log polling must not rediscover containers')):
            result = handle({'operation': 'appLogs', 'workspaceID': '81ada04d-59da-4ad9-9e6a-32a7f8fc1482', 'serviceID': 'app'})
        self.assertEqual(result['appLog'], {})
        self.assertNotIn('workspaces', result)


if __name__ == '__main__':
    unittest.main()
