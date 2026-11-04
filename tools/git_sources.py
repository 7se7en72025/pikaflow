"""Pinned Git sources, materialized without checkout filters or executable hooks."""
from __future__ import annotations

import base64
import hashlib
import json
import os
import pathlib
import re
import stat
import subprocess
import tempfile
import urllib.parse

META = ".pika-source.json"
MAX_FILE = 16 * 1024 * 1024
MAX_TOTAL = 128 * 1024 * 1024


def linked(path):
    try:
        info = path.lstat()
    except FileNotFoundError:
        return False
    return stat.S_ISLNK(info.st_mode) or getattr(info, "st_reparse_tag", 0) == 0xA0000003


def relative(path, root):
    try:
        return pathlib.Path(os.path.relpath(path, root)).as_posix()
    except ValueError:
        return path.as_posix()


def origin(project, directory, value):
    if not isinstance(value, str) or not value or any(c in value for c in "\0\t\r\n"):
        raise ValueError("Git origin must be a nonempty HTTPS URL or local repository path")
    url = urllib.parse.urlsplit(value)
    if url.scheme.lower() == "https":
        if not url.hostname or url.username is not None or url.password is not None or url.query or url.fragment:
            raise ValueError("Git HTTPS origins cannot contain credentials, query strings or fragments")
        canonical = urllib.parse.urlunsplit(("https", url.netloc.lower(), url.path, "", ""))
        return canonical, None, canonical
    path = pathlib.Path(value)
    if ((url.scheme or ":" in value) and not (os.name == "nt" and path.drive)) or value.startswith(("-", "//", "\\\\")):
        raise ValueError("Git origins support HTTPS or explicit local repository paths only")
    if path.drive and not path.is_absolute():
        raise ValueError("local Git origins cannot use drive-relative paths")
    local = pathlib.Path(os.path.abspath(directory / path))
    return "local:" + relative(local, project), local, str(local)


def cache_path(project, identity, revision):
    key = hashlib.sha256((identity + "\0" + revision).encode("utf-8")).hexdigest()
    path = project / ".pika" / "deps" / key
    for item in (project / ".pika", project / ".pika/deps", path):
        if linked(item) or (item.exists() and not item.is_dir()):
            raise ValueError("dependency cache paths must be ordinary directories")
    return path


def _bytes(path, maximum=MAX_FILE):
    if linked(path) or not path.is_file():
        raise ValueError("Git dependency contains an invalid file or symbolic link")
    with path.open("rb") as file:
        data = file.read(maximum + 1)
    if len(data) > maximum:
        raise ValueError("Git dependency file exceeds its size limit")
    return data


def _oid(kind, data, revision):
    algorithm = hashlib.sha1 if len(revision) == 40 else hashlib.sha256
    return algorithm(kind + b" " + str(len(data)).encode("ascii") + b"\0" + data).hexdigest()


def _entries(entries, revision):
    if not isinstance(entries, list) or len(entries) > 10000:
        raise ValueError("Git dependency exceeds 10,000 files")
    paths = {}
    for entry in entries:
        if not isinstance(entry, list) or len(entry) != 3 or not all(isinstance(x, str) for x in entry):
            raise ValueError("invalid Git source metadata entry")
        mode, name, oid = entry
        parts = name.split("/")
        if (mode not in ("100644", "100755") or len(name) > 512 or len(parts) > 64
                or any(not part or part in (".", "..") or "\\" in part or ":" in part
                       or any(ord(char) < 32 or ord(char) == 127 for char in part)
                       or part.rstrip(" .") != part or part.lower() == ".git"
                       or re.fullmatch(r"(?i)(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?", part)
                       for part in parts) or name.casefold() == META
                or not re.fullmatch(r"[0-9a-f]{" + str(len(revision)) + "}", oid)):
            raise ValueError("Git dependencies cannot contain symlinks, submodules or unsafe portable paths")
        for index in range(1, len(parts) + 1):
            prefix = "/".join(parts[:index])
            known = paths.setdefault(prefix.casefold(), prefix)
            if known != prefix:
                raise ValueError("Git dependency paths collide on case-insensitive filesystems")
    if len({entry[1] for entry in entries}) != len(entries):
        raise ValueError("duplicate Git source path")
    return entries


def verify(path, identity, revision):
    metadata = json.loads(_bytes(path / META, 8 * 1024 * 1024).decode("utf-8"))
    if not isinstance(metadata, dict) or metadata.get("origin") != identity or metadata.get("rev") != revision:
        raise ValueError("Git cache origin or pinned revision does not match its manifest")
    try:
        commit = base64.b64decode(metadata["commit"], validate=True)
    except (KeyError, ValueError, TypeError):
        raise ValueError("invalid pinned Git commit metadata") from None
    if len(commit) > 1024 * 1024 or _oid(b"commit", commit, revision) != revision:
        raise ValueError("Git cache commit does not match its exact pinned revision")
    entries = _entries(metadata.get("entries"), revision)
    expected = {entry[1] for entry in entries}
    actual = set()
    pending = [path]
    count = 0
    while pending:
        for item in pending.pop().iterdir():
            count += 1
            if count > 20000 or linked(item):
                raise ValueError("Git cache contains symbolic links or too many entries")
            if item.is_dir():
                pending.append(item)
            elif item.relative_to(path).as_posix() != META:
                actual.add(item.relative_to(path).as_posix())
    if actual != expected:
        raise ValueError("Git cache file set differs from its pinned commit")
    tree = {}
    total = 0
    for mode, name, oid in entries:
        data = _bytes(path / name)
        total += len(data)
        if total > MAX_TOTAL or _oid(b"blob", data, revision) != oid:
            raise ValueError("Git cache content differs from its pinned commit")
        node = tree
        parts = name.split("/")
        for part in parts[:-1]:
            node = node.setdefault(part, {})
            if not isinstance(node, dict):
                raise ValueError("invalid Git tree path collision")
        if parts[-1] in node:
            raise ValueError("invalid Git tree path collision")
        node[parts[-1]] = (mode, oid)

    def tree_oid(node):
        output = bytearray()
        for name, item in sorted(node.items(), key=lambda pair: pair[0].encode("utf-8") + (b"/" if isinstance(pair[1], dict) else b"")):
            mode, oid = ("40000", tree_oid(item)) if isinstance(item, dict) else item
            output.extend(mode.encode("ascii") + b" " + name.encode("utf-8") + b"\0" + bytes.fromhex(oid))
        return _oid(b"tree", output, revision)

    if commit.split(b"\n", 1)[0] != b"tree " + tree_oid(tree).encode("ascii"):
        raise ValueError("Git cache tree does not belong to its pinned commit")


