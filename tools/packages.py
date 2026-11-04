"""Deterministic local-path packages. No implicit downloads or install scripts."""
from __future__ import annotations

import hashlib
import json
import pathlib
import re

NAME = re.compile(r"[A-Za-z_][A-Za-z_0-9]*\Z")
LOCK_HEADER = "pikaflow-lock-v1"


def project_root(start: pathlib.Path) -> pathlib.Path:
    start = start.resolve()
    for path in (start, *start.parents):
        if (path / "pika.json").is_file():
            return path
    raise ValueError("no pika.json found; run init first")


def manifest(root: pathlib.Path) -> dict:
    path = root / "pika.json"
    if path.stat().st_size > 1024 * 1024:
        raise ValueError("pika.json exceeds 1 MiB")
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or not NAME.fullmatch(str(data.get("name", ""))):
        raise ValueError(f"invalid package name in {path}")
    if not isinstance(data.get("dependencies", {}), dict):
        raise ValueError(f"dependencies must be an object in {path}")
    if "entry" in data and (not isinstance(data["entry"], str) or not data["entry"]
                            or "\0" in data["entry"]):
        raise ValueError(f"entry must be a nonempty path string in {path}")
    if "version" in data and not isinstance(data["version"], str):
        raise ValueError(f"version must be a string in {path}")
    return data


def content_hash(root: pathlib.Path) -> str:
    digest = hashlib.sha256()
    files = [root / "pika.json"]
    pending = [root]
    while pending:
        directory = pending.pop()
        for path in directory.iterdir():
            if path.name.startswith(".") or path.name in ("build", "out"):
                continue
            if path.is_symlink():
                raise ValueError(f"package symlinks are not supported: {path}")
            if path.is_dir():
                pending.append(path)
            elif path.suffix == ".pf":
                files.append(path)
            if len(pending) + len(files) > 10000:
                raise ValueError("package exceeds 10,000 source files/directories")
    for path in sorted(files, key=lambda item: item.relative_to(root).as_posix()):
        data = path.read_bytes()
        if len(data) > 16 * 1024 * 1024:
            raise ValueError(f"package source exceeds 16 MiB: {path}")
        name = path.relative_to(root).as_posix().encode("utf-8")
        digest.update(len(name).to_bytes(8, "big") + name)
        digest.update(len(data).to_bytes(8, "big") + data)
    return digest.hexdigest()


def resolve(root: pathlib.Path) -> list[tuple[str, pathlib.Path, str]]:
    found: dict[str, pathlib.Path] = {}
    active: set[pathlib.Path] = set()
    visited: set[pathlib.Path] = set()

    def visit(directory: pathlib.Path, depth: int) -> None:
        if directory in active:
            raise ValueError(f"cyclic package dependency: {directory}")
        if depth > 64:
            raise ValueError("package dependency nesting exceeds 64")
        if directory in visited:
            return
        active.add(directory)
        data = manifest(directory)
        for alias, specification in sorted(data.get("dependencies", {}).items()):
            if not NAME.fullmatch(alias):
                raise ValueError(f"invalid dependency name: {alias}")
            if not isinstance(specification, dict) or set(specification) != {"path"}:
                raise ValueError(f"dependency {alias} requires exactly a local path")
            if not isinstance(specification["path"], str) or not specification["path"] or "\0" in specification["path"]:
                raise ValueError(f"dependency {alias} path must be a string")
            dependency = (directory / specification["path"]).resolve()
            if any(char in str(dependency) for char in "\t\r\n"):
                raise ValueError("dependency paths cannot contain tab/newline characters")
            if alias in found and found[alias] != dependency:
                raise ValueError(f"conflicting dependency alias: {alias}")
            found[alias] = dependency
            visit(dependency, depth + 1)
        active.remove(directory)
        visited.add(directory)

    visit(root, 0)
    return [(alias, path, content_hash(path)) for alias, path in sorted(found.items())]


def lock_text(root: pathlib.Path) -> str:
    import os
    lines = [LOCK_HEADER]
    for alias, path, digest in resolve(root):
        try:
            relative = pathlib.Path(os.path.relpath(path, root)).as_posix()
        except ValueError:
            relative = path.as_posix()  # Different Windows drive.
        lines.append(f"{alias}\t{relative}\t{digest}")
    return "\n".join(lines) + "\n"


def lock(root: pathlib.Path, *, verify: bool = False) -> None:
    expected = lock_text(root)
    path = root / "pika.lock"
    if verify:
        if not path.exists() or path.read_text(encoding="utf-8") != expected:
            raise ValueError("package lock is missing or stale; run deps lock")
    else:
        path.write_text(expected, encoding="utf-8", newline="\n")


def initialize(directory: pathlib.Path, name: str | None = None) -> None:
    directory = directory.resolve()
    name = name or re.sub(r"[^A-Za-z_0-9]", "_", directory.name)
    if not NAME.fullmatch(name):
        raise ValueError("package name must be an ASCII identifier")
    if any((directory / path).exists() for path in ("pika.json", "pika.lock", "src/main.pf")):
        raise ValueError("init refuses to replace existing project files")
    if (directory / "src").exists() and not (directory / "src").is_dir():
        raise ValueError("init requires src to be a directory")
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "src").mkdir(exist_ok=True)
    (directory / "pika.json").write_text(json.dumps({"name": name, "version": "0.1.0",
        "entry": "src/main.pf", "dependencies": {}}, indent=2) + "\n", encoding="utf-8")
    (directory / "src/main.pf").write_text('fun main() -> unit:\n    print("Hello, PikaFlow!")\n',
                                            encoding="utf-8")
    lock(directory)
