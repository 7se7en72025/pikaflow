"""A bounded stdio LSP 3.17 server backed by the real PikaFlow compiler."""
from __future__ import annotations

import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request

from formatter import checked_format, split_comment
import packages

LIMIT = 4 * 1024 * 1024
IMPORT = re.compile(r'^(\s*import\s+)("(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\')', re.MULTILINE)
DECLARATION = re.compile(r"^(\s*)(?:export\s+)?(?:(?:extern\s+)?(fun|word|enum)|(let|var))\s+([A-Za-z_]\w*)")
KEYWORDS = "fun word enum let var if elif else while for in return match case break continue unsafe import export true false Result i64 f64 bool str unit spawn join".split()


def path_from_uri(uri: str) -> pathlib.Path:
    parsed = urllib.parse.urlsplit(uri)
    if parsed.scheme != "file" or parsed.netloc not in ("", "localhost"):
        raise ValueError("only local file: document URIs are supported")
    return pathlib.Path(urllib.request.url2pathname(parsed.path)).resolve()


def utf16_length(text: str) -> int:
    return len(text.encode("utf-16-le")) // 2


def read_message(stream) -> dict | None:
    headers = {}
    total = 0
    while True:
        line = stream.readline(8193)
        if not line:
            return None
        total += len(line)
        if len(line) > 8192 or total > 16384:
            raise ValueError("LSP headers exceed limit")
        if line in (b"\r\n", b"\n"):
            break
        key, separator, value = line.decode("ascii").partition(":")
        if not separator or key.lower() in headers:
            raise ValueError("invalid or duplicate LSP header")
        headers[key.lower()] = value.strip()
    size = int(headers.get("content-length", "-1"))
    if not 0 <= size <= LIMIT:
        raise ValueError("invalid LSP Content-Length")
    charset = headers.get("content-type", "utf-8").lower()
    if "charset=" in charset and charset.split("charset=", 1)[1] not in ("utf-8", "utf8"):
        raise ValueError("LSP requires UTF-8")
    content = stream.read(size)
    if len(content) != size:
        raise ValueError("truncated LSP message")
    result = json.loads(content)
    if not isinstance(result, dict) or result.get("jsonrpc") != "2.0":
        raise ValueError("expected a JSON-RPC 2.0 object")
    return result


def write_message(stream, message: dict) -> None:
    payload = json.dumps({"jsonrpc": "2.0", **message}, ensure_ascii=False).encode("utf-8")
    stream.write(f"Content-Length: {len(payload)}\r\n\r\n".encode("ascii") + payload)
    stream.flush()


def symbols(source: str) -> list[dict]:
    found = []
    for number, raw in enumerate(source.splitlines()):
        line, _ = split_comment(raw)
        match = DECLARATION.match(line)
        if not match:
            continue
        name = match[4]
        start = utf16_length(line[:match.start(4)])
        selection = {"start": {"line": number, "character": start},
                     "end": {"line": number, "character": start + len(name)}}
        found.append({"name": name, "kind": {"fun": 12, "word": 12, "enum": 10}.get(match[2], 13),
                      "range": {"start": {"line": number, "character": 0},
                                "end": {"line": number, "character": utf16_length(line)}},
                      "selectionRange": selection, "detail": line.strip()})
    return found


def package_module(source: pathlib.Path, name: str) -> pathlib.Path:
    root = packages.project_root(source.parent)
    alias, slash, module = name.partition("/")
    if not packages.NAME.fullmatch(alias):
        raise ValueError("invalid package import name")
    lines = (root / "pika.lock").read_text(encoding="utf-8").splitlines()
    if not lines or lines[0] != packages.LOCK_HEADER:
        raise ValueError("invalid package lock")
    for line in lines[1:]:
        fields = line.split("\t")
        if len(fields) != 3:
            raise ValueError("invalid package lock entry")
        if fields[0] == alias:
            dependency = (root / fields[1]).resolve()
            relative = pathlib.Path(module if slash else "lib.pf")
            if relative.is_absolute() or ".." in relative.parts:
                raise ValueError("package import escapes package")
            if not relative.suffix:
                relative = relative.with_suffix(".pf")
            result = (dependency / relative).resolve()
            if not result.is_relative_to(dependency):
                raise ValueError("package import escapes package")
            return result
    raise ValueError(f"package dependency is not locked: {alias}")


