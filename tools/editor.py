"""Compiler-backed editor operations. All edits are returned to the client."""
from __future__ import annotations

import os
import json
import pathlib
import re
import subprocess
import tempfile

import packages


class EditorError(ValueError):
    def __init__(self, message: str, code: int = -32803):
        super().__init__(message)
        self.code = code


def line_bounds(text: str) -> list[tuple[int, int]]:
    result, start = [], 0
    for match in re.finditer(r"\r\n|\r|\n", text):
        result.append((start, match.start()))
        start = match.end()
    result.append((start, len(text)))
    return result


def units(text: str) -> int:
    return len(text.encode("utf-16-le")) // 2


def offset(text: str, position: dict) -> int:
    line, character = position["line"], position["character"]
    if (type(line) is not int or type(character) is not int or line < 0 or character < 0):
        raise ValueError("editor positions must be nonnegative integers")
    bounds = line_bounds(text)
    if line >= len(bounds):
        raise ValueError("editor line is outside the document")
    start, end = bounds[line]
    count = 0
    for index in range(start, end):
        if count == character:
            return index
        count += 2 if ord(text[index]) > 0xFFFF else 1
        if count > character:
            raise ValueError("editor position splits a UTF-16 surrogate pair")
    return end  # LSP positions beyond a line are clamped to its end.


def point(text: str, index: int) -> dict:
    for line, (start, end) in enumerate(line_bounds(text)):
        if start <= index <= end:
            return {"line": line, "character": units(text[start:index])}
    raise ValueError("offset is outside an editor line")


def native_offset(text: str, location: dict) -> int:
    bounds = line_bounds(text)
    line, column = location["line"] - 1, location["column"] - 1
    if not 0 <= line < len(bounds) or column < 0:
        raise ValueError("compiler source location is outside the document")
    start, end = bounds[line]
    if line == 0 and text.startswith("\ufeff"):
        start += 1  # The compiler ignores a leading UTF-8 BOM.
    data = text[start:end].encode("utf-8")
    if column > len(data):
        raise ValueError("compiler source column is outside the document")
    return start + len(data[:column].decode("utf-8"))


def apply_changes(text: str, changes: list[dict]) -> str:
    if not isinstance(changes, list):
        raise ValueError("contentChanges must be an array")
    for change in changes:
        replacement = change["text"]
        if not isinstance(replacement, str):
            raise ValueError("editor text must be a string")
        if "range" not in change:
            text = replacement
        else:
            start, end = (offset(text, change["range"][key]) for key in ("start", "end"))
            if end < start:
                raise ValueError("editor range ends before its start")
            if "rangeLength" in change:
                length = change["rangeLength"]
                if type(length) is not int or length < 0 or length != units(text[start:end]):
                    raise ValueError("editor rangeLength does not match its UTF-16 range")
            text = text[:start] + replacement + text[end:]
        if len(text.encode("utf-8")) > 4 * 1024 * 1024:
            raise ValueError("editor source exceeds 4 MiB")
    return text


def write_overlays(path: pathlib.Path, sources: dict[pathlib.Path, str]) -> None:
    if len(sources) > 256:
        raise ValueError("editor overlay limit is 256 files")
    total = 0
    with path.open("wb") as out:
        out.write(b"PIKAFLOW-OVERLAY-1\n")
        for file, text in sorted(sources.items(), key=lambda item: str(item[0])):
            name = str(file.resolve()).encode("utf-8")
            content = text.replace("\r\n", "\n").replace("\r", "\n").encode("utf-8")
            total += len(content)
            if not 0 < len(name) <= 65536 or len(content) > 4 * 1024 * 1024 or total > 128 * 1024 * 1024:
                raise ValueError("editor overlay limits exceeded")
            out.write(f"{len(name)} {len(content)}\n".encode("ascii"))
            out.write(name + content + b"\n")


