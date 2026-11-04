"""Qualified modules and single-evaluation assignment through every execution mode."""
import os
import pathlib
import subprocess
import sys
import tempfile
import textwrap


LIBRARY = '''
export enum Option[T]:
    Some(value: T)
    None
enum Secret:
    Hidden
fun private_sum(a: i64, b: i64) -> i64:
    return a + b
export fun answer() -> i64:
    return private_sum(20, 22)
export fun identity[T](value: T) -> T:
    return value
export word add(i64, i64) -> i64:
    +
export extern fun magnitude(value: f64) -> f64 = "fabs"
extern fun private_magnitude(value: f64) -> f64 = "fabs"
'''

CASES = {
    "qualified": ('''
        import "library.pf" as lib
        fun main() -> unit:
            print(lib.answer())
            print(lib.identity(42))
            let function = lib.identity::[str]
            print(function("qualified"))
            let value: lib.Option[i64] = lib.Some::[i64](42)
            match value:
                case lib.Some(result):
                    print(result)
                case lib.None():
                    print(0)
            let sum = stack(20, 22) -> i64:
                lib.add
            print(sum)
            unsafe:
                print(lib.magnitude(-42.0))
    ''', "42\n42\nqualified\n42\n42\n42\n"),
    "standard_alias": ('''
        import "std:numeric" as numeric
        fun main() -> unit:
            print(unwrap(numeric.safe_divide(84, 2)))
            6 |> numeric.square |> print
    ''', "42\n36\n"),
    "separate_aliases": ('''
        import "library.pf" as first
        import "other.pf" as second
        fun main() -> unit:
            print(first.answer())
            print(second.answer())
    ''', "42\n9\n"),
    "indexed_order": ('''
        fun main() -> unit:
            let values = [20]
            var trace = 0
            fun base() -> [i64]:
                trace = trace * 10 + 1
                return values
            fun index() -> i64:
                trace = trace * 10 + 2
                return 0
            fun right() -> i64:
                trace = trace * 10 + 3
                values[0] = 100
                return 22
            base()[index()] += right()
            print(trace)
            print(values[0])
    ''', "123\n42\n"),
    "local_order": ('''
        fun main() -> unit:
            var value = 20
            fun right() -> i64:
                value = 100
                return 22
            value += right()
            print(value)
    ''', "42\n"),
    "all_compound_operators": ('''
        fun main() -> unit:
            let values = [84]
            values[0] /= 2
            values[0] *= 2
            values[0] -= 40
            values[0] %= 43
            values[0] += 41
            print(values[0])
            let real = [21.0]
            real[0] *= 2.0
            print(real[0])
    ''', "42\n42\n"),
    "nested_index_order": ('''
        fun main() -> unit:
            let values = [[20]]
            var trace = 0
            fun base() -> [[i64]]:
                trace = trace * 10 + 1
                return values
            fun row() -> i64:
                trace = trace * 10 + 2
                return 0
            fun column() -> i64:
                trace = trace * 10 + 3
                return 0
            fun right() -> i64:
                trace = trace * 10 + 4
                return 22
            base()[row()][column()] += right()
            print(trace)
            print(values[0][0])
    ''', "1234\n42\n"),
}

NEGATIVE = {
    "private_function": ('import "library.pf" as lib\nfun main():\n    lib.private_sum(1,2)\n', "private"),
    "private_type": ('import "library.pf" as lib\nfun main():\n    let value:lib.Secret=lib.Hidden()\n', "private"),
    "private_constructor": ('import "library.pf" as lib\nfun main():\n    lib.Hidden()\n', "private"),
    "private_external": ('import "library.pf" as lib\nfun main():\n    unsafe:\n        lib.private_magnitude(-42.0)\n', "private"),
    "no_unqualified_leak": ('import "library.pf" as lib\nfun main():\n    answer()\n', "unknown function"),
    "duplicate_alias": ('import "library.pf" as lib\nimport "other.pf" as lib\nfun main():\n    pass\n', "duplicate"),
    "unknown_alias": ('fun main():\n    missing.answer()\n', "unknown module alias"),
    "alias_binding_conflict": ('import "library.pf" as lib\nfun main():\n    let lib=42\n', "conflicts with a module alias"),
    "compound_non_numeric": ('fun main():\n    let values=[true]\n    values[0]+=true\n', "arithmetic requires"),
}


def main():
    compiler = str(pathlib.Path(sys.argv[1]).resolve())
    def run(command, expected=None, diagnostic=None):
        result = subprocess.run(command, text=True, encoding="utf-8", capture_output=True, timeout=60)
        if diagnostic:
            assert result.returncode != 0 and diagnostic in result.stderr, (command, result.stderr)
        else:
            assert result.returncode == 0, (command, result.stderr)
            if expected is not None:
                assert result.stdout.replace("\r\n", "\n") == expected, (command, result.stdout, expected)
        return result
    native = "LLVM enabled" in run([compiler, "--version"]).stdout
    checks = 0
    with tempfile.TemporaryDirectory(prefix="pikaflow namespaces ") as temporary:
        root = pathlib.Path(temporary)
        (root / "library.pf").write_text(textwrap.dedent(LIBRARY).lstrip(), encoding="utf-8")
        (root / "other.pf").write_text("export fun answer()->i64:\n    return 9\n", encoding="utf-8")
        for name, (source, expected) in CASES.items():
            path = root / (name + ".pf")
            path.write_text(textwrap.dedent(source).lstrip(), encoding="utf-8")
            run([compiler, "interpret", str(path)], expected); checks += 1
            if native:
                for level in ("-O0", "-O2"):
                    run([compiler, "run", str(path), level], expected); checks += 1
                    executable = root / (name + level + (".exe" if os.name == "nt" else ""))
                    run([compiler, "build", str(path), level, "-o", str(executable)])
                    run([str(executable)], expected); checks += 1
        for name, (source, diagnostic) in NEGATIVE.items():
            path = root / (name + ".pf"); path.write_text(source, encoding="utf-8")
            run([compiler, "check", str(path)], diagnostic=diagnostic); checks += 1
        recovery = root / "recovery.pf"
        recovery.write_text("fun bad[T](value:T)->i64:\n    return len(value)\nfun first():\n    bad(42)\nfun second():\n    bad(42)\nfun third():\n    let value:imaginary=1\nfun fourth():\n    print(missing)\nfun main():\n    pass\n", encoding="utf-8")
        result = run([compiler, "check", str(recovery)], diagnostic="len requires")
        assert result.stderr.count("len requires") == 2, result.stderr
        assert "unknown type 'imaginary'" in result.stderr and "unknown name 'missing'" in result.stderr, result.stderr
        checks += 1
        recovery.write_text("\n".join(f"fun bad{i}():\n    missing{i}()" for i in range(25)) + "\nfun main():\n    pass\n", encoding="utf-8")
        run([compiler, "check", str(recovery)], diagnostic="stopped after 20 semantic function errors"); checks += 1
    print(f"{checks} namespace/assignment checks passed ({'LLVM enabled' if native else 'interpreter'})")


if __name__ == "__main__":
    main()