def materialize(project, identity, local, address, revision, allow_fetch):
    destination = cache_path(project, identity, revision)
    if destination.exists():
        verify(destination, identity, revision)
        return destination
    if not allow_fetch:
        raise ValueError("pinned Git cache is missing; run deps fetch --locked (or deps lock for a new dependency)")
    if local is not None:
        if not local.is_dir() or any(linked(part) for part in (local, *local.parents)):
            raise ValueError("local Git origin must be an existing repository without symlinked directories")
    git = None
    # Do not let Windows' implicit current-directory lookup run a project git.exe.
    for directory in os.environ.get("PATH", "").split(os.pathsep):
        directory = pathlib.Path(directory.strip('"'))
        if not directory.is_absolute():
            continue
        candidate = directory / ("git.exe" if os.name == "nt" else "git")
        if candidate.is_file() and os.access(candidate, os.X_OK) and not candidate.resolve().is_relative_to(project):
            git = str(candidate.resolve())
            break
    if not git:
        raise ValueError("Git is required to fetch a pinned dependency")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=destination.parent, prefix=".fetch-") as temporary:
        temporary = pathlib.Path(temporary)
        if not temporary.resolve().is_relative_to(destination.parent.resolve()):
            raise ValueError("dependency temporary directory escaped its project cache")
        repository, exported, hooks = temporary / "objects", temporary / "source", temporary / "hooks-off"
        exported.mkdir()
        hooks.mkdir()
        environment = {key: value for key, value in os.environ.items() if not key.upper().startswith("GIT_")}
        environment.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_SYSTEM=os.devnull, GIT_CONFIG_GLOBAL=os.devnull,
                           GIT_TERMINAL_PROMPT="0", GIT_ASKPASS=os.devnull, GCM_INTERACTIVE="Never",
                           GIT_ALLOW_PROTOCOL="https:file", GIT_NO_LAZY_FETCH="1", GIT_ATTR_NOSYSTEM="1")
        configuration = ["-c", "core.hooksPath=" + str(hooks), "-c", "core.attributesFile=" + os.devnull,
                         "-c", "credential.helper=", "-c", "protocol.allow=never", "-c", "protocol.file.allow=always",
                         "-c", "protocol.https.allow=always", "-c", "http.followRedirects=false",
                         "-c", "fetch.fsckObjects=true", "-c", "core.fsmonitor=false"]

        def run(*arguments, cwd=temporary, maximum=MAX_FILE):
            with tempfile.TemporaryFile() as output:
                try:
                    result = subprocess.run([git, *configuration, *arguments], cwd=cwd, env=environment,
                                            stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.DEVNULL, timeout=120)
                except (OSError, subprocess.SubprocessError):
                    raise ValueError("Git dependency operation failed or timed out; check origin availability and exact commit") from None
                if result.returncode:
                    raise ValueError("Git dependency operation failed; check origin availability and exact commit")
                output.seek(0)
                data = output.read(maximum + 1)
                if len(data) > maximum:
                    raise ValueError("Git dependency metadata exceeds its size limit")
                return data

        run("init", "--bare", "--object-format=" + ("sha1" if len(revision) == 40 else "sha256"), "--template=" + str(hooks), str(repository))
        run("fetch", "--quiet", "--no-tags", "--no-recurse-submodules", "--depth=1", address, revision, cwd=repository)
        fetched = run("rev-parse", "FETCH_HEAD^{commit}", cwd=repository).decode("ascii").strip()
        if fetched != revision:
            raise ValueError("Git origin did not provide the exact pinned commit")
        commit = run("cat-file", "commit", revision, cwd=repository, maximum=1024 * 1024)
        entries = []
        for record in run("ls-tree", "-r", "-z", "--full-tree", revision, cwd=repository).split(b"\0"):
            if not record:
                continue
            header, name = record.split(b"\t", 1)
            mode, kind, oid = header.decode("ascii").split()
            entries.append([mode, name.decode("utf-8"), oid])
        _entries(entries, revision)
        total = 0
        for mode, name, oid in entries:
            size = int(run("cat-file", "-s", oid, cwd=repository).decode("ascii"))
            total += size
            if size > MAX_FILE or total > MAX_TOTAL:
                raise ValueError("Git dependency exceeds its source size limits")
            path = exported / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(run("cat-file", "blob", oid, cwd=repository))
        (exported / META).write_text(json.dumps({"origin": identity, "rev": revision,
            "commit": base64.b64encode(commit).decode("ascii"), "entries": entries}, sort_keys=True), encoding="utf-8")
        verify(exported, identity, revision)
        if not temporary.resolve().is_relative_to(destination.parent.resolve()) or linked(destination.parent):
            raise ValueError("dependency cache escaped its project")
        if destination.exists():
            verify(destination, identity, revision)
        else:
            exported.rename(destination)
    return destination
