"""Reproducible local-path and exact-commit Git packages, without install hooks."""
from __future__ import annotations

import hashlib
import json
import pathlib
import re
import os
import stat
import tempfile
import git_sources

NAME = re.compile(r"[A-Za-z_][A-Za-z_0-9]*\Z")
LOCK_HEADER = "pikaflow-lock-v1"


def linked(path: pathlib.Path) -> bool:
    try:
        info = path.lstat()
    except FileNotFoundError:
        return False
    # Windows junctions use the mount-point reparse tag rather than symlink mode.
    return stat.S_ISLNK(info.st_mode) or getattr(info, "st_reparse_tag", 0) == 0xA0000003


def package_source(root: pathlib.Path, path: pathlib.Path) -> pathlib.Path:
    path = path.resolve()
    try:
        relative = path.relative_to(root.resolve())
    except ValueError:
        raise ValueError("package import cannot escape its locked package") from None
    if path.suffix != ".pf":
        raise ValueError("package import must name a locked .pf source")
    if any(part.startswith(".") or part.lower() in ("build", "out") for part in relative.parts):
        raise ValueError("package import names an excluded, unlocked source")
    return path


def source_bytes(path: pathlib.Path, limit: int) -> bytes:
    with path.open("rb") as source:
        data = source.read(limit + 1)
    if len(data) > limit:
        raise ValueError(f"package source exceeds {limit // (1024 * 1024)} MiB: {path}")
    return data


def project_root(start: pathlib.Path) -> pathlib.Path:
    start = start.resolve()
    for path in (start, *start.parents):
        if (path / "pika.json").is_file():
            return path
    raise ValueError("no pika.json found; run init first")


def manifest(root: pathlib.Path) -> dict:
    path = root / "pika.json"
    data = json.loads(source_bytes(path, 1024 * 1024).decode("utf-8"))
    if (not isinstance(data, dict) or not isinstance(data.get("name"), str)
            or not NAME.fullmatch(data["name"])):
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
    entries = 1
    while pending:
        directory = pending.pop()
        for path in directory.iterdir():
            if path.name.startswith(".") or path.name.lower() in ("build", "out"):
                continue
            if linked(path):
                raise ValueError(f"package symlinks are not supported: {path}")
            if path.is_dir():
                pending.append(path)
                entries += 1
            elif path.suffix.lower() == ".pf":
                files.append(path)
                entries += 1
            if entries > 10000:
                raise ValueError("package exceeds 10,000 source files/directories")
    for path in sorted(files, key=lambda item: item.relative_to(root).as_posix()):
        data = source_bytes(path, 16 * 1024 * 1024).replace(b"\r\n", b"\n").replace(b"\r", b"\n")
        name = path.relative_to(root).as_posix().encode("utf-8")
        digest.update(len(name).to_bytes(8, "big") + name)
        digest.update(len(data).to_bytes(8, "big") + data)
    return digest.hexdigest()


def read_lock(root: pathlib.Path) -> dict[str, tuple[str, str]]:
    path = root / "pika.lock"
    if linked(path):
        raise ValueError("package lock cannot be a symbolic link")
    if not path.is_file():
        raise ValueError("package lock is missing or stale; run deps lock")
    lines = source_bytes(path, 1024 * 1024).decode("utf-8").splitlines()
    if not lines or lines[0] != LOCK_HEADER:
        raise ValueError("unsupported package lock format")
    entries = {}
    for line in lines[1:]:
        fields = line.split("\t")
        if (len(fields) != 3 or not NAME.fullmatch(fields[0]) or not fields[1]
                or "\0" in line or not re.fullmatch(r"[0-9a-f]{64}", fields[2]) or fields[0] in entries):
            raise ValueError("invalid or duplicate package lock entry")
        entries[fields[0]] = (fields[1], fields[2])
    return entries


