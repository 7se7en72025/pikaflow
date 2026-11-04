"""Contextual inference and callable-namespace regressions through the real CLI."""
import pathlib
import subprocess
import sys
import tempfile
import textwrap
from test_cli import require


def main():
    compiler = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="pikaflow contexts ") as temporary:
        root = pathlib.Path(temporary)

        def execute(source, expected=None, diagnostic=None):
            path = root / "main.pf"
            path.write_text(textwrap.dedent(source).lstrip(), encoding="utf-8")
            result = subprocess.run([compiler, "interpret" if diagnostic is None else "check", str(path)],
                                    text=True, encoding="utf-8", capture_output=True, timeout=20)
            if diagnostic is not None:
                require(result.returncode != 0 and diagnostic in result.stderr, result.stderr)
            else:
                require(result.returncode == 0, result.stderr)
                require(result.stdout.replace("\r\n", "\n") == expected, result.stdout)

        execute("""
            fun count(values: [i64]) -> i64:
                return len(values)
            fun empty() -> Result[[i64], str]:
                return ok([])
            fun main() -> unit:
                let result: Result[[i64], str] = ok([])
                let pair: ([i64], i64) = ([], 42)
                let nested: [[i64]] = [[]]
                print(len(unwrap(result)))
                print(len(pair[0]))
                print(count([]))
                print(len(unwrap(empty())))
                print(len(nested[0]))
        """, "0\n0\n0\n0\n0\n")
        (root / "ffi.pf").write_text('export extern fun magnitude(value: f64) -> f64 = "fabs"\n', encoding="utf-8")
        execute("""
            import "ffi.pf"
            fun main() -> unit:
                unsafe:
                    print(magnitude(-42.0))
        """, "42\n")
        (root / "functions.pf").write_text("export fun magnitude(value: i64) -> i64:\n    return 1\n", encoding="utf-8")
        execute("""
            import "functions.pf"
            extern fun magnitude(value: f64) -> f64 = "fabs"
            fun main() -> unit:
                unsafe:
                    print(magnitude(-42.0))
        """, "42\n")
        execute("""
            import "ffi.pf"
            import "functions.pf"
            fun main() -> unit:
                unsafe:
                    print(magnitude(-42.0))
        """, diagnostic="ambiguous imported callable")
        execute("""
            fun magnitude(value: i64) -> i64:
                return value
            extern fun magnitude(value: f64) -> f64 = "fabs"
            fun main() -> unit:
                pass
        """, diagnostic="duplicate extern")
        execute("""
            fun main() -> unit:
                let result: Result[[i64], str] = ok([true])
        """, diagnostic="requires i64")
    print("Contextual type and callable namespace regressions passed (6 programs)")


if __name__ == "__main__":
    main()
