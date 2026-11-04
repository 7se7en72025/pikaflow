"""Actual formatter/package/LSP subprocess regressions. No editor required."""
from __future__ import annotations

import io
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import formatter
import lsp
import packages

COMPILER = pathlib.Path(sys.argv.pop(1)).resolve()
TOOL = ROOT / "tools/pikaflow.py"


def run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(TOOL), "--compiler", str(COMPILER), *args],
                          text=True, encoding="utf-8", capture_output=True, timeout=45)


class ToolsTests(unittest.TestCase):
    def test_formatter_examples_keep_tokens_and_are_idempotent(self):
        for path in sorted((ROOT / "examples").rglob("*.pf")):
            with self.subTest(path=path.name):
                source = path.read_text(encoding="utf-8")
                formatted = formatter.checked_format(COMPILER, source)
                self.assertEqual(formatter.format_text(formatted), formatted)

    def test_formatter_comments_strings_exponents_and_check_mode(self):
        source = ('# before\nfun main( )->unit:\n'
                  '  let x=1e+3  # preserved\n'
                  '  print("# // 😀");print(x) // also preserved\n')
        expected = ('# before\nfun main() -> unit:\n'
                    '    let x = 1e+3  # preserved\n'
                    '    print("# // 😀"); print(x)  // also preserved\n')
        self.assertEqual(formatter.checked_format(COMPILER, source), expected)
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "format.pf"
            path.write_text(source, encoding="utf-8")
            self.assertEqual(run("fmt", str(path), "--check").returncode, 1)
            self.assertEqual(path.read_text(encoding="utf-8"), source)
            self.assertEqual(run("fmt", str(path)).returncode, 0)
            self.assertEqual(run("fmt", str(path), "--check").returncode, 0)
            self.assertEqual(path.read_text(encoding="utf-8"), expected)

    def test_formatter_invalid_input_never_replaced(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "bad.pf"
            source = 'fun main():\n    print("unterminated)\n'
            path.write_text(source, encoding="utf-8")
            self.assertNotEqual(run("fmt", str(path)).returncode, 0)
            self.assertEqual(path.read_text(encoding="utf-8"), source)

    def test_package_init_lock_import_staleness_and_no_overwrite(self):
        with tempfile.TemporaryDirectory(prefix="pika-包-") as directory:
            root = pathlib.Path(directory)
            app, library = root / "app", root / "library"
            self.assertEqual(run("init", str(app), "--name", "app").returncode, 0)
            packages.initialize(library, "library")
            (library / "lib.pf").write_text('export fun answer() -> i64:\n    return 42\n', encoding="utf-8")
            data = packages.manifest(app)
            data["dependencies"] = {"mathkit": {"path": "../library"}}
            (app / "pika.json").write_text(json.dumps(data), encoding="utf-8")
            (app / "src/main.pf").write_text('import "pkg:mathkit"\nfun main() -> unit:\n    print(answer())\n', encoding="utf-8")
            self.assertNotEqual(run("deps", "check", "--project", str(app)).returncode, 0)
            self.assertEqual(run("deps", "lock", "--project", str(app)).returncode, 0)
            first = (app / "pika.lock").read_bytes()
            self.assertEqual(run("deps", "lock", "--project", str(app)).returncode, 0)
            self.assertEqual((app / "pika.lock").read_bytes(), first)
            result = run("interpret", "--project", str(app))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, "42\n")
            version = subprocess.run([str(COMPILER), "--version"], capture_output=True, text=True, timeout=10)
            if "LLVM enabled" in version.stdout:
                executable = root / ("native-包.exe" if sys.platform == "win32" else "native-包")
                result = run("build", "--project", str(app), "-O2", "-o", str(executable))
                self.assertEqual(result.returncode, 0, result.stderr)
                native = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
                self.assertEqual((native.returncode, native.stdout), (0, "42\n"))
            (library / "lib.pf").write_text('export fun answer() -> i64:\n    return 43\n', encoding="utf-8")
            self.assertNotEqual(run("interpret", "--project", str(app)).returncode, 0)
            before = (app / "src/main.pf").read_bytes()
            self.assertNotEqual(run("init", str(app)).returncode, 0)
            self.assertEqual((app / "src/main.pf").read_bytes(), before)

    def test_package_cycles_conflicts_and_escape(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            paths = [root / name for name in ("a", "b", "c", "d")]
            for path in paths:
                packages.initialize(path)

            def depend(path, dependencies):
                data = packages.manifest(path)
                data["dependencies"] = {name: {"path": value} for name, value in dependencies.items()}
                (path / "pika.json").write_text(json.dumps(data), encoding="utf-8")

            a, b, c, d = paths
            depend(a, {"b": "../b"})
            depend(b, {"a": "../a"})
            with self.assertRaisesRegex(ValueError, "cyclic"):
                packages.lock(a)
            depend(b, {"same": "../c"})
            depend(a, {"b": "../b", "same": "../d"})
            with self.assertRaisesRegex(ValueError, "conflicting"):
                packages.lock(a)
            depend(a, {"b": "../b"})
            packages.lock(a)
            (a / "src/main.pf").write_text('import "pkg:b/../c/src/main.pf"\nfun main() -> unit:\n    pass\n', encoding="utf-8")
            result = run("check", "--project", str(a))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("escape", result.stderr)

    def test_package_manifest_boundary_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            packages.initialize(root, "valid")
            for key, value in (("entry", []), ("entry", ""), ("version", 42), ("dependencies", [])):
                data = {"name": "valid", key: value}
                (root / "pika.json").write_text(json.dumps(data), encoding="utf-8")
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    packages.manifest(root)

    def test_lsp_framing_limits(self):
        stream = io.BytesIO()
        lsp.write_message(stream, {"id": 1, "method": "echo", "params": {"text": "😀"}})
        stream.seek(0)
        self.assertEqual(lsp.read_message(stream)["params"]["text"], "😀")
        for payload in (b"Content-Length: 99999999\r\n\r\n",
                        b"Content-Length: 1\r\nContent-Length: 1\r\n\r\nx",
                        b"Content-Length: 10\r\n\r\n{}"):
            with self.subTest(payload=payload), self.assertRaises(ValueError):
                lsp.read_message(io.BytesIO(payload))

    def test_lsp_unsaved_buffers_utf16_and_editor_features(self):
        with tempfile.TemporaryDirectory(prefix="lsp-包-") as directory:
            path = pathlib.Path(directory) / "main.pf"
            saved = 'fun main() -> unit:\n    print(1)\n'
            path.write_text(saved, encoding="utf-8")
            uri = path.as_uri()
            broken = 'fun main() -> unit:\n    let x = "😀"; print(missing)\n'
            fixed = 'fun main()->unit:\n  print(42)\n'
            messages = [
                {"id": 0, "method": "textDocument/documentSymbol", "params": {"textDocument": {"uri": uri}}},
                {"id": 1, "method": "initialize", "params": {"capabilities": {}}},
                {"method": "initialized", "params": {}},
                {"method": "textDocument/didOpen", "params": {"textDocument": {
                    "uri": uri, "languageId": "pikaflow", "version": 1, "text": broken}}},
                {"method": "textDocument/didChange", "params": {"textDocument": {"uri": uri, "version": 2},
                    "contentChanges": [{"text": fixed}]}},
                {"id": 2, "method": "textDocument/formatting", "params": {"textDocument": {"uri": uri}}},
                {"id": 3, "method": "textDocument/documentSymbol", "params": {"textDocument": {"uri": uri}}},
                {"id": 4, "method": "textDocument/completion", "params": {"textDocument": {"uri": uri},
                    "position": {"line": 1, "character": 2}}},
                {"id": 5, "method": "unsupported", "params": {}},
                {"id": 6, "method": "shutdown"}, {"method": "exit"}]
            request = io.BytesIO()
            for message in messages:
                lsp.write_message(request, message)
            result = subprocess.run([sys.executable, str(TOOL), "--compiler", str(COMPILER), "lsp"],
                input=request.getvalue(), capture_output=True, timeout=45)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            stream = io.BytesIO(result.stdout)
            responses = []
            while (message := lsp.read_message(stream)) is not None:
                responses.append(message)
            by_id = {message["id"]: message for message in responses if "id" in message}
            self.assertEqual(by_id[0]["error"]["code"], -32002)
            self.assertEqual(by_id[1]["result"]["capabilities"]["positionEncoding"], "utf-16")
            self.assertEqual(by_id[5]["error"]["code"], -32601)
            self.assertEqual(by_id[2]["result"][0]["newText"], 'fun main() -> unit:\n    print(42)\n')
            self.assertEqual(by_id[3]["result"][0]["name"], "main")
            self.assertIn("main", {item["label"] for item in by_id[4]["result"]["items"]})
            diagnostics = [item["params"] for item in responses if item.get("method") == "textDocument/publishDiagnostics"]
            errors = diagnostics[0]["diagnostics"]
            self.assertTrue(errors, responses)
            expected = lsp.utf16_length(broken.splitlines()[1].split("missing")[0])
            self.assertEqual(errors[0]["range"]["start"], {"line": 1, "character": expected})
            self.assertEqual(diagnostics[-1]["diagnostics"], [])
            self.assertEqual(path.read_text(encoding="utf-8"), saved)

    def test_lsp_unsaved_import_graph(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            library, main = root / "library.pf", root / "main.pf"
            library.write_text('export fun value() -> i64:\n    return 1\n', encoding="utf-8")
            main.write_text('import "library.pf"\nfun main() -> unit:\n    print(value() + 1)\n', encoding="utf-8")
            server = lsp.Server(COMPILER, io.BytesIO())
            server.documents[library] = {"text": 'export fun value() -> bool:\n    return true\n', "version": 2}
            errors = server.diagnostics(main)
            self.assertTrue(errors)
            self.assertIn(main.as_uri(), errors)
            self.assertEqual(server.diagnostics(library), {})  # Libraries need no artificial source main.


if __name__ == "__main__":
    unittest.main()
