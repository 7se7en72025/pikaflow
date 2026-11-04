"""Check generated C aggregate calls against a C-compiler-built shared fixture.

Arguments: pikaflow executable, fixture shared library, fixture import/link library.
"""
from __future__ import annotations
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import textwrap


def source(declarations: str, body: str) -> str:
    statements = "\n".join("        " + line for line in textwrap.dedent(body).strip().splitlines())
    return textwrap.dedent(declarations).strip() + "\n\nfun main() -> unit:\n    unsafe:\n" + statements + "\n"


CASES = {
    "pair": source('extern fun shift(p: (i64, i64), d: i64) -> (i64, i64) = "pf_ffi_pair"', """
        let p = shift((10, 20), 6)
        print(p[0] + p[1])
    """),
    "mixed": source('extern fun shift(p: (f64, i64), d: f64) -> (f64, i64) = "pf_ffi_mixed"', """
        let p = shift((10.0, 20), 12.0)
        print(i64(p[0]) + p[1])
    """),
    "padded": source('extern fun score(p: (bool, f64, bool)) -> i64 = "pf_ffi_padded"', "print(score((true, 40.0, true)))"),
    "tiny": source('extern fun score(p: (bool, bool, bool)) -> i64 = "pf_ffi_tiny"', "print(score((true, true, true)))"),
    "doubles": source('extern fun shift(p: (f64, f64), d: f64) -> (f64, f64) = "pf_ffi_doubles"', """
        let p = shift((10.0, 20.0), 6.0)
        print(i64(p[0] + p[1]))
    """),
    "big": source('extern fun shift(p: (i64, i64, i64), d: i64) -> (i64, i64, i64) = "pf_ffi_big"', """
        let p = shift((1, 2, 3), 12)
        print(p[0] + p[1] + p[2])
    """),
    "gpr_pressure": source('extern fun pressure(a: i64, b: i64, c: i64, d: i64, e: i64, p: (i64, i64), tail: i64) -> i64 = "pf_ffi_pressure"', "print(pressure(1, 2, 3, 4, 5, (10, 11), 6))"),
    "sse_pressure": source('extern fun pressure(a: f64, b: f64, c: f64, d: f64, e: f64, f: f64, g: f64, p: (f64, f64), tail: f64) -> f64 = "pf_ffi_sse_pressure"', "print(i64(pressure(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, (20.0, 22.0), 0.0)))"),
    "varargs": source('extern fun variable(marker: i64, ...) -> i64 = "pf_ffi_varargs"', "print(variable(1, true, 10.0, (10, 20)))"),
    "pointer": source('extern fun score(p: (ptr[i64], bool)) -> i64 = "pf_ffi_pointer"', """
        let p = alloc_i64(1)
        store(p, 0, 42)
        print(score((p, true)))
        free(p)
    """),
    "small": source('''
        extern fun flip(p: (bool, bool)) -> (bool, bool) = "pf_ffi_small"
        extern fun score(p: (bool, bool)) -> i64 = "pf_ffi_small_score"
    ''', "print(score(flip((false, true))))"),
    "one": source('extern fun increment(p: (i64,)) -> (i64,) = "pf_ffi_one"', """
        let p = increment((20,))
        print(p[0])
    """),
    "nested": source('extern fun shift(p: ((i64, i64), (f64, i64), bool), delta: i64) -> ((i64, i64), (f64, i64), bool) = "pf_ffi_nested"', """
        var index = 0
        var total = 0
        while index < 200:
            let p = shift(((1, 2), (3.0, 4), true), 8)
            total = p[0][0] + p[0][1] + i64(p[1][0]) + p[1][1]
            index = index + 1
        gc()
        print(total)
    """),
    "nested_small": source('extern fun shift(p: ((f64,), i64), delta: f64) -> ((f64,), i64) = "pf_ffi_nested_small"', """
        let p = shift(((10.0,), 20), 12.0)
        print(i64(p[0][0]) + p[1])
    """),
    "callback_i64": source('''
        extern fun invoke(callback: ptr[unit]) -> i64 = "pf_ffi_callback_i64"
        fun callback(a: i64, b: i64) -> i64:
            return a + b
    ''', 'print(invoke(c_callback(callback)))'),
    "callback_f64": source('''
        extern fun invoke(callback: ptr[unit]) -> f64 = "pf_ffi_callback_f64"
        fun callback(a: f64, b: f64) -> f64:
            return a + b
    ''', 'print(i64(invoke(c_callback(callback))))'),
    "callback_bool": source('''
        extern fun invoke(callback: ptr[unit]) -> i64 = "pf_ffi_callback_bool"
        fun callback(a: bool, b: bool) -> bool:
            return a and not b
    ''', 'print(invoke(c_callback(callback)))'),
    "callback_unit": source('''
        extern fun invoke(callback: ptr[unit]) -> unit = "pf_ffi_callback_unit"
        fun callback() -> unit:
            print(42)
    ''', 'invoke(c_callback(callback))'),
    "callback_pointer": source('''
        extern fun invoke(callback: ptr[unit]) -> i64 = "pf_ffi_callback_pointer"
        fun callback(value: ptr[i64]) -> ptr[i64]:
            return value
    ''', 'print(invoke(c_callback(callback)))'),
    "libc": source('''
        extern fun length(p: ptr[unit]) -> i64 = "strlen"
        extern fun root(x: f64) -> f64 = "sqrt"
        extern fun foreign_alloc(n: i64) -> ptr[unit] = "malloc"
        extern fun foreign_free(p: ptr[unit]) -> unit = "free"
    ''', '''
        let text = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        let handle = pin(text)
        gc()
        print(length(cstr(text)))
        unpin(handle)
        print(i64(root(1764.0)))
        let memory = foreign_alloc(16)
        foreign_free(memory)
    '''),
}