def resolve(root: pathlib.Path, *, fetch: bool = False, locked=None) -> list[tuple[str, pathlib.Path, str]]:
    root = root.resolve()
    found: dict[str, pathlib.Path] = {}
    active: set[pathlib.Path] = set()
    visited: set[pathlib.Path] = set()

    def visit(directory: pathlib.Path, depth: int, original=None, boundary=None) -> None:
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
            if not isinstance(specification, dict):
                raise ValueError(f"dependency {alias} requires a local path or pinned Git source")
            if set(specification) == {"path"}:
                value = specification["path"]
                if not isinstance(value, str) or not value or "\0" in value:
                    raise ValueError(f"dependency {alias} path must be a string")
                dependency = (directory / value).resolve()
                if boundary is not None and not dependency.is_relative_to(boundary):
                    raise ValueError("path dependencies inside Git sources cannot escape their pinned repository")
                next_original = (dependency if boundary is None else (original / value).absolute()) if original is not None else None
                next_boundary = boundary
            elif set(specification) == {"git", "rev"}:
                revision = specification["rev"]
                if not isinstance(revision, str) or not re.fullmatch(r"[0-9a-fA-F]{40}|[0-9a-fA-F]{64}", revision):
                    raise ValueError("Git dependencies require an exact full 40- or 64-hex commit")
                revision = revision.lower()
                if original is None and not str(specification["git"]).lower().startswith("https://"):
                    raise ValueError("Git dependencies inside HTTPS sources must use HTTPS origins")
                identity, local, address = git_sources.origin(root, original or directory, specification["git"])
                dependency = git_sources.cache_path(root, identity, revision)
                if locked is not None and (alias not in locked or locked[alias][0] != git_sources.relative(dependency, root)):
                    raise ValueError("Git origin or revision is not locked; run deps lock explicitly")
                dependency = git_sources.materialize(root, identity, local, address, revision, fetch)
                next_original, next_boundary = local, dependency
            else:
                raise ValueError(f"dependency {alias} requires exactly path or git plus rev")
            if any(char in str(dependency) for char in "\t\r\n"):
                raise ValueError("dependency paths cannot contain tab/newline characters")
            if alias in found and found[alias] != dependency:
                raise ValueError(f"conflicting dependency alias: {alias}")
            found[alias] = dependency
            if locked is not None:
                expected = (git_sources.relative(dependency, root), content_hash(dependency))
                if locked.get(alias) != expected:
                    raise ValueError("package lock is missing or stale; run deps lock")
            visit(dependency, depth + 1, next_original, next_boundary)
        active.remove(directory)
        visited.add(directory)

    visit(root, 0, root)
    return [(alias, path, content_hash(path)) for alias, path in sorted(found.items())]


def lock_text(root: pathlib.Path, *, fetch: bool = False, locked=None) -> str:
    lines = [LOCK_HEADER]
    for alias, path, digest in resolve(root, fetch=fetch, locked=locked):
        try:
            relative = pathlib.Path(os.path.relpath(path, root)).as_posix()
        except ValueError:
            relative = path.as_posix()  # Different Windows drive.
        lines.append(f"{alias}\t{relative}\t{digest}")
    return "\n".join(lines) + "\n"


def lock(root: pathlib.Path, *, verify: bool = False, fetch: bool = False, offline: bool = False) -> None:
    root = root.resolve()
    path = root / "pika.lock"
    if linked(path):
        raise ValueError("package lock cannot be a symbolic link")
    locked = read_lock(root) if verify else None
    expected = lock_text(root, fetch=(fetch or not verify) and not offline, locked=locked)
    if verify:
        if not path.exists() or path.read_text(encoding="utf-8") != expected:
            raise ValueError("package lock is missing or stale; run deps lock")
    else:
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n",
                                             dir=root, prefix=".pika-lock-", delete=False) as output:
                temporary = pathlib.Path(output.name)
                output.write(expected)
            if path.exists():
                os.chmod(temporary, path.stat().st_mode)
            os.replace(temporary, path)
            temporary = None
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)


def initialize(directory: pathlib.Path, name: str | None = None) -> None:
    directory = directory.resolve()
    name = name or re.sub(r"[^A-Za-z_0-9]", "_", directory.name)
    if not NAME.fullmatch(name):
        raise ValueError("package name must be an ASCII identifier")
    if any((directory / path).exists() or linked(directory / path)
           for path in ("pika.json", "pika.lock", "src/main.pf")):
        raise ValueError("init refuses to replace existing project files")
    if linked(directory / "src") or ((directory / "src").exists() and not (directory / "src").is_dir()):
        raise ValueError("init requires src to be a directory")
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "src").mkdir(exist_ok=True)
    (directory / "pika.json").write_text(json.dumps({"name": name, "version": "0.1.0",
        "entry": "src/main.pf", "dependencies": {}}, indent=2) + "\n", encoding="utf-8")
    (directory / "src/main.pf").write_text('fun main() -> unit:\n    print("Hello, PikaFlow!")\n',
                                            encoding="utf-8")
    lock(directory)
