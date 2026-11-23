#!/usr/bin/env python3
"""External build contexts, self-modifying mounts, and delete ownership in the Linux provider."""
import contextlib
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest
from unittest.mock import patch

provider = runpy.run_path(str(Path(__file__).resolve().parents[1] / "Resources/outershell-container-provider"))
g = provider["create_runtime_container"].__globals__
IDENTIFIER = "7c1d2e3f-4a5b-4c6d-8e9f-0a1b2c3d4e5f"


class ExternalBuildContextTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(os.path.realpath(self.temporary.name))
        self.home = self.root / "state"
        self.home.mkdir()
        self.external = self.root / "project"
        self.external.mkdir()
        (self.external / "Dockerfile").write_text("FROM debian:bookworm\n")
        self.calls = []
        self.resources = {}
        self.record = {"id": IDENTIFIER, "name": "External", "ownsContainer": True, "cpus": 2, "memoryInMB": 2048}

        def docker(arguments, check=False):
            self.calls.append(arguments)
            return subprocess.CompletedProcess(arguments, 0, "", "")

        @contextlib.contextmanager
        def records():
            class Values:
                values = [self.record]

                def save(self):
                    pass
            yield Values()

        self.patches = patch.dict(g, docker=docker, Records=records,
                                  outer_shell_home=lambda: self.home,
                                  api_socket_path=lambda: str(self.root / "api"),
                                  registry_resource_get=lambda key: self.resources.get(key),
                                  registry_resource_set=lambda key, value: self.resources.__setitem__(key, g["offset_archive_encode"](value)),
                                  registry_resource_remove=lambda key: self.resources.pop(key, None),
                                  registry_container_remove=lambda identifier: None,
                                  remove_runtime_container=lambda identifier: None,
                                  remove_owned_image=lambda identifier: None,
                                  container_directory=lambda identifier: self.home / "containers" / identifier)
        self.patches.start()
        self.environment = patch.dict(os.environ, {"XDG_RUNTIME_DIR": str(self.root / "runtime")})
        self.environment.start()
        g["CONTAINER_BUILD_CONTEXTS"][IDENTIFIER] = str(self.external)

    def tearDown(self):
        self.environment.stop()
        self.patches.stop()
        g["CONTAINER_BUILD_CONTEXTS"].pop(IDENTIFIER, None)
        g["SELF_MODIFYING_CONTAINERS"].pop(IDENTIFIER, None)
        self.temporary.cleanup()

    def mounts(self):
        create = next(call for call in self.calls if call[:1] == ["create"])
        return [create[index + 1] for index, value in enumerate(create) if value == "--mount"]

    def test_sealed_container_has_no_project_mount(self):
        g["SELF_MODIFYING_CONTAINERS"][IDENTIFIER] = False
        g["create_runtime_container"](IDENTIFIER)
        self.assertFalse(any("/var/lib/outershell/project" in mount for mount in self.mounts()))

    def test_self_modifying_container_mounts_the_external_folder(self):
        g["SELF_MODIFYING_CONTAINERS"][IDENTIFIER] = True
        g["create_runtime_container"](IDENTIFIER)
        self.assertIn(f"type=bind,src={self.external},dst=/var/lib/outershell/project", self.mounts())

    def test_linked_or_protected_build_contexts_are_refused(self):
        link = self.root / "link"
        link.symlink_to(self.external)
        g["CONTAINER_BUILD_CONTEXTS"][IDENTIFIER] = str(link)
        with self.assertRaisesRegex(ValueError, "not a link"):
            g["checked_build_context_directory"](IDENTIFIER)
        g["CONTAINER_BUILD_CONTEXTS"][IDENTIFIER] = str(self.root)
        g["SELF_MODIFYING_CONTAINERS"][IDENTIFIER] = True
        with self.assertRaisesRegex(ValueError, "Outer Shell's own folders"):
            g["checked_build_context_directory"](IDENTIFIER)
        g["SELF_MODIFYING_CONTAINERS"][IDENTIFIER] = False
        self.assertEqual(g["checked_build_context_directory"](IDENTIFIER), self.root)

    def test_switching_self_modifying_needs_a_rebuild(self):
        g["SELF_MODIFYING_CONTAINERS"][IDENTIFIER] = False
        with patch.dict(g, docker_path=lambda: "/usr/bin/docker"):
            g["mark_realized"](IDENTIFIER)
            configuration = g["load_configuration"](IDENTIFIER)
            self.assertFalse(g["definition_dictionary"](self.record, configuration)["needsRebuild"])
            g["SELF_MODIFYING_CONTAINERS"][IDENTIFIER] = True
            self.assertTrue(g["definition_dictionary"](self.record, configuration)["needsRebuild"])

    def test_registry_rows_keep_their_flags(self):
        self.assertEqual(g["CONTAINER_SELF_MODIFYING"], 0x08)
        record = {"ownsContainer": True, "ownsBuildContext": False, "selfModifying": True}
        self.assertEqual(g["registry_container_flags"](record), 0x09)

    def test_writable_mount_of_outer_shell_state_is_refused(self):
        request = {"workspaceID": IDENTIFIER, "mounts": [{"id": "m", "hostPath": str(self.home), "guestPath": "/state"}]}
        with patch.dict(g, require_managed_container=lambda record: None):
            with self.assertRaisesRegex(ValueError, "writable folder mount"):
                g["operation_update_configuration"](request)
            request["mounts"][0]["isReadOnly"] = True
            g["operation_update_configuration"](request)

    def test_delete_keeps_an_external_build_context(self):
        owned = self.home / "containers" / IDENTIFIER / "Runtime"
        owned.mkdir(parents=True)
        g["operation_delete"]({"workspaceID": IDENTIFIER})
        self.assertTrue((self.external / "Dockerfile").exists())
        self.assertFalse((self.home / "containers" / IDENTIFIER).exists())


class RecordsLockTests(unittest.TestCase):
    def test_failed_load_releases_the_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            def broken():
                raise RuntimeError("registry unavailable")
            with patch.dict(g, provider_root=lambda: Path(directory), registry_container_list=broken):
                for _ in range(2):
                    with self.assertRaisesRegex(RuntimeError, "registry unavailable"):
                        with g["Records"]():
                            pass


if __name__ == "__main__":
    unittest.main()
