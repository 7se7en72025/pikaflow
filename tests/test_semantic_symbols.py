"""Compiler-owned binding IDs, exact identifier ranges and qualified references."""
import json
import pathlib
import subprocess
import sys
import tempfile
import textwrap


def main():
    compiler = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="pikaflow semantic symbols ") as temporary:
        root = pathlib.Path(temporary)
        library = root / "library.pf"
        library.write_text(textwrap.dedent('''
            export enum Box[T]:
                Boxed(value: T)
            export fun identity[T](value: T) -> T:
                return value
            export word square(i64) -> i64:
                dup *
        ''').lstrip(), encoding="utf-8")
        source = root / "main.pf"
        source.write_text(textwrap.dedent('''
            import "library.pf" as lib
            fun unused[T](value: T) -> T:
                let copy = value
                return copy
            fun outer(value: i64) -> i64:
                var current = value
                fun bump() -> i64:
                    current += 1
                    return current
                fun inner(value: i64) -> i64:
                    return value
                if true:
                    let value = 100
                    print(value)
                return bump() + inner(value)
            fun main() -> unit:
                let function = lib.identity::[i64]
                let real = lib.identity(2.5)
                let boxed: lib.Box[i64] = lib.Boxed(function(21))
                match boxed:
                    case lib.Boxed(value) if value > 0:
                        print(value)
                    case _:
                        pass
                let value = outer(20)
                let text = "😀"; print(value)
                for value in range(1):
                    print(value)
                print(value)
                let squared = stack(6) -> i64:
                    lib.square
                print(squared)
        ''').lstrip(), encoding="utf-8")

        def symbols(path, *flags):
            result = subprocess.run([compiler, "symbols", str(path), *flags], capture_output=True,
                                    text=True, encoding="utf-8", timeout=30)
            assert result.returncode == 0, result.stderr
            return json.loads(result.stdout)

        data = symbols(source)
        records = data["symbols"]
        assert data["version"] == 1
        assert len({record["id"] for record in records}) == len(records)
        sources = {str(path.resolve()): path.read_bytes().splitlines() for path in (library, source)}
        # Every range must point to the actual identifier, never a keyword or alias prefix.
        for record in records:
            for location in [record["definition"], *record["uses"]]:
                lines = sources[str(pathlib.Path(location["file"]).resolve())]
                start = location["column"] - 1
                assert lines[location["line"] - 1][start:start + len(record["name"])] == record["name"].encode(), record
        assert not any(record["name"] == "arg0" for record in records), "anonymous word argument is not a named declaration"

        def find(name, kind=None, contains=None):
            matches = [record for record in records if record["name"] == name and (kind is None or record["kind"] == kind)]
            if contains:
                matches = [record for record in matches if contains in sources[str(pathlib.Path(record["definition"]["file"]).resolve())][record["definition"]["line"] - 1].decode()]
            assert len(matches) == 1, (name, kind, contains, matches)
            return matches[0]

        alias = find("lib", "module")
        assert len(alias["uses"]) == 6, alias
        assert len(find("Box", "enum")["uses"]) == 1
        assert len(find("Boxed", "constructor")["uses"]) == 2
        assert len(find("identity", "function")["uses"]) == 2
        assert len(find("square", "word")["uses"]) == 1
        assert len(find("current", "variable")["uses"]) == 2
        assert len(find("value", "parameter", "fun outer")["uses"]) == 2
        assert len(find("value", "parameter", "fun inner")["uses"]) == 1
        assert len(find("value", "variable", "let value = 100")["uses"]) == 1
        assert len(find("value", "variable", "case lib.Boxed")["uses"]) == 2
        assert len(find("value", "variable", "let value = outer")["uses"]) == 2
        assert len(find("value", "variable", "for value in")["uses"]) == 1
        assert find("current", "variable")["type"] == "i64"
        assert find("function", "variable")["type"] == "fn(i64)->i64"
        # Uninstantiated templates still have lexical binding/type references.
        assert len(find("value", "parameter", "fun unused")["uses"]) == 1
        assert find("copy", "variable")["detail"] == "generic declaration"
        assert len(find("copy", "variable")["uses"]) == 1
        type_parameters = [record for record in records if record["kind"] == "typeParameter"]
        assert len(type_parameters) == 3 and all(record["uses"] for record in type_parameters)
        library_only = symbols(library, "--library")
        assert not any(record["name"] == "main" for record in library_only["symbols"]), "synthetic library entry must not be indexed"
        anonymous = root / "anonymous.pf"
        anonymous.write_text("fun arg0()->i64:\n    return 1\nword echo(i64)->i64:\n    drop arg0\nfun main():\n    print(echo(42))\n", encoding="utf-8")
        anonymous_data = symbols(anonymous)
        function_arg0 = [record for record in anonymous_data["symbols"] if record["name"] == "arg0"]
        assert len(function_arg0) == 1 and not function_arg0[0]["uses"], "anonymous parameters must shadow globals without exposing a fake rename target"
        underscores = root / "underscores.pf"
        underscore_text = "\n".join([
            "fun _()->i64:", "    return 1",
            "fun global_call()->i64:", "    return _()",
            "fun parameter(_:i64)->i64:", "    return _",
            "fun generic[_](value:_)->_:", "    return value",
            "fun generic_value[_](_:_) -> _:", "    return _",
            "fun main():", "    let _=42", "    print(_)", "    print(parameter(_))",
            "    print(generic(_))", "    print(generic_value(_))",
            "    if true:", "        let (first,_)=(1,2)", "        print(_)",
            "    match (3,4):", "        case (_,value):", "            print(_)", ""
        ])
        underscores.write_text(underscore_text, encoding="utf-8")
        underscore_records = symbols(underscores)["symbols"]
        def underscore(kind, line):
            matches = [record for record in underscore_records if record["name"] == "_" and record["kind"] == kind and record["definition"]["line"] == line]
            assert len(matches) == 1, (kind, line, matches)
            return matches[0]
        assert [use["line"] for use in underscore("function", 1)["uses"]] == [4]
        assert [use["line"] for use in underscore("parameter", 5)["uses"]] == [6]
        assert len(underscore("typeParameter", 7)["uses"]) == 2
        assert len(underscore("typeParameter", 9)["uses"]) == 2
        assert [use["line"] for use in underscore("parameter", 9)["uses"]] == [10]
        assert [use["line"] for use in underscore("variable", 12)["uses"]] == [13, 14, 15, 16, 22]
        assert [use["line"] for use in underscore("variable", 18)["uses"]] == [19]
        assert not any(record["name"] == "_" and record["definition"]["line"] == 21 for record in underscore_records), "match wildcard must not introduce an underscore binding"

        # Changing an identifier spelling leaves unrelated same-text bindings alone.
        shadow = find("value", "parameter", "fun inner")
        old = source.read_text(encoding="utf-8")
        lines = old.splitlines(keepends=True)
        positions = [shadow["definition"], *shadow["uses"]]
        for location in sorted(positions, key=lambda item: (item["line"], item["column"]), reverse=True):
            line = location["line"] - 1
            start = location["column"] - 1
            lines[line] = lines[line][:start] + "inner_value" + lines[line][start + len(shadow["name"]):]
        source.write_text("".join(lines), encoding="utf-8")
        renamed = symbols(source)
        target = [record for record in renamed["symbols"] if record["name"] == "inner_value"]
        assert len(target) == 1 and len(target[0]["uses"]) == 1
        assert sum(record["name"] == "value" for record in renamed["symbols"]) == sum(record["name"] == "value" for record in records) - 1
    print("Semantic symbol positions, qualification, scopes, captures, templates and rename isolation passed")


if __name__ == "__main__":
    main()
