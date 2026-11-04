"""Compiler-backed semantic decorations; no request writes user sources."""
from __future__ import annotations

import io
import json
import pathlib
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import editor
import editor_features as features
import lsp
import packages

COMPILER = pathlib.Path(sys.argv.pop(1)).resolve()


def decoded(result):
    data, tokens, line, character = result["data"], [], 0, 0
    assert len(data) % 5 == 0
    for index in range(0, len(data), 5):
        delta_line, delta_start, length, kind, modifiers = data[index:index+5]
        assert delta_line >= 0 and delta_start >= 0 and length > 0
        line += delta_line
        character = delta_start if delta_line else character + delta_start
        tokens.append((line, character, length, features.TOKEN_TYPES[kind], modifiers))
    return tokens


def whole(source):
    return {"range": {"start": {"line": 0, "character": 0}, "end": editor.point(source, len(source))}}


class EditorFeatures(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="pika semantic features \u03bb ")
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name).resolve()
        self.originals = {}
        self.server = lsp.Server(COMPILER, io.BytesIO())
        self.server.handle({"method": "initialize", "params": {"rootUri": self.root.as_uri()}})

    def tearDown(self):
        for path, content in self.originals.items():
            self.assertEqual(path.read_bytes(), content, f"feature request changed {path}")

    def write(self, name, source):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(source.encode("utf-8"))
        self.originals[path] = path.read_bytes()
        return path

    def names(self, path):
        source = self.server.source(path)
        result = []
        for line, character, length, kind, modifiers in decoded(features.semantic_tokens(self.server, path)):
            begin = editor.offset(source, {"line": line, "character": character})
            end = editor.offset(source, {"line": line, "character": character+length})
            result.append((source[begin:end], kind, modifiers, line, character))
        return result

    def test_resolved_records_fields_generics_modules_and_words(self):
        library = self.write("lib.pf", 'export record Cell[T]:\n    value:T\n'
            'export enum Choice:\n    Some(value:i64)\n    Other\n'
            'export fun identity[T](x:T)->T:\n    return x\n'
            'export word square(i64)->i64:\n    dup *\n')
        main = self.write("main.pf", 'import "lib.pf" as lib\n'
            'fun main()->unit:\n    let cell=lib.Cell::[i64](value:41)\n'
            '    let value=lib.identity(42)\n    print(cell.value)\n'
            '    let squared=stack(value)->i64:\n        lib.square\n    print(squared)\n'
            '    # cell value lib are comments\n    print("cell value lib")\n')
        tokens = self.names(main)
        for expected in (("lib","namespace",1), ("main","function",1),
                         ("cell","variable",1), ("Cell","struct",0),
                         ("value","property",0), ("identity","function",0), ("square","function",0)):
            self.assertTrue(any(item[:3] == expected for item in tokens), (expected,tokens))
        self.assertFalse(any(item[3] >= 8 for item in tokens), tokens)
        library_tokens = self.names(library)
        for expected in (("Cell","struct",1), ("T","typeParameter",1), ("value","property",1),
                         ("Choice","enum",1), ("Some","enumMember",1), ("x","parameter",1)):
            self.assertTrue(any(item[:3] == expected for item in library_tokens), expected)
        self.assertEqual(features.semantic_tokens(self.server, main), features.semantic_tokens(self.server, main))

    def test_inferred_locals_skip_annotations_and_unused_generic_placeholders(self):
        main = self.write("main.pf", 'record Cell[T]:\n    value:T\n'
            'fun unused[T](x:T)->T:\n    let copy=x\n    return copy\n'
            'fun main()->unit:\n    let cell=Cell::[i64](value:41)\n'
            '    var inferred=42\n    let annotated : i64=2\n'
            '    let function=fun(x:i64)->i64:\n        return x+1\n'
            '    print(function(inferred)+cell.value+annotated)\n')
        source = self.server.source(main)
        hints = features.inlay_hints(self.server, main, whole(source))
        labels = {item["label"] for item in hints}
        self.assertEqual(labels, {": Cell[i64]", ": i64", ": fn(i64)->i64"})
        self.assertEqual(len(hints), 3)
        self.assertTrue(all(item["kind"] == 1 and "textEdits" not in item for item in hints))
        for name in ("copy", "annotated"):
            position = editor.point(source, source.index(name)+len(name))
            self.assertNotIn(position, [item["position"] for item in hints])
        target = next(item for item in hints if item["label"] == ": i64")
        end = dict(target["position"], character=target["position"]["character"]+1)
        selected = features.inlay_hints(self.server, main, {"range": {"start": target["position"], "end": end}})
        self.assertEqual(selected, [target])
        self.assertEqual(features.inlay_hints(self.server, main, {"range": {"start": target["position"], "end": target["position"]}}), [])

    def test_utf16_bom_crlf_and_unsaved_type_changes(self):
        source = '\ufefffun main()->unit:\r\n    let first=42\r\n    let text="\U0001f600"; let after=first\r\n    print(after)\r\n'
        path = self.write("main.pf", source)
        for opened in (False, True):
            with self.subTest(opened=opened):
                if opened:
                    self.server.documents[path] = {"text": source, "version": 1}
                visible = self.server.source(path)
                token = next(item for item in self.names(path) if item[0] == "after" and item[2] == 1)
                start = editor.point(visible, visible.index("after"))
                self.assertEqual(token[3:], (start["line"], start["character"]))
                hint = next(item for item in features.inlay_hints(self.server, path, whole(visible))
                            if item["position"] == editor.point(visible, visible.index("after")+5))
                self.assertEqual(hint["label"], ": i64")
        updated = source.replace("first=42", "first=2.5")
        self.server.documents[path] = {"text": updated, "version": 2}
        hints = features.inlay_hints(self.server, path, whole(updated))
        self.assertEqual(sum(item["label"] == ": f64" for item in hints), 2)
        split = editor.point(updated, updated.index("\U0001f600"))
        split["character"] += 1
        with self.assertRaisesRegex(ValueError,"surrogate"):
            features.inlay_hints(self.server,path,{"range":{"start":split,"end":whole(updated)["range"]["end"]}})

    def test_new_unsaved_graph_and_readonly_cached_dependency(self):
        main, library = self.root/"main.pf", self.root/"newlib.pf"
        self.server.documents[main] = {"text": 'import "newlib.pf" as lib\nfun main():\n    let answer=lib.value()\n    print(answer)\n', "version": 1}
        self.server.documents[library] = {"text": 'export fun value()->i64:\n    return 42\n', "version": 1}
        self.assertIn(("lib","namespace",1), [item[:3] for item in self.names(main)])
        self.assertEqual(features.inlay_hints(self.server, main, whole(self.server.source(main)))[0]["label"], ": i64")
        self.assertFalse(main.exists())
        self.assertFalse(library.exists())
        self.server.documents.clear()
        app, dep = self.root/"app", self.root/"app/.pika/deps/local"
        packages.initialize(app,"app")
        packages.initialize(dep,"dep")
        dependency = self.write("app/.pika/deps/local/lib.pf", 'export fun answer()->i64:\n    let local=42\n    return local\n')
        manifest = packages.manifest(app)
        manifest["dependencies"] = {"dep": {"path": ".pika/deps/local"}}
        (app/"pika.json").write_text(json.dumps(manifest))
        self.write("app/src/main.pf", 'import "pkg:dep"\nfun main():\n    print(answer())\n')
        packages.lock(app)
        self.server.editor.roots = [app]
        self.assertFalse(self.server.editor.editable(dependency,dependency))
        self.assertIn(("answer","function",1), [item[:3] for item in self.names(dependency)])
        hints = features.inlay_hints(self.server, dependency, whole(self.server.source(dependency)))
        self.assertEqual([item["label"] for item in hints], [": i64"])

    def test_stdio_capabilities_overlay_requests_and_invalid_range_recovery(self):
        main = self.write("main.pf", 'fun main():\n    let value=42\n    print(value)\n')
        overlay = 'fun main():\n    let value=2.5\n    print(value)\n'
        document = {"uri":main.as_uri()}
        requests = io.BytesIO()
        for message in [
            {"id":1,"method":"initialize","params":{"rootUri":self.root.as_uri()}},
            {"method":"textDocument/didOpen","params":{"textDocument":{**document,"text":overlay,"version":1}}},
            {"id":2,"method":"textDocument/semanticTokens/full","params":{"textDocument":document}},
            {"id":3,"method":"textDocument/inlayHint","params":{"textDocument":document,**whole(overlay)}},
            {"id":4,"method":"textDocument/inlayHint","params":{"textDocument":document,"range":None}},
            {"id":5,"method":"textDocument/inlayHint","params":{"textDocument":document,**whole(overlay)}},
            {"id":6,"method":"shutdown"},{"method":"exit"},
        ]:
            lsp.write_message(requests,message)
        result = subprocess.run([sys.executable,str(ROOT/"tools/pikaflow.py"),"--compiler",str(COMPILER),"lsp"],
            input=requests.getvalue(),capture_output=True,timeout=30)
        self.assertEqual(result.returncode,0,result.stderr)
        stream, responses = io.BytesIO(result.stdout), {}
        while (message := lsp.read_message(stream)) is not None:
            if "id" in message:
                responses[message["id"]] = message
        capabilities = responses[1]["result"]["capabilities"]
        self.assertEqual(capabilities["semanticTokensProvider"]["legend"],
            {"tokenTypes":features.TOKEN_TYPES,"tokenModifiers":features.TOKEN_MODIFIERS})
        self.assertIs(capabilities["semanticTokensProvider"]["full"],True)
        self.assertIs(capabilities["inlayHintProvider"],True)
        self.assertTrue(decoded(responses[2]["result"]))
        self.assertEqual([item["label"] for item in responses[3]["result"]],[": f64"])
        self.assertEqual(responses[4]["error"]["code"],-32602)
        self.assertEqual(responses[3]["result"],responses[5]["result"])
        self.assertIsNone(responses[6]["result"])


