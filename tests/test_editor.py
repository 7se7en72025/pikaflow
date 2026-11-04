"""Real compiler-overlay editor workflows; WorkspaceEdits never write user files."""
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
import editor
import lsp
import packages

COMPILER = pathlib.Path(sys.argv.pop(1)).resolve()


def write(path, source):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(source.encode("utf-8"))
    return path


class EditorWorkflows(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="pikaflow editor \u03bb ")
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name).resolve()
        self.server = lsp.Server(COMPILER, io.BytesIO())
        self.server.handle({"method": "initialize", "params": {"rootUri": self.root.as_uri(),
            "capabilities": {"workspace": {"workspaceEdit": {"documentChanges": True}}}}})

    def index_symbol(self, path, name, line=None):
        index = self.server.editor.index(path, strict=True)
        candidates = [item for item in index["symbols"] if item["name"] == name
                      and (line is None or item["definition"]["line"] == line)]
        self.assertEqual(len(candidates), 1, candidates)
        return index, candidates[0]

    def renamed(self, path, name, new, line=None):
        index, symbol = self.index_symbol(path, name, line)
        result = self.server.editor.rename(path, symbol, new, index)
        changed = {}
        for document in result["documentChanges"]:
            file = lsp.path_from_uri(document["textDocument"]["uri"])
            changed[file] = editor.apply_changes(self.server.source(file), [
                {"range": item["range"], "text": item["newText"]} for item in document["edits"]])
        return result, changed

    def test_utf16_crlf_changes_and_atomic_failures(self):
        text = "a\U0001f600b\r\nnext\r\n"
        self.assertEqual(editor.offset(text, {"line": 0, "character": 3}), 2)
        self.assertEqual(editor.point(text, 2), {"line": 0, "character": 3})
        self.assertEqual(editor.point(text, len(text)), {"line": 2, "character": 0})
        replacement = {"range": {"start": {"line": 0, "character": 1}, "end": {"line": 0, "character": 3}},
                       "rangeLength": 2, "text": "x"}
        self.assertEqual(editor.apply_changes(text, [replacement]), "axb\r\nnext\r\n")
        for change in (
            {**replacement, "rangeLength": True},
            {**replacement, "rangeLength": -1},
            {**replacement, "rangeLength": 1},
            {"range": {"start": {"line": 0, "character": 2}, "end": {"line": 0, "character": 3}}, "text": "bad"},
        ):
            with self.subTest(change=change), self.assertRaises(ValueError):
                editor.apply_changes(text, [change])
        path = self.root / "main.pf"
        self.server.documents[path] = {"text": text, "version": 1}
        with self.assertRaises(ValueError):
            self.server.handle({"method": "textDocument/didChange", "params": {
                "textDocument": {"uri": path.as_uri(), "version": 2},
                "contentChanges": [replacement, {"range": {"start": {"line": 99, "character": 0},
                    "end": {"line": 99, "character": 1}}, "text": "bad"}]}})
        self.assertEqual(self.server.documents[path], {"text": text, "version": 1})

    def test_initialize_validation_is_atomic(self):
        server = lsp.Server(COMPILER, io.BytesIO())
        with self.assertRaises(ValueError):
            server.handle({"method": "initialize", "params": {"rootUri": "https://invalid.example/project"}})
        self.assertFalse(server.initialized)
        self.assertTrue(server.handle({"method": "initialize", "params": {}})["capabilities"]["hoverProvider"])

    def test_protocol_capabilities_context_and_nested_json(self):
        main = write(self.root / "main.pf", 'fun main() -> unit:\n    let answer = 42\n    print(answer)\n')
        server = lsp.Server(COMPILER, io.BytesIO())
        server.handle({"method": "initialize", "params": {"rootUri": self.root.as_uri()}})
        params = {"position": {"line": 2, "character": 11}, "newName": "result"}
        result = server.editor.request("textDocument/rename", params, main)
        self.assertEqual(set(result), {"changes"})
        self.assertEqual(len(result["changes"][main.as_uri()]), 2)
        self.assertEqual(server.editor.request("textDocument/rename", {**params, "newName": "answer"}, main), {"changes": {}})
        for context in (None, [], {"includeDeclaration": 1}):
            with self.subTest(context=context), self.assertRaises(ValueError):
                server.editor.request("textDocument/references", {**params, "context": context}, main)
        for levels, accepted in ((127, True), (128, False)):
            content = b'{"jsonrpc":"2.0","params":' + b"[" * levels + b"0" + b"]" * levels + b"}"
            framed = f"Content-Length: {len(content)}\r\n\r\n".encode() + content
            if accepted:
                self.assertEqual(lsp.read_message(io.BytesIO(framed))["jsonrpc"], "2.0")
            else:
                with self.assertRaisesRegex(ValueError, "nesting"):
                    lsp.read_message(io.BytesIO(framed))
        literal = io.BytesIO()
        lsp.write_message(literal, {"method": "test", "params": {"text": '["\\' * 2000}})
        literal.seek(0)
        self.assertEqual(lsp.read_message(literal)["params"]["text"], '["\\' * 2000)
        content = b'{"jsonrpc":"2.0","params":' + b"[" * 2000 + b"0" + b"]" * 2000 + b"}"
        framed = f"Content-Length: {len(content)}\r\n\r\n".encode() + content
        with self.assertRaisesRegex(ValueError, "nesting"):
            lsp.read_message(io.BytesIO(framed))
        rejected = subprocess.run([sys.executable, str(ROOT / "tools/pikaflow.py"), "--compiler", str(COMPILER), "lsp"],
                                  input=framed, capture_output=True, timeout=15)
        self.assertEqual(rejected.returncode, 1)
        self.assertEqual(lsp.read_message(io.BytesIO(rejected.stdout))["error"]["code"], -32700)

    def test_workspace_rename_includes_closed_modules_without_touching_comments(self):
        library = write(self.root / "library.pf", 'export fun greet(n: i64) -> i64:\n    return n\n')
        main = write(self.root / "main.pf", 'import "library.pf"\nfun main() -> unit:\n'
                     '    # greet stays in this comment\n    print("greet"); print(greet(42))\n')
        closed = write(self.root / "closed.pf", 'import "library.pf"\nfun helper() -> i64:\n    return greet(1)\n')
        before = {file: file.read_bytes() for file in (main, library, closed)}
        self.server.documents[main] = {"text": self.server.source(main), "version": 7}
        result, changed = self.renamed(main, "greet", "welcome")
        self.assertEqual(set(changed), {main, library, closed})
        self.assertIn('# greet stays in this comment\n    print("greet"); print(welcome(42))', changed[main])
        self.assertIn("return welcome(1)", changed[closed])
        versions = {item["textDocument"]["uri"]: item["textDocument"]["version"] for item in result["documentChanges"]}
        self.assertEqual(versions[main.as_uri()], 7)
        self.assertIsNone(versions[closed.as_uri()])
        self.assertEqual({file: file.read_bytes() for file in before}, before)

    def test_captured_shadowed_bindings_and_capture_rejection(self):
        main = write(self.root / "main.pf", 'fun main() -> unit:\n    let value = 1\n'
                     '    fun read() -> i64:\n        return value\n    if true:\n'
                     '        let value = 2\n        print(value)\n    print(read()); print(value)\n')
        _, changed = self.renamed(main, "value", "outer", line=2)
        self.assertIn("return outer", changed[main])
        self.assertIn("let value = 2\n        print(value)", changed[main])
        self.assertIn("print(read()); print(outer)", changed[main])
        source = 'fun main() -> unit:\n    let source = 1\n    if true:\n        let target = 2\n        print(source)\n    print(source)\n'
        write(main, source)
        with self.assertRaisesRegex(editor.EditorError, "capture|resolution"):
            self.renamed(main, "source", "target")
        self.assertEqual(main.read_bytes(), source.encode())
        write(main, 'fun outer[T](x: T) -> fn(T) -> T:\n    fun inner(a: T) -> T:\n'
                    '        print(x)\n        return a\n    return inner\nfun main() -> unit:\n    pass\n')
        with self.assertRaisesRegex(editor.EditorError, "capture|resolution"):
            self.renamed(main, "a", "x", line=2)

    def test_generic_enum_and_type_parameter_renames_are_source_bindings(self):
        source = ('enum Option[T]:\n    Some(value: T)\n    None\n'
                  'fun identity[T](value: T) -> T:\n    return value\n'
                  'fun main() -> unit:\n    let value: Option[i64] = Some(identity::[i64](42))\n'
                  '    match value:\n        case Some(n):\n            print(n)\n        case None():\n            pass\n')
        main = write(self.root / "main.pf", source)
        _, changed = self.renamed(main, "identity", "echo")
        self.assertIn("fun echo[T]", changed[main])
        self.assertIn("Some(echo::[i64](42))", changed[main])
        _, changed = self.renamed(main, "Option", "Maybe")
        self.assertIn("enum Maybe[T]", changed[main])
        self.assertIn("value: Maybe[i64]", changed[main])
        _, changed = self.renamed(main, "T", "Element", line=1)
        self.assertIn("enum Option[Element]:\n    Some(value: Element)", changed[main])
        self.assertIn("fun identity[T](value: T) -> T:", changed[main])
        self.assertEqual(main.read_bytes(), source.encode())

    def test_namespace_alias_qualified_columns_and_references(self):
        library = write(self.root / "library.pf", 'export fun answer() -> i64:\n    return 42\n')
        main = write(self.root / "main.pf", 'import "library.pf" as maths\nfun main() -> unit:\n    print(maths.answer())\n')
        _, changed = self.renamed(main, "maths", "numbers")
        self.assertIn('import "library.pf" as numbers', changed[main])
        self.assertIn("numbers.answer()", changed[main])
        index, symbol = self.index_symbol(main, "answer")
        location = self.server.editor.location(symbol["uses"][0], "answer")
        expected = self.server.source(main).splitlines()[2].index("answer")
        self.assertEqual(location["range"]["start"], {"line": 2, "character": expected})
        response = self.server.editor.request("textDocument/definition", {"position": location["range"]["start"]}, main)
        self.assertEqual(response["uri"], library.as_uri())
        references = self.server.editor.request("textDocument/references", {"position": location["range"]["start"],
            "context": {"includeDeclaration": True}}, main)
        self.assertEqual(len(references), 2)

    def test_bom_crlf_and_non_bmp_columns_match_client_text(self):
        main = write(self.root / "main.pf", '\ufefffun main() -> unit:\r\n    let value = 42\r\n'
                     '    print("\U0001f600"); print(value)\r\n')
        before = main.read_bytes()
        _, symbol = self.index_symbol(main, "value")
        actual = self.server.editor.location(symbol["uses"][0], "value")
        source = self.server.source(main)
        line = editor.line_bounds(source)[2]
        prefix = source[line[0]:line[1]].split("value")[0]
        self.assertEqual(actual["range"]["start"], {"line": 2, "character": editor.units(prefix)})
        _, changed = self.renamed(main, "value", "result")
        self.assertIn('print("\U0001f600"); print(result)\r\n', changed[main])
        self.assertEqual(main.read_bytes(), before)

    def test_new_unsaved_import_graph_and_true_diagnostic_caret(self):
        main, library = self.root / "new.pf", self.root / "new_library.pf"
        self.server.documents[main] = {"text": 'import "new_library.pf"\nfun main() -> unit:\n    print(answer())\n', "version": 4}
        self.server.documents[library] = {"text": 'export fun answer() -> i64:\n    return 42\n', "version": 3}
        self.assertEqual(self.server.diagnostics(main), {})
        result, changed = self.renamed(main, "answer", "response")
        self.assertEqual(set(changed), {main, library})
        self.assertTrue(all(item["textDocument"]["version"] in (3, 4) for item in result["documentChanges"]))
        self.assertFalse(main.exists())
        self.assertFalse(library.exists())
        saved = write(self.root / "saved.pf", 'fun main() -> unit:\n    print(1)\n')
        text = 'fun main() -> unit:\r\n    print("\U0001f600"); print(never_defined)\r\n'
        self.server.documents[saved] = {"text": text, "version": 5}
        diagnosed = self.server.editor.compile("check", saved)
        self.assertIn('print("\U0001f600"); print(never_defined)', diagnosed.stderr)
        self.assertNotIn("print(1)", diagnosed.stderr)
        errors = self.server.diagnostics(saved)[saved.as_uri()]
        prefix = text.splitlines()[1].split("never_defined")[0]
        self.assertEqual(errors[0]["range"]["start"], {"line": 1, "character": editor.units(prefix)})

    def test_open_external_dependency_does_not_grant_edit_permission(self):
        app, dependency = self.root / "app", self.root / "dependency"
        packages.initialize(app, "app")
        packages.initialize(dependency, "dependency")
        library = write(dependency / "lib.pf", 'export fun answer() -> i64:\n    return 42\n')
        data = packages.manifest(app)
        data["dependencies"] = {"dep": {"path": "../dependency"}}
        (app / "pika.json").write_text(json.dumps(data))
        packages.lock(app)
        main = write(app / "src/main.pf", 'import "pkg:dep"\nfun main() -> unit:\n    print(answer())\n')
        self.server.editor.roots = [app]
        self.server.documents[library] = {"text": self.server.source(library), "version": 1}
        _, symbol = self.index_symbol(main, "answer")
        with self.assertRaisesRegex(editor.EditorError, "dependency outside"):
            self.server.editor.rename(main, symbol, "response", self.server.editor.index(main, strict=True))
        hover = self.server.editor.request("textDocument/hover", {"position": {"line": 0, "character": 12}}, library)
        self.assertIn("answer", hover["contents"]["value"])
        self.assertFalse(self.server.editor.editable(app / ".pika/deps/cache/lib.pf", main))

    def test_multiple_projects_merge_references_and_never_return_partial_rename(self):
        common = write(self.root / "shared.pf", 'export fun answer() -> i64:\n    return 42\n')
        projects = []
        for name in ("first", "second"):
            project = self.root / name
            packages.initialize(project, name)
            main = write(project / "src/main.pf", 'import "../../shared.pf"\nfun main() -> unit:\n    print(answer())\n')
            projects.append(main)
        index, symbol = self.index_symbol(projects[0], "answer")
        self.assertEqual(len(symbol["uses"]), 2)
        result, changed = self.renamed(projects[0], "answer", "response")
        self.assertEqual(set(changed), {common, *projects})
        write(projects[1], 'fun main() -> unit:\n    print(undefined)\n')
        with self.assertRaisesRegex(editor.EditorError, "could not index"):
            self.server.editor.index(projects[0], strict=True)
        hover = self.server.editor.request("textDocument/hover", {"position": {"line": 2, "character": 11}}, projects[0])
        self.assertIn("answer", hover["contents"]["value"])
        self.assertIn("answer", common.read_text())

    def test_cached_dependency_navigation_uses_workspace_lock_and_stays_readonly(self):
        app = self.root / "app"
        packages.initialize(app, "app")
        cached = app / ".pika/deps/cached"
        packages.initialize(cached, "cached")
        library = write(cached / "lib.pf", 'export fun answer() -> i64:\n    return 42\n')
        (cached / "pika.lock").unlink()  # Dependency navigation must use the application's lock.
        data = packages.manifest(app)
        data["dependencies"] = {"dep": {"path": ".pika/deps/cached"}}
        (app / "pika.json").write_text(json.dumps(data))
        packages.lock(app)
        main = write(app / "src/main.pf", 'import "pkg:dep"\nfun main() -> unit:\n    print(answer())\n')
        self.server.editor.roots = [app]
        self.server.documents[library] = {"text": self.server.source(library), "version": 1}
        self.assertEqual(self.server.diagnostics(library), {})
        hover = self.server.editor.request("textDocument/hover", {"position": {"line": 0, "character": 12}}, library)
        self.assertIn("answer", hover["contents"]["value"])
        with self.assertRaisesRegex(editor.EditorError, "dependency outside"):
            self.renamed(main, "answer", "response")
        self.server.documents[library] = {"text": 'export fun answer() -> i64:\n    return undefined\n', "version": 2}
        errors = self.server.diagnostics(library)[library.as_uri()]
        self.assertEqual(errors[0]["range"]["start"], {"line": 1, "character": 11})
        self.assertIn("answer", library.read_text())

    def test_workspace_source_cap_precedes_compiler_invocation(self):
        for index in range(129):
            write(self.root / f"source{index}.pf", "# source\n")
        with self.assertRaisesRegex(editor.EditorError, "128 sources"):
            self.server.editor.workspace_sources(self.root / "source0.pf")

    def test_overlay_rejects_malformed_records_and_caps(self):
        source = write(self.root / "main.pf", 'fun main() -> unit:\n    pass\n')
        overlay = self.root / "overlay.bin"
        encoded = str(source).encode("utf-8")
        record = f"{len(encoded)} 0\n".encode() + encoded + b"\n"
        malformed = [b"wrong-header\n", b"PIKAFLOW-OVERLAY-1\n1 2\nab",
                     b"PIKAFLOW-OVERLAY-1\n65537 0\n", b"PIKAFLOW-OVERLAY-1\n1 4194305\n",
                     b"PIKAFLOW-OVERLAY-1\n" + record + record,
                     b"PIKAFLOW-OVERLAY-1\n4 0\na.pf\n"]
        too_many = bytearray(b"PIKAFLOW-OVERLAY-1\n")
        for index in range(257):
            name = str(self.root / f"source{index}.pf").encode("utf-8")
            too_many.extend(f"{len(name)} 0\n".encode() + name + b"\n")
        malformed.append(bytes(too_many))
        for payload in malformed:
            overlay.write_bytes(payload)
            result = subprocess.run([str(COMPILER), "check", str(source), "--overlay", str(overlay)],
                                    capture_output=True, text=True, timeout=15)
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn("overlay", result.stderr)
        with self.assertRaises(ValueError):
            editor.write_overlays(overlay, {self.root / f"f{i}.pf": "" for i in range(257)})
        with self.assertRaises(ValueError):
            editor.write_overlays(overlay, {source: "x" * (4 * 1024 * 1024 + 1)})
        self.assertEqual(source.read_text(), 'fun main() -> unit:\n    pass\n')

    def test_real_lsp_hover_and_workspace_rename_subprocess(self):
        main = write(self.root / "main.pf", 'fun main() -> unit:\n    let answer = 42\n    print(answer)\n')
        messages = [
            {"id": 1, "method": "initialize", "params": {"rootUri": self.root.as_uri(),
                "capabilities": {"workspace": {"workspaceEdit": {"documentChanges": True}}}}},
            {"method": "textDocument/didOpen", "params": {"textDocument": {"uri": main.as_uri(),
                "languageId": "pikaflow", "version": 3, "text": main.read_text()}}},
            {"id": 2, "method": "textDocument/hover", "params": {"textDocument": {"uri": main.as_uri()}, "position": {"line": 2, "character": 11}}},
            {"id": 3, "method": "textDocument/prepareRename", "params": {"textDocument": {"uri": main.as_uri()}, "position": {"line": 2, "character": 11}}},
            {"id": 4, "method": "textDocument/rename", "params": {"textDocument": {"uri": main.as_uri()}, "position": {"line": 2, "character": 11}, "newName": "result"}},
            {"id": 5, "method": "shutdown"}, {"method": "exit"},
        ]
        stream = io.BytesIO()
        for message in messages:
            lsp.write_message(stream, message)
        result = subprocess.run([sys.executable, str(ROOT / "tools/pikaflow.py"), "--compiler", str(COMPILER), "lsp"],
                                input=stream.getvalue(), capture_output=True, timeout=45)
        self.assertEqual(result.returncode, 0, result.stderr)
        responses = {}
        stream = io.BytesIO(result.stdout)
        while (message := lsp.read_message(stream)) is not None:
            if "id" in message:
                responses[message["id"]] = message
        self.assertIn("i64", responses[2]["result"]["contents"]["value"])
        self.assertEqual(responses[3]["result"]["placeholder"], "answer")
        changes = responses[4]["result"]["documentChanges"]
        self.assertEqual(changes[0]["textDocument"]["version"], 3)
        self.assertEqual(len(changes[0]["edits"]), 2)
        self.assertIn("let answer", main.read_text())


if __name__ == "__main__":
    unittest.main()
