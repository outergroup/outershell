#!/usr/bin/env python3
import os
from pathlib import Path
import runpy
import tempfile
import unittest
from unittest.mock import patch

provider = runpy.run_path(str(Path(__file__).resolve().parents[1] / "Resources/outershell-container-provider"))
g = provider["build_context"].__globals__


class RecipeLinkTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.outside = self.root / "outside"
        self.outside.mkdir()
        (self.outside / "secret").write_text("host secret\n")
        self.recipe = self.root / "Recipe"
        self.recipe.mkdir()
        (self.recipe / "Dockerfile").write_text("FROM debian:bookworm\n")
        (self.recipe / "kept").write_text("kept\n")
        self.patches = patch.dict(g, recipe_directory=lambda _: self.recipe,
                                  dockerfile_path=lambda _: self.recipe / "Dockerfile",
                                  runtime_directory=lambda _: self.root / "Runtime")
        self.patches.start()

    def tearDown(self):
        self.patches.stop()
        self.temporary.cleanup()

    def test_build_context_keeps_links_as_links(self):
        os.symlink("../outside/secret", self.recipe / "relative")
        os.symlink(self.outside / "secret", self.recipe / "absolute")
        os.symlink(self.outside, self.recipe / "folder")
        context = g["build_context"]("qa")
        for name in ("relative", "absolute", "folder"):
            self.assertTrue((context / name).is_symlink(), name)
        self.assertEqual(os.readlink(context / "relative"), "../outside/secret")
        self.assertEqual((context / "kept").read_text(), "kept\n")

    def test_build_context_rejects_special_files(self):
        os.mkfifo(self.recipe / "pipe")
        with self.assertRaisesRegex(Exception, "special file"):
            g["build_context"]("qa")

    def test_linked_dockerfile_is_neither_read_nor_written_through(self):
        (self.recipe / "Dockerfile").unlink()
        os.symlink(self.outside / "secret", self.recipe / "Dockerfile")
        with self.assertRaisesRegex(RuntimeError, "symbolic link"):
            g["read_dockerfile"]("qa")
        g["write_dockerfile"]("qa", "FROM alpine\n")
        self.assertFalse((self.recipe / "Dockerfile").is_symlink())
        self.assertEqual((self.recipe / "Dockerfile").read_text(), "FROM alpine\n")
        self.assertEqual((self.outside / "secret").read_text(), "host secret\n")
        self.assertEqual(sorted(path.name for path in self.recipe.iterdir()), ["Dockerfile", "kept"])

    def test_shared_recipe_rejects_links(self):
        destination = self.root / "Shared"
        g["copy_recipe_tree"](self.recipe, destination)
        g["require_unlinked_tree"](destination)
        nested = self.recipe / "nested"
        nested.mkdir()
        os.symlink(self.outside / "secret", nested / "leak")
        destination = self.root / "Shared Again"
        g["copy_recipe_tree"](self.recipe, destination)
        with self.assertRaisesRegex(RuntimeError, "nested/leak"):
            g["require_unlinked_tree"](destination)
        self.assertTrue((destination / "nested" / "leak").is_symlink())


if __name__ == "__main__":
    unittest.main()