class Server:
    def __init__(self, compiler: pathlib.Path, output):
        self.compiler = compiler
        self.output = output
        self.documents: dict[pathlib.Path, dict] = {}
        self.published: set[str] = set()
        self.initialized = False
        self.shutdown = False

    def notify(self, method: str, params: dict) -> None:
        write_message(self.output, {"method": method, "params": params})

    def source(self, path: pathlib.Path) -> str:
        text = self.documents[path]["text"] if path in self.documents else path.read_text(encoding="utf-8")
        if len(text.encode("utf-8")) > LIMIT:
            raise ValueError("editor source exceeds 4 MiB")
        return text

    def diagnostics(self, path: pathlib.Path) -> dict[str, list[dict]]:
        with tempfile.TemporaryDirectory(prefix="pikaflow-lsp-") as directory:
            mapping: dict[pathlib.Path, pathlib.Path] = {}

            def stage(original: pathlib.Path) -> pathlib.Path:
                original = original.resolve()
                if original in mapping:
                    return mapping[original]
                if len(mapping) >= 128:
                    raise ValueError("editor import graph exceeds 128 modules")
                temporary = pathlib.Path(directory) / f"module{len(mapping)}.pf"
                mapping[original] = temporary
                text = self.source(original)

                def replace(match: re.Match) -> str:
                    # Decode only the source language's normal quoted escapes.
                    import ast
                    name = ast.literal_eval(match[2])
                    if name.startswith("std:"):
                        return match[0]
                    dependency = (package_module(path, name[4:]) if name.startswith("pkg:")
                                  else (original.parent / name).resolve())
                    target = stage(dependency)
                    return match[1] + json.dumps(target.as_posix(), ensure_ascii=False)

                text = IMPORT.sub(replace, text)
                temporary.write_text(text, encoding="utf-8")
                return temporary

            staged = stage(path)
            result = subprocess.run([str(self.compiler), "check", str(staged), "--library"], capture_output=True,
                                    text=True, encoding="utf-8", errors="replace", timeout=30)
            diagnostics: dict[str, list[dict]] = {}
            reverse = {str(temporary): original for original, temporary in mapping.items()}
            for line in result.stderr.splitlines():
                match = re.match(r"^(.*?):(\d+):(\d+): error: (.*)$", line)
                if not match:
                    continue
                original = reverse.get(match[1], path)
                number, column = max(0, int(match[2]) - 1), max(0, int(match[3]) - 1)
                lines = self.source(original).splitlines()
                before = lines[number].encode("utf-8")[:column].decode("utf-8", errors="ignore") if number < len(lines) else ""
                character = utf16_length(before)
                message = match[4]
                for temporary, real in reverse.items():
                    message = message.replace(temporary, str(real))
                diagnostics.setdefault(original.as_uri(), []).append({"range": {
                    "start": {"line": number, "character": character},
                    "end": {"line": number, "character": character + 1}},
                    "severity": 1, "source": "pikaflow", "message": message})
            if result.returncode and not diagnostics:
                diagnostics[path.as_uri()] = [{"range": {"start": {"line": 0, "character": 0},
                    "end": {"line": 0, "character": 1}}, "severity": 1, "source": "pikaflow",
                    "message": result.stderr.strip() or "compiler failed without a diagnostic"}]
            return diagnostics

    def validate(self) -> None:
        diagnostics: dict[str, list[dict]] = {}
        for path in self.documents:
            try:
                for uri, items in self.diagnostics(path).items():
                    existing = diagnostics.setdefault(uri, [])
                    existing.extend(item for item in items if item not in existing)
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                diagnostics[path.as_uri()] = [{"range": {"start": {"line": 0, "character": 0},
                    "end": {"line": 0, "character": 1}}, "severity": 1, "source": "pikaflow",
                    "message": str(error)}]
        current = {path.as_uri() for path in self.documents} | set(diagnostics)
        for uri in sorted(current | self.published):
            params = {"uri": uri, "diagnostics": diagnostics.get(uri, [])}
            path = path_from_uri(uri)
            if path in self.documents:
                params["version"] = self.documents[path]["version"]
            self.notify("textDocument/publishDiagnostics", params)
        self.published = current

    def handle(self, message: dict):
        method, params = message.get("method"), message.get("params", {})
        if method == "initialize":
            if self.initialized:
                raise ValueError("server already initialized")
            self.initialized = True
            return {"capabilities": {"positionEncoding": "utf-16", "textDocumentSync": 1,
                "documentFormattingProvider": True, "documentSymbolProvider": True,
                "completionProvider": {"resolveProvider": False}},
                "serverInfo": {"name": "PikaFlow", "version": "0.2.0"}}
        if not self.initialized:
            return {"__error__": -32002, "message": "server not initialized"}
        if method == "shutdown":
            self.shutdown = True
            return None
        if self.shutdown:
            raise ValueError("server has shut down")
        if method in ("initialized", "$/cancelRequest", "$/setTrace", "workspace/didChangeConfiguration"):
            return None
        if method in ("textDocument/didOpen", "textDocument/didChange", "textDocument/didClose", "textDocument/didSave"):
            document = params["textDocument"]
            path = path_from_uri(document["uri"])
            if method.endswith("didClose"):
                self.documents.pop(path, None)
            elif method.endswith("didOpen"):
                if path not in self.documents and len(self.documents) >= 32:
                    raise ValueError("at most 32 editor documents may be open")
                if len(document["text"].encode("utf-8")) > LIMIT:
                    raise ValueError("editor source exceeds 4 MiB")
                self.documents[path] = {"text": document["text"], "version": document["version"]}
            elif method.endswith("didChange"):
                if path not in self.documents:
                    raise ValueError("document is not open")
                if document["version"] <= self.documents[path]["version"]:
                    return None
                changes = params["contentChanges"]
                if len(changes) != 1 or "range" in changes[0]:
                    raise ValueError("server requires full-document synchronization")
                if len(changes[0]["text"].encode("utf-8")) > LIMIT:
                    raise ValueError("editor source exceeds 4 MiB")
                self.documents[path] = {"text": changes[0]["text"], "version": document["version"]}
            self.validate()
            return None
        if method in ("textDocument/formatting", "textDocument/documentSymbol", "textDocument/completion"):
            path = path_from_uri(params["textDocument"]["uri"])
            source = self.source(path)
            if method.endswith("formatting"):
                formatted = checked_format(self.compiler, source)
                if source == formatted:
                    return []
                lines = source.split("\n")
                return [{"range": {"start": {"line": 0, "character": 0},
                    "end": {"line": len(lines) - 1, "character": utf16_length(lines[-1])}},
                    "newText": formatted}]
            declarations = symbols(source)
            if method.endswith("documentSymbol"):
                return declarations
            items = [{"label": name, "kind": 14} for name in KEYWORDS]
            items.extend({"label": item["name"], "kind": 3 if item["kind"] == 12 else 6,
                          "detail": item["detail"]} for item in declarations)
            return {"isIncomplete": False, "items": items}
        return {"__error__": -32601, "message": f"method not found: {method}"}


def serve(compiler: pathlib.Path) -> int:
    # Windows text streams would translate framing bytes; use binary streams.
    server = Server(compiler, sys.stdout.buffer)
    while True:
        try:
            message = read_message(sys.stdin.buffer)
        except (ValueError, UnicodeError) as error:
            write_message(sys.stdout.buffer, {"id": None, "error": {"code": -32700, "message": str(error)}})
            return 1
        if message is None:
            return 0 if server.shutdown else 1
        if message.get("method") == "exit":
            return 0 if server.shutdown else 1
        try:
            result = server.handle(message)
            if "id" in message:
                if isinstance(result, dict) and "__error__" in result:
                    write_message(sys.stdout.buffer, {"id": message["id"], "error": {
                        "code": result["__error__"], "message": result["message"]}})
                else:
                    write_message(sys.stdout.buffer, {"id": message["id"], "result": result})
        except (ValueError, KeyError, TypeError, OSError, subprocess.SubprocessError) as error:
            if "id" in message:
                write_message(sys.stdout.buffer, {"id": message["id"],
                    "error": {"code": -32602, "message": str(error)}})
            else:
                server.notify("window/logMessage", {"type": 1, "message": str(error)})