REJECTED = {
    "callback_unsafe": ("fun callback() -> i64:\n    return 42\nfun main():\n    c_callback(callback)\n", "unsafe"),
    "callback_capture": ("fun main():\n    let n = 42\n    fun callback() -> i64:\n        return n\n    unsafe:\n        c_callback(callback)\n", "noncapturing module-level"),
    "callback_managed": ("fun callback(value: str) -> i64:\n    return len(value)\nfun main():\n    unsafe:\n        c_callback(callback)\n", "scalar/pointer parameters"),
}


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    compiler, fixture, link_library = (str(pathlib.Path(x).resolve()) for x in sys.argv[1:])
    environment = os.environ.copy()
    variable = "PATH" if os.name == "nt" else "LD_LIBRARY_PATH"
    environment[variable] = str(pathlib.Path(fixture).parent) + os.pathsep + environment.get(variable, "")

    def run(arguments: list[str], expected: str | None = None) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(arguments, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=90, env=environment)
        if result.returncode != 0 or (expected is not None and result.stdout.replace("\r\n", "\n") != expected):
            raise AssertionError(f"{arguments}\nexit {result.returncode}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}")
        return result

    count = 0
    with tempfile.TemporaryDirectory(prefix="pikaflow C ABI ") as temporary:
        directory = pathlib.Path(temporary)
        if os.name == "nt":
            shutil.copy2(fixture, directory / pathlib.Path(fixture).name)
        for name, (contents, diagnostic) in REJECTED.items():
            path = directory / (name + ".pf")
            path.write_text(contents, encoding="utf-8")
            result = subprocess.run([compiler, "check", str(path)], capture_output=True, text=True, timeout=30, env=environment)
            if result.returncode == 0 or diagnostic not in result.stderr:
                raise AssertionError(f"{name}: expected {diagnostic!r}\n{result.stderr}")
        for name, contents in CASES.items():
            path = directory / (name + ".pf")
            path.write_text(contents, encoding="utf-8")
            expected = "42\n42\n" if name == "libc" else "42\n"
            run([compiler, "check", str(path)])
            for level in ("-O0", "-O2"):
                run([compiler, "run", str(path), level, "--load", fixture], expected)
                executable = directory / (name + level + (".exe" if os.name == "nt" else ""))
                run([compiler, "build", str(path), level, "--link", link_library, "-o", str(executable)])
                run([str(executable)], expected)
                count += 2
    print(f"{count} C ABI JIT/native checks and {len(REJECTED)} rejected callbacks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