class Editor:
    def __init__(self, server):
        self.server = server
        self.roots: list[pathlib.Path] = []

    def roots_for(self, path: pathlib.Path) -> list[pathlib.Path]:
        if self.roots:
            return self.roots
        try:
            return [packages.project_root(path.parent)]
        except ValueError:
            for directory in (path.parent, *path.parent.parents):
                if (directory / ".git").exists():
                    return [directory]
            return [path.parent]

    def workspace_sources(self, path: pathlib.Path) -> list[pathlib.Path]:
        found = {path} if self.editable(path, path) else set()
        for root in self.roots_for(path):
            for directory, subdirs, files in os.walk(root, followlinks=False):
                parent = pathlib.Path(directory)
                subdirs[:] = sorted(name for name in subdirs if not name.startswith(".")
                    and name.lower() not in ("build", "out", "node_modules")
                    and not packages.linked(parent / name))
                for name in sorted(files):
                    file = parent / name
                    if file.suffix.lower() == ".pf" and not packages.linked(file) and self.editable(file.resolve(), path):
                        found.add(file.resolve())
                if len(found) > 128:
                    raise EditorError("workspace index exceeds 128 sources; open a smaller project folder")
        for opened in self.server.documents:
            if self.editable(opened, path) and not any(part.startswith(".") or part.lower() in
                    ("build", "out", "node_modules") for base in self.roots_for(path)
                    if opened.is_relative_to(base) for part in opened.relative_to(base).parts[:-1]):
                found.add(opened)
        if len(found) > 128:
            raise EditorError("workspace index exceeds 128 sources")
        return sorted(found)

    def groups(self, path):
        groups = {}
        for file in self.workspace_sources(path):
            try:
                project = packages.project_root(file.parent)
            except ValueError:
                project = None
            scope = project or max((base for base in self.roots_for(path) if file.is_relative_to(base)),
                                   key=lambda base: len(base.parts))
            groups.setdefault((project, scope), []).append(file)
        return groups

    def primary(self, path, project, files):
        if project is None:
            return path if path in files else files[0]
        primary = (project / packages.manifest(project).get("entry", "src/main.pf")).resolve()
        if not primary.is_relative_to(project):
            raise EditorError("project entry must remain inside its project")
        return primary

    def compile(self, command: str, path: pathlib.Path, *, index_sources=(), overlays=None):
        if command == "check" and not self.editable(path, path):
            failures, reachable = [], False
            for (project, _), files in self.groups(path).items():
                try:
                    primary = self.primary(path, project, files)
                    result = self.compile("symbols", primary, index_sources=files, overlays=overlays)
                except (ValueError, OSError, subprocess.SubprocessError) as error:
                    failures.append(str(error))
                    continue
                if result.returncode:
                    failures.append(result.stderr)
                else:
                    value = json.loads(result.stdout)
                    reachable |= any(pathlib.Path(file) == path for file in value.get("sources", []))
            if not reachable and not failures:
                failures.append("document is not reachable from a workspace project")
            return subprocess.CompletedProcess([str(self.server.compiler), command, str(path)],
                1 if failures else 0, "", "\n".join(failures))
        try:
            project = packages.project_root(path.parent)
        except ValueError:
            project = None
        if project is not None:
            packages.lock(project, verify=True)  # Offline; the editor never fetches dependencies.
        sources = {file: document["text"] for file, document in self.server.documents.items()}
        sources.update(overlays or {})
        with tempfile.TemporaryDirectory(prefix="pikaflow-overlay-") as directory:
            manifest = pathlib.Path(directory) / "sources.bin"
            write_overlays(manifest, sources)
            arguments = [str(self.server.compiler), command, str(path), "--overlay", str(manifest)]
            if command == "check":
                arguments.append("--library")
            for extra in index_sources:
                if extra != path:
                    arguments.extend(("--index-source", str(extra)))
            return subprocess.run(arguments, capture_output=True, text=True, encoding="utf-8",
                                  errors="replace", timeout=30)

    def index(self, path: pathlib.Path, *, strict=False, overlays=None) -> dict:
        symbols, sources = {}, set()
        failures = []
        for (project, _), files in self.groups(path).items():
            try:
                primary = self.primary(path, project, files)
                result = self.compile("symbols", primary, index_sources=files, overlays=overlays)
                if result.returncode and not strict:
                    fallback = path if path in files else primary
                    result = self.compile("symbols", fallback, overlays=overlays)
            except (ValueError, OSError, subprocess.SubprocessError) as error:
                failures.append(str(error))
                continue
            if result.returncode:
                failures.append(result.stderr.strip())
                continue
            value = json.loads(result.stdout)
            if value.get("version") != 1 or not isinstance(value.get("symbols"), list):
                raise EditorError("incompatible compiler symbol index")
            sources.update(pathlib.Path(file) for file in value.get("sources", []))
            if len(sources) > 128:
                raise EditorError("workspace index exceeds 128 sources including dependencies")
            for item in value["symbols"]:
                if item["id"] not in symbols:
                    symbols[item["id"]] = dict(item, uses=list(item["uses"]))
                else:
                    known = symbols[item["id"]]
                    if any(known[key] != item[key] for key in ("name", "kind", "definition")):
                        raise EditorError("workspace projects disagree about a source binding")
                    seen = {(use["file"], use["line"], use["column"]) for use in known["uses"]}
                    known["uses"].extend(use for use in item["uses"] if
                        (use["file"], use["line"], use["column"]) not in seen)
        if (strict and failures) or path not in sources:
            raise EditorError("compiler could not index this workspace: " +
                              (failures[0] if failures else "document is not reachable from a workspace project"))
        return {"version": 1, "sources": sorted(str(file) for file in sources), "symbols": list(symbols.values())}

    def location(self, where: dict, name: str, sources=None) -> dict:
        path = pathlib.Path(where["file"])
        source = sources[path] if sources and path in sources else self.server.source(path)
        start = native_offset(source, where)
        if source[start:start + len(name)] != name:
            raise EditorError("source changed since compiler indexing", -32801)
        return {"uri": path.as_uri(), "range": {"start": point(source, start),
                "end": point(source, start + len(name))}}

    def selected(self, index: dict, path: pathlib.Path, position: dict) -> dict | None:
        source = self.server.source(path)
        selected = offset(source, position)
        for symbol in index["symbols"]:
            for where in (symbol["definition"], *symbol["uses"]):
                if pathlib.Path(where["file"]) != path:
                    continue
                start = native_offset(source, where)
                if start <= selected < start + len(symbol["name"]):
                    return symbol
        return None

    def editable(self, path: pathlib.Path, root: pathlib.Path) -> bool:
        return ".pika" not in [part.lower() for part in path.parts] and any(
            path.is_relative_to(base) for base in self.roots_for(root))

    def rename(self, path: pathlib.Path, symbol: dict, new_name: str, index: dict) -> dict:
        if not symbol.get("renameable", True):
            raise EditorError("rename is unavailable because some field references cannot be resolved; annotate receiver types")
        if not packages.NAME.fullmatch(new_name) or len(new_name) > 128:
            raise EditorError("rename requires an ASCII identifier of at most 128 characters")
        if symbol["name"] == "main":
            raise EditorError("the main entry point cannot be renamed")
        if new_name == symbol["name"]:
            return {"documentChanges": []} if self.server.document_changes else {"changes": {}}
        originals, edits, seen = {}, {}, set()
        for where in (symbol["definition"], *symbol["uses"]):
            file = pathlib.Path(where["file"])
            if not self.editable(file, path):
                raise EditorError("rename would edit a dependency outside the editable workspace")
            originals.setdefault(file, self.server.source(file))
            start = native_offset(originals[file], where)
            identity = (file, start)
            if identity in seen:
                continue
            seen.add(identity)
            if originals[file][start:start + len(symbol["name"])] != symbol["name"]:
                raise EditorError("source changed during rename", -32801)
            edits.setdefault(file, []).append((start, start + len(symbol["name"])))
        changed = {}
        for file, ranges in edits.items():
            text = originals[file]
            for start, end in sorted(ranges, reverse=True):
                text = text[:start] + new_name + text[end:]
            changed[file] = text
        after = self.index(path, strict=True, overlays=changed)

        def moved(file, old):
            return old + sum(len(new_name) - (end - start) for start, end in edits.get(file, []) if start < old)

        definition_file = pathlib.Path(symbol["definition"]["file"])
        definition = moved(definition_file, native_offset(originals[definition_file], symbol["definition"]))
        target = next((item for item in after["symbols"] if item["name"] == new_name
            and pathlib.Path(item["definition"]["file"]) == definition_file
            and native_offset(changed[definition_file], item["definition"]) == definition), None)
        if target is None:
            raise EditorError("rename changed the declaration's meaning")
        expected = {(file, moved(file, start)) for file, start in seen}
        actual = set()
        for where in (target["definition"], *target["uses"]):
            file = pathlib.Path(where["file"])
            text = changed[file] if file in changed else self.server.source(file)
            actual.add((file, native_offset(text, where)))
        if expected != actual:
            raise EditorError("rename would capture another binding or change symbol resolution")
        for file, text in originals.items():
            if self.server.source(file) != text:
                raise EditorError("source changed during rename", -32801)
        documents = []
        for file, ranges in sorted(edits.items()):
            documents.append({"textDocument": {"uri": file.as_uri(), "version":
                self.server.documents[file]["version"] if file in self.server.documents else None},
                "edits": [{"range": {"start": point(originals[file], start),
                    "end": point(originals[file], end)}, "newText": new_name}
                    for start, end in sorted(ranges, reverse=True)]})
        if self.server.document_changes:
            return {"documentChanges": documents}
        return {"changes": {document["textDocument"]["uri"]: document["edits"] for document in documents}}

    def request(self, method: str, params: dict, path: pathlib.Path):
        context = params.get("context", {})
        if method.endswith("references") and (not isinstance(context, dict) or
                type(context.get("includeDeclaration", False)) is not bool):
            raise ValueError("reference context must contain a boolean includeDeclaration")
        strict = method.endswith(("references", "rename", "prepareRename"))
        index = self.index(path, strict=strict)
        symbol = self.selected(index, path, params["position"])
        if symbol is None:
            return [] if method.endswith("references") else None
        declaration = self.location(symbol["definition"], symbol["name"])
        if method.endswith("hover"):
            return {"contents": {"kind": "plaintext", "value": symbol["kind"] + " " +
                symbol["name"] + "\n" + (symbol["detail"] or symbol["type"])}}
        if method.endswith("definition"):
            return declaration
        if method.endswith("references"):
            locations = [self.location(where, symbol["name"]) for where in symbol["uses"]]
            if context.get("includeDeclaration", False):
                locations.insert(0, declaration)
            unique = {(item["uri"], item["range"]["start"]["line"],
                       item["range"]["start"]["character"]): item for item in locations}
            return list(unique.values())
        if method.endswith("prepareRename"):
            if not symbol.get("renameable", True):
                raise EditorError("rename is unavailable because some field references cannot be resolved; annotate receiver types")
            if not self.editable(pathlib.Path(symbol["definition"]["file"]).resolve(), path) or symbol["name"] == "main":
                raise EditorError("this declaration cannot be renamed")
            source = self.server.source(path)
            cursor = offset(source, params["position"])
            for where in (symbol["definition"], *symbol["uses"]):
                if pathlib.Path(where["file"]).resolve() == path:
                    start = native_offset(source, where)
                    if start <= cursor < start + len(symbol["name"]):
                        return {"range": self.location(where, symbol["name"])["range"], "placeholder": symbol["name"]}
        return self.rename(path, symbol, params["newName"], index)
