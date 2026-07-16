#!/usr/bin/env python3
"""Create and deploy a canonical Outer Shell project from this template."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", required=True)
    parser.add_argument("--app-id", required=True)
    parser.add_argument("--scheme", required=True)
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--folder", required=True)
    parser.add_argument("--socket", required=True)
    parser.add_argument("--targets", required=True)
    parser.add_argument("--macos-language", choices=("swift", "objc"), required=True)
    parser.add_argument("--backend-language", choices=("go", "c"), required=True)
    parser.add_argument("--isolation", choices=("container", "host"), required=True)
    return parser.parse_args()


def remove(path: Path) -> None:
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path)
    elif path.exists() or path.is_symlink():
        path.unlink()


def canonical_source_root(value: str) -> Path:
    if value != "~" and not value.startswith("~/") and not value.startswith("/"):
        raise ValueError("source root must be an absolute path or start with ~/")
    if any(character in value for character in ('"', "\\", "$", "`", "\n", "\r")):
        raise ValueError("source root contains an unsupported shell path character")
    try:
        root = Path(value).expanduser()
    except RuntimeError as error:
        raise ValueError(f"could not expand source root: {error}") from error
    if not root.is_absolute():
        raise ValueError("source root must resolve to an absolute path")
    return root.resolve(strict=False)


def move_if_present(source: Path, destination: Path) -> None:
    if source.exists():
        source.rename(destination)


def specialize_layout(project: Path, args: argparse.Namespace, targets: set[str]) -> None:
    swift = project / "frontend-swift"
    objc = project / "frontend-objc"
    if "macos" in targets:
        selected = swift if args.macos_language == "swift" else objc
        unselected = objc if args.macos_language == "swift" else swift
        selected.rename(project / "macos")
        remove(unselected)
    else:
        remove(swift)
        remove(objc)

    if "html" not in targets:
        remove(project / "html")

    selected_backend = project / f"backend-{args.backend_language}"
    unselected_backend = project / ("backend-c" if args.backend_language == "go" else "backend-go")
    selected_backend.rename(project / "server")
    remove(unselected_backend)

    go_dockerfile = project / "Dockerfile-go"
    c_dockerfile = project / "Dockerfile-c"
    if args.isolation == "container":
        selected_dockerfile = go_dockerfile if args.backend_language == "go" else c_dockerfile
        unselected_dockerfile = c_dockerfile if args.backend_language == "go" else go_dockerfile
        selected_dockerfile.rename(project / "Dockerfile")
        remove(unselected_dockerfile)
    else:
        remove(go_dockerfile)
        remove(c_dockerfile)
        remove(project / "deploy" / "container-entrypoint.sh")
        remove(project / "deploy" / "app-container.service.in")
        remove(project / "deploy" / "run-container.sh")
        remove(project / "deploy" / "web-providers")


def rename_macos_files(project: Path, scheme: str) -> None:
    macos = project / "macos"
    if not macos.exists():
        return
    old_project = macos / "HelloFullstack.xcodeproj"
    new_project = macos / f"{scheme}.xcodeproj"
    move_if_present(old_project, new_project)
    move_if_present(macos / "Frontend" / "HelloFullstackContent.swift",
                    macos / "Frontend" / f"{scheme}Content.swift")
    move_if_present(macos / "Frontend" / "HelloFullstackContent.h",
                    macos / "Frontend" / f"{scheme}Content.h")
    move_if_present(macos / "Frontend" / "HelloFullstackContent.m",
                    macos / "Frontend" / f"{scheme}Content.m")
    move_if_present(new_project / "xcshareddata" / "xcschemes" / "HelloFullstack.xcscheme",
                    new_project / "xcshareddata" / "xcschemes" / f"{scheme}.xcscheme")


def module_component(folder: str) -> str:
    value = re.sub(r"[^a-z0-9_.-]+", "-", folder.lower()).strip("-.")
    return value or "outerframe-app"


def patch_text(project: Path, args: argparse.Namespace, ordered_targets: list[str]) -> None:
    replacements = (
        ("com.example.HelloFullstackSocket", args.socket),
        ("com.example.HelloFullstack", args.app_id),
        ("HelloFullstackContent", f"{args.scheme}Content"),
        ("HelloFullstackHandler", f"{args.scheme}Handler"),
        ("HelloResponse", f"{args.scheme}HelloResponse"),
        ("HelloFullstackBackend", f"{args.scheme}Backend"),
        ("HelloFullstack", args.scheme),
        ("Hello World", args.name),
        ("Hello world", args.name),
        ("hellofullstack/server", f"{module_component(args.folder)}/server"),
        ('APP_TARGETS="html macos"', f'APP_TARGETS="{" ".join(ordered_targets)}"'),
        ('ISOLATION_MODE="container"', f'ISOLATION_MODE="{args.isolation}"'),
        ('BACKEND_LANGUAGE="go"', f'BACKEND_LANGUAGE="{args.backend_language}"'),
        ('MACOS_LANGUAGE="swift"', f'MACOS_LANGUAGE="{args.macos_language}"'),
    )
    for path in project.rglob("*"):
        if not path.is_file() or path.name == "app-icon.png":
            continue
        try:
            contents = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        updated = contents
        for old, new in replacements:
            updated = updated.replace(old, new)
        if updated != contents:
            path.write_text(updated, encoding="utf-8")


def install_default_icon(project: Path, template: Path) -> None:
    for candidate in (template.parent / "app-icon.png", template.parent.parent / "app-icon.png"):
        if candidate.is_file():
            shutil.copy2(candidate, project / "app-icon.png")
            if (project / "macos").is_dir():
                shutil.copy2(candidate, project / "macos" / "app-icon.png")
            return


def main() -> int:
    args = arguments()
    if re.fullmatch(r"[A-Za-z0-9_.-]+", args.folder) is None or args.folder in {".", ".."}:
        print("error: invalid canonical project folder", file=sys.stderr)
        return 2
    if any(character in args.name for character in ('"', "\\", "$", "`", "\n", "\r")):
        print("error: app name contains an unsupported shell configuration character", file=sys.stderr)
        return 2
    ordered_targets = args.targets.split()
    targets = set(ordered_targets)
    if not targets or not targets <= {"html", "macos"} or len(targets) != len(ordered_targets):
        print("error: targets must contain html and/or macos exactly once", file=sys.stderr)
        return 2

    try:
        projects_root = canonical_source_root(args.source_root)
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    template = Path(__file__).resolve().parent
    # Canonical source is deliberately user-visible and user-owned. Outer Shell's
    # private data directory is reserved for deployed runtime snapshots.
    project = projects_root / args.folder
    projects_root.mkdir(parents=True, exist_ok=True)
    if project.exists():
        print(f"error: a canonical project already exists at {project}", file=sys.stderr)
        return 17

    incoming = Path(tempfile.mkdtemp(prefix=f".{args.folder}.incoming.", dir=projects_root))
    try:
        shutil.copytree(template, incoming, dirs_exist_ok=True)
        remove(incoming / Path(__file__).name)
        specialize_layout(incoming, args, targets)
        rename_macos_files(incoming, args.scheme)
        patch_text(incoming, args, ordered_targets)
        install_default_icon(incoming, template)
        os.rename(incoming, project)
    except BaseException:
        remove(incoming)
        raise

    result = subprocess.run([str(project / "app"), "deploy"], cwd=project, check=False)
    if result.returncode != 0:
        print(f"error: the canonical project was created at {project}, but its first deployment failed", file=sys.stderr)
        return result.returncode
    print(f"Canonical project: {project}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
