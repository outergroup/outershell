#!/usr/bin/env python3
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest
from unittest.mock import patch

provider = runpy.run_path(str(Path(__file__).resolve().parents[1] / "Resources/outershell-container-provider"))
g = provider["stage_build_context"].__globals__


class BuildContextLinkTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.outside = self.root / "outside"
        self.outside.mkdir()
        (self.outside / "secret").write_text("host secret\n")
        self.build_context = self.root / "BuildContext"
        self.build_context.mkdir()
        (self.build_context / "Dockerfile").write_text("FROM debian:bookworm\n")
        (self.build_context / "kept").write_text("kept\n")
        self.patches = patch.dict(g, build_context_directory=lambda _: self.build_context,
                                  dockerfile_path=lambda _: self.build_context / "Dockerfile",
                                  runtime_directory=lambda _: self.root / "Runtime")
        self.patches.start()

    def tearDown(self):
        self.patches.stop()
        self.temporary.cleanup()

    def test_build_context_keeps_links_as_links(self):
        os.symlink("../outside/secret", self.build_context / "relative")
        os.symlink(self.outside / "secret", self.build_context / "absolute")
        os.symlink(self.outside, self.build_context / "folder")
        context = g["stage_build_context"]("qa")
        for name in ("relative", "absolute", "folder"):
            self.assertTrue((context / name).is_symlink(), name)
        self.assertEqual(os.readlink(context / "relative"), "../outside/secret")
        self.assertEqual((context / "kept").read_text(), "kept\n")

    def test_build_context_rejects_special_files(self):
        os.mkfifo(self.build_context / "pipe")
        with self.assertRaisesRegex(Exception, "special file"):
            g["stage_build_context"]("qa")

    def test_linked_dockerfile_is_neither_read_nor_written_through(self):
        (self.build_context / "Dockerfile").unlink()
        os.symlink(self.outside / "secret", self.build_context / "Dockerfile")
        with self.assertRaisesRegex(RuntimeError, "symbolic link"):
            g["read_dockerfile"]("qa")
        g["write_dockerfile"]("qa", "FROM alpine\n")
        self.assertFalse((self.build_context / "Dockerfile").is_symlink())
        self.assertEqual((self.build_context / "Dockerfile").read_text(), "FROM alpine\n")
        self.assertEqual((self.outside / "secret").read_text(), "host secret\n")
        self.assertEqual(sorted(path.name for path in self.build_context.iterdir()), ["Dockerfile", "kept"])

    def test_shared_build_context_rejects_links(self):
        destination = self.root / "Shared"
        g["copy_build_context_tree"](self.build_context, destination)
        g["require_unlinked_tree"](destination)
        nested = self.build_context / "nested"
        nested.mkdir()
        os.symlink(self.outside / "secret", nested / "leak")
        destination = self.root / "Shared Again"
        g["copy_build_context_tree"](self.build_context, destination)
        with self.assertRaisesRegex(RuntimeError, "nested/leak"):
            g["require_unlinked_tree"](destination)
        self.assertTrue((destination / "nested" / "leak").is_symlink())


class LegacyDefinitionMigrationTests(unittest.TestCase):
    def test_folder_key_and_image_move_to_new_names(self):
        identifier = "3633c063-e2d9-4eca-96d6-302c8dbfa565"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            legacy = root / "Recipe"
            legacy.mkdir()
            (legacy / "Dockerfile").write_text("FROM debian:bookworm\n")
            resources = {f"containers/{identifier}/recipe": g["offset_archive_encode"]({"mounts": [{"id": "m"}]})}
            images = {f"outershell/container-{identifier}:recipe"}
            commands = []

            def docker(arguments, check=False):
                commands.append(arguments)
                if arguments[:2] == ["image", "inspect"]:
                    return subprocess.CompletedProcess(arguments, 0 if arguments[2] in images else 1, "", "")
                if arguments[:2] == ["image", "tag"]:
                    images.add(arguments[3])
                if arguments[:2] == ["image", "rm"]:
                    images.discard(arguments[2])
                return subprocess.CompletedProcess(arguments, 0, "", "")

            record = {"id": identifier, "ownsContainer": True}
            with patch.dict(g, container_directory=lambda _: root, docker=docker,
                            registry_resource_get=lambda key: resources.get(key),
                            registry_resource_set=lambda key, value: resources.__setitem__(key, g["offset_archive_encode"](value)),
                            registry_resource_remove=lambda key: resources.pop(key, None)):
                self.assertTrue(g["migrate_legacy_definition"](record))
                self.assertFalse(g["migrate_legacy_definition"](dict(record)))
            self.assertEqual((root / "BuildContext" / "Dockerfile").read_text(), "FROM debian:bookworm\n")
            self.assertTrue(legacy.is_symlink())
            self.assertEqual((legacy / "Dockerfile").read_text(), "FROM debian:bookworm\n")
            self.assertEqual(list(resources), [f"containers/{identifier}/configuration"])
            self.assertEqual(g["offset_archive_decode"](resources[f"containers/{identifier}/configuration"]), {"mounts": [{"id": "m"}]})
            self.assertEqual(images, {f"outershell/container-{identifier}:latest"})


if __name__ == "__main__":
    unittest.main()