class DecorationBounds(unittest.TestCase):
    def server(self, source, symbols):
        path = pathlib.Path(__file__).resolve().with_name("virtual.pf")
        index = {"sources": [str(path)], "symbols": symbols}
        return path, types.SimpleNamespace(source=lambda _: source,
            editor=types.SimpleNamespace(index=lambda *a,**kw: index))

    def symbol(self, path, name, kind="variable", column=5, **extra):
        return {"id": name, "name": name, "kind": kind, "type": "i64", "detail": "",
                "definition": {"file": str(path), "line": 1, "column": column}, "uses": [], **extra}

    def test_dedup_unknown_mismatch_and_type_alias_mapping(self):
        path, _ = self.server("let value=42\n", [])
        value = self.symbol(path,"value")
        value["uses"] = [value["definition"],value["definition"]]
        _, server = self.server("let value=42\n", [value, self.symbol(path,"missing"), self.symbol(path,"let","keyword",1)])
        tokens = decoded(features.semantic_tokens(server,path))
        self.assertEqual(tokens, [(0,4,5,"variable",1)])
        _, alias = self.server("type Name=i64\n", [self.symbol(path,"Name","typeAlias",6)])
        self.assertEqual(decoded(features.semantic_tokens(alias,path))[0][3], "type")
        _, prefix = self.server("let values=42\n", [value])
        self.assertEqual(features.semantic_tokens(prefix,path), {"data": []})

    def test_requested_ranges_limits_and_generic_types(self):
        path, _ = self.server("let value=42\n", [])
        _, server = self.server("let value=42\n", [self.symbol(path,"value")])
        for params in (None, {}, {"range":None}, {"range":{"start":{},"end":{}}},
                {"range":{"start":{"line":True,"character":0},"end":{"line":0,"character":0}}},
                {"range":{"start":{"line":0,"character":5},"end":{"line":0,"character":4}}}):
            with self.subTest(params=params), self.assertRaises(ValueError):
                features.inlay_hints(server,path,params)
        with mock.patch.object(features,"MAX_TOKENS",0), self.assertRaises(editor.EditorError):
            features.semantic_tokens(server,path)
        with mock.patch.object(features,"MAX_HINTS",0), self.assertRaises(editor.EditorError):
            features.inlay_hints(server,path,whole(server.source(path)))
        for value in ("unit","?","<unknown>","T","[T]"):
            symbols = [self.symbol(path,"value",type=value), self.symbol(path,"T","typeParameter",99)]
            _, unknown = self.server("let value=42\n",symbols)
            self.assertEqual(features.inlay_hints(unknown,path,whole(unknown.source(path))), [])

    def test_ambiguous_classifications_and_source_changes_are_omitted(self):
        path, _ = self.server("let value=42\n",[])
        _, conflicting = self.server("let value=42\n",[self.symbol(path,"value"),self.symbol(path,"value","field")])
        self.assertEqual(features.semantic_tokens(conflicting,path),{"data":[]})
        _, changed = self.server("let value=42\n",[self.symbol(path,"value")])
        with mock.patch.object(changed,"source",side_effect=["let value=42\n","let value=2.5\n"]):
            self.assertEqual(features.semantic_tokens(changed,path),{"data":[]})
        with mock.patch.object(changed,"source",side_effect=["let value=42\n","let value=2.5\n"]):
            self.assertEqual(features.inlay_hints(changed,path,whole("let value=42\n")),[])


if __name__ == "__main__":
    unittest.main()
