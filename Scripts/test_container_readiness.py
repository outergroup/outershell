#!/usr/bin/env python3
import os
from pathlib import Path
import runpy
import subprocess
import unittest
from unittest.mock import patch

provider = runpy.run_path(str(Path(__file__).resolve().parents[1] / "Resources/outershell-container-provider"))
g = provider["probe_docker_readiness"].__globals__

class ReadinessTests(unittest.TestCase):
    def probe(self, executable="/fake/docker", host="unix:///custom/docker.sock", socket=True, code=0, output='["name=rootless"]', error=""):
        with patch.dict(g, docker_path=lambda: executable, docker_host=lambda: host), \
             patch.object(Path, "is_socket", return_value=socket), \
             patch.dict(g, run=lambda *args, **kwargs: subprocess.CompletedProcess(args, code, output, error)):
            return g["probe_docker_readiness"]()

    def test_missing_and_stopped(self):
        self.assertEqual(self.probe(executable=None)[0], "notInstalled")
        status, detail = self.probe(socket=False)
        self.assertEqual(status, "stopped")
        self.assertIn("unix:///custom/docker.sock", detail)
        self.assertEqual(self.probe(code=1, error="Cannot connect to the Docker daemon")[0], "stopped")

    def test_engine_must_respond_and_be_rootless(self):
        self.assertEqual(self.probe()[0], "ready")
        self.assertEqual(self.probe(output='["name=seccomp"]')[0], "failed")
        self.assertEqual(self.probe(output="invalid JSON")[0], "failed")
        self.assertEqual(self.probe(code=1, error="permission denied")[0], "failed")
        self.assertEqual(self.probe(host="tcp://test:123", socket=False)[0], "ready")

    def test_timeout(self):
        def timeout(*args, **kwargs):
            self.assertEqual(kwargs["timeout"], 3)
            raise subprocess.TimeoutExpired(args, 3)
        with patch.dict(g, docker_path=lambda: "/fake/docker", docker_host=lambda: "tcp://test:123", run=timeout):
            self.assertEqual(g["probe_docker_readiness"]()[0], "failed")

    def test_selected_host_overrides_inherited_context(self):
        with patch.dict(os.environ, DOCKER_CONTEXT="wrong", OUTER_SHELL_DOCKER_HOST="unix:///custom.sock"):
            env = g["docker_environment"]()
            self.assertNotIn("DOCKER_CONTEXT", env)
            self.assertEqual(env["DOCKER_HOST"], "unix:///custom.sock")

    def test_recheck_after_install(self):
        with patch.dict(g, _docker_readiness_cache=(g["time"].monotonic(), ("notInstalled", "missing")),
                        probe_docker_readiness=lambda: ("ready", "ready"), list_workspaces=lambda **kwargs: []):
            self.assertEqual(g["provider_dictionary"]()["status"], "notInstalled")
            result = g["handle_request"]({"operation": "checkRuntimes"})
            self.assertTrue(result["providers"][0]["isAvailable"])

    def test_create_fails_before_writing_when_unready(self):
        with patch.dict(g, docker_readiness=lambda: ("stopped", "Start Docker")):
            with self.assertRaisesRegex(RuntimeError, "Start Docker"):
                g["operation_create"]({})

if __name__ == "__main__":
    unittest.main()
