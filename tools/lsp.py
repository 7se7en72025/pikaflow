"""A bounded stdio LSP 3.17 server backed by the real PikaFlow compiler."""
from __future__ import annotations

import json
import pathlib
import re
import subprocess
import sys
import urllib.parse
import urllib.request

from formatter import checked_format, split_comment
from editor import Editor, apply_changes, point
import packages

LIMIT = 4 * 1024 * 1024
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
    depth, quoted, escaped = 0, False, False
    for byte in content:
        if quoted:
            if escaped:
                escaped = False
            elif byte == 92:
                escaped = True
            elif byte == 34:
                quoted = False
        elif byte == 34:
            quoted = True
        elif byte in (91, 123):
            depth += 1
            if depth > 128:
                raise ValueError("LSP JSON nesting exceeds 128 container levels")
        elif byte in (93, 125):
            depth -= 1
    try:
        result = json.loads(content)
    except RecursionError:
        raise ValueError("LSP JSON nesting exceeds the parser limit") from None
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


class Server:
    def __init__(self, compiler: pathlib.Path, output):
        self.compiler = compiler
        self.output = output
        self.documents: dict[pathlib.Path, dict] = {}
        self.published: set[str] = set()
        self.initialized = False
        self.shutdown = False
        self.document_changes = False
        self.editor = Editor(self)

    def notify(self, method: str, params: dict) -> None:
        write_message(self.output, {"method": method, "params": params})

    def source(self, path: pathlib.Path) -> str:
        if path in self.documents:
            text = self.documents[path]["text"]
        else:
            with path.open("rb") as source:
                data = source.read(LIMIT + 1)
            if len(data) > LIMIT:
                raise ValueError("editor source exceeds 4 MiB")
            text = data.decode("utf-8-sig")
        if len(text.encode("utf-8")) > LIMIT:
            raise ValueError("editor source exceeds 4 MiB")
        return text

    def diagnostics(self, path: pathlib.Path) -> dict[str, list[dict]]:
        result = self.editor.compile("check", path)
        diagnostics: dict[str, list[dict]] = {}
        for line in result.stderr.splitlines():
            match = re.match(r"^(.*?):(\d+):(\d+): error: (.*)$", line)
            if not match:
                continue
            original = pathlib.Path(match[1]).resolve()
            where = {"line": int(match[2]), "column": int(match[3])}
            from editor import native_offset
            source = self.source(original)
            start = point(source, native_offset(source, where))
            diagnostics.setdefault(original.as_uri(), []).append({"range": {
                "start": start, "end": {"line": start["line"], "character": start["character"] + 1}},
                "severity": 1, "source": "pikaflow", "message": match[4]})
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
        if not isinstance(params, dict):
            raise ValueError("LSP parameters must be an object")
        if method == "initialize":
            if self.initialized:
                raise ValueError("server already initialized")
            roots = params.get("workspaceFolders") or []
            if not roots and params.get("rootUri"):
                roots = [{"uri": params["rootUri"]}]
            capabilities = params.get("capabilities", {})
            if not isinstance(capabilities, dict):
                raise ValueError("client capabilities must be an object")
            workspace = capabilities.get("workspace", {})
            if not isinstance(workspace, dict):
                raise ValueError("workspace capabilities must be an object")
            edit = workspace.get("workspaceEdit", {})
            if not isinstance(edit, dict) or type(edit.get("documentChanges", False)) is not bool:
                raise ValueError("workspace edit capability must contain a boolean documentChanges")
            resolved_roots = list(dict.fromkeys(path_from_uri(item["uri"]) for item in roots))
            self.editor.roots = resolved_roots
            self.document_changes = edit.get("documentChanges", False)
            self.initialized = True
            return {"capabilities": {"positionEncoding": "utf-16", "textDocumentSync": {
                "openClose": True, "change": 2, "save": {"includeText": False}},
                "documentFormattingProvider": True, "documentSymbolProvider": True,
                "hoverProvider": True, "definitionProvider": True, "referencesProvider": True,
                "renameProvider": {"prepareProvider": True},
                "completionProvider": {"resolveProvider": False}},
                "serverInfo": {"name": "PikaFlow", "version": "0.3.0"}}
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
            if method.endswith(("didOpen", "didChange")) and type(document["version"]) is not int:
                raise ValueError("document version must be an integer")
            if method.endswith("didClose"):
                self.documents.pop(path, None)
            elif method.endswith("didOpen"):
                if path not in self.documents and len(self.documents) >= 32:
                    raise ValueError("at most 32 editor documents may be open")
                if not isinstance(document["text"], str):
                    raise ValueError("editor text must be a string")
                if len(document["text"].encode("utf-8")) > LIMIT:
                    raise ValueError("editor source exceeds 4 MiB")
                self.documents[path] = {"text": document["text"], "version": document["version"]}
            elif method.endswith("didChange"):
                if path not in self.documents:
                    raise ValueError("document is not open")
                if document["version"] <= self.documents[path]["version"]:
                    return None
                updated = apply_changes(self.documents[path]["text"], params["contentChanges"])
                self.documents[path] = {"text": updated, "version": document["version"]}
            self.validate()
            return None
        if method in ("textDocument/hover", "textDocument/definition", "textDocument/references",
                      "textDocument/prepareRename", "textDocument/rename"):
            return self.editor.request(method, params, path_from_uri(params["textDocument"]["uri"]))
        if method in ("textDocument/formatting", "textDocument/documentSymbol", "textDocument/completion"):
            path = path_from_uri(params["textDocument"]["uri"])
            source = self.source(path)
            if method.endswith("formatting"):
                formatted = checked_format(self.compiler, source)
                if source == formatted:
                    return []
                return [{"range": {"start": {"line": 0, "character": 0},
                    "end": point(source, len(source))},
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
                    "error": {"code": getattr(error, "code", -32602), "message": str(error)}})
            else:
                server.notify("window/logMessage", {"type": 1, "message": str(error)})
