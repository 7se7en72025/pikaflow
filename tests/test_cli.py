"""End-to-end compiler checks; no third-party Python dependencies.

Usage: python tests/test_cli.py path/to/pikaflow[.exe]
LLVM builds additionally compare the reference interpreter with JIT and native
execution at O0/O2. Every subprocess has a timeout, including malformed inputs.
"""
from __future__ import annotations

import pathlib
import re
import subprocess
import sys
import tempfile
import textwrap


def run(command: list[str], *, timeout: int = 60) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, encoding="utf-8", errors="replace",
                          capture_output=True, timeout=timeout, shell=False)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def output(result: subprocess.CompletedProcess[str]) -> str:
    return result.stdout.replace("\r\n", "\n")


def successful(result: subprocess.CompletedProcess[str], expected: str, context: str) -> None:
    require(result.returncode == 0,
            f"{context}: exit {result.returncode}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    require(output(result) == expected,
            f"{context}: expected {expected!r}, got {output(result)!r}\nstderr:\n{result.stderr}")


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    compiler = str(pathlib.Path(sys.argv[1]).resolve())
    version = run([compiler, "--version"])
    require(version.returncode == 0, "--version failed: " + version.stderr)
    llvm = bool(re.search(r"LLVM\s+(?:enabled|24)", version.stdout, re.IGNORECASE))
    examples = pathlib.Path(__file__).resolve().parents[1] / "examples"
    completed = 0

    cases = [
        ("hello.pf", "42\n36\n"),
        ("shared_stack.pf", "10\n42\n9\n7\n15\n"),
        ("recursion.pf", "3628800\n"),
        ("arrays.pf", "44\n3\n[1, 40, 3]\n0\n"),
        ("closures.pf", "41\n42\n42\n"),
        ("recursive_closure.pf", "720\n"),
        ("results.pf", "true\n42\nfalse\ndivision requires a nonzero denominator\n"),
        ("control_flow.pf", "13\n3\n2\n1\nmatched\n3\n2\n1\n"),
        ("strings.pf", "PikaFlow\n8\n80\ntrue\nescaped\nline\n1.5\n"),
        ("pointers.pf", "42\n42\n"),
        ("patterns.pf", "42\n15\n5\n42\nexpected failure\nten\n"),
        ("modules/main.pf", "42\n"),
    ]
    native_cases = {"hello.pf", "shared_stack.pf", "closures.pf", "arrays.pf", "results.pf", "patterns.pf", "pointers.pf"}

    with tempfile.TemporaryDirectory(prefix="pikaflow cli ") as temporary:
        directory = pathlib.Path(temporary)
        for filename, expected in cases:
            source = str(examples / filename)
            checked = run([compiler, "check", source])
            require(checked.returncode == 0, f"check {filename}: {checked.stderr}")
            successful(run([compiler, "interpret", source]), expected, f"interpret {filename}")
            completed += 2
            if llvm:
                for optimization in ("-O0", "-O2"):
                    successful(run([compiler, "run", source, optimization]), expected,
                               f"JIT {optimization} {filename}")
                    completed += 1
                    if filename in native_cases:
                        executable = directory / (filename.replace(".pf", "") + optimization + (".exe" if sys.platform == "win32" else ""))
                        built = run([compiler, "build", source, optimization, "-o", str(executable)], timeout=120)
                        require(built.returncode == 0, f"build {filename} {optimization}: {built.stderr}\n{built.stdout}")
                        successful(run([str(executable)]), expected, f"native {optimization} {filename}")
                        completed += 2

        extra_cases = [
            ("short_circuit", """
                fun explode() -> bool:
                    panic("short-circuit failed")
                    return true
                fun main() -> unit:
                    print(false and explode())
                    print(true or explode())
            """, "false\ntrue\n"),
            ("stack_join", """
                fun main() -> unit:
                    stack(10, 10):
                        pass
                    if true:
                        stack:
                            2 +
                    else:
                        stack:
                            3 +
                    let (first, second) = stack() -> (i64, i64):
                        swap
                    print(first)
                    print(second)
            """, "12\n10\n"),
            ("capture_mutation", """
                fun main() -> unit:
                    var value = 1
                    let read = fun() -> i64:
                        return value
                    value = 42
                    gc()
                    print(read())
            """, "42\n"),
            ("call_evaluation", """
                fun main() -> unit:
                    var callable = fun(x: i64) -> i64:
                        return 1
                    fun replace() -> i64:
                        callable = fun(x: i64) -> i64:
                            return 2
                        return 0
                    print(callable(replace()))
                    print(callable(0))
            """, "1\n2\n"),
            ("gc_roots", """
                fun main() -> unit:
                    let retained = [[42], [7]]
                    var index = 0
                    while index < 200:
                        let temporary = [index, index + 1]
                        gc()
                        index = index + 1
                    print(retained[0][0])
                    print(retained[1][0])
            """, "42\n7\n"),
            ("match_return", """
                fun describe(value: i64) -> str:
                    match value:
                        case 1:
                            return "one"
                        case _:
                            return "other"
                fun main() -> unit:
                    print(describe(1))
                    print(describe(2))
            """, "one\nother\n"),
            ("minimum_integer", """
                fun main() -> unit:
                    print(-9223372036854775808)
                    print(i64(1.9))
                    print(i64(-1.9))
            """, "-9223372036854775808\n1\n-1\n"),
            ("stack_control", """
                fun main() -> unit:
                    let answer = stack(5, true) -> i64:
                        if:
                            1 +
                        else:
                            2 +
                    print(answer)
                    let count = stack(0) -> i64:
                        dup 3 <
                        while:
                            1 + dup 3 <
                    print(count)
                    let logic = stack(true, false) -> bool:
                        and
                    print(logic)
            """, "6\n3\nfalse\n"),
            ("floating_comparisons", """
                fun main() -> unit:
                    let nan = 0.0 / 0.0
                    print(nan == nan)
                    print(nan != nan)
                    print(nan < 1.0)
            """, "false\ntrue\nfalse\n"),
        ]
        for name, source_text, expected in extra_cases:
            source = directory / f"{name}.pf"
            source.write_text(textwrap.dedent(source_text).lstrip(), encoding="utf-8")
            successful(run([compiler, "interpret", str(source)]), expected, name)
            completed += 1
            if llvm:
                for optimization in ("-O0", "-O2"):
                    successful(run([compiler, "run", str(source), optimization]), expected, f"JIT {name} {optimization}")
                    completed += 1

        negative_cases = [
            ("mixed_types", "fun main():\n    print(1 + true)\n", "received bool"),
            ("immutable", "fun main():\n    let x = 1\n    x = 2\n", "immutable"),
            ("unknown_name", "fun main():\n    print(missing)\n", "unknown name"),
            ("arity", "fun add(a: i64, b: i64) -> i64:\n    return a+b\nfun main():\n    add(1)\n", "expects 2 arguments"),
            ("stack_underflow", "fun main():\n    stack:\n        +\n", "underflow"),
            ("stack_output", "fun main():\n    let x = stack(1) -> bool:\n        pass\n", "requires bool"),
            ("stack_branch", "fun main():\n    if true:\n        stack(1):\n            pass\n", "different stack heights"),
            ("stack_escape", "fun main():\n    stack(1):\n        pass\n    return\n", "shared operand stack"),
            ("missing_return", "fun f() -> i64:\n    print(1)\nfun main():\n    print(f())\n", "does not return"),
            ("condition", "fun main():\n    if 1:\n        print(1)\n", "requires bool"),
            ("empty_array", "fun main():\n    let values = []\n", "explicit element type"),
            ("tabs", "fun main():\n\tprint(1)\n", "tabs"),
            ("indent", "fun main():\n    print(1)\n  print(2)\n", "indentation"),
            ("literal_overflow", "fun main():\n    print(9223372036854775808)\n", "i64 range"),
            ("unknown_type", "fun main():\n    let value: imaginary = 1\n", "unknown type"),
            ("unsafe_allocation", "fun main():\n    let p = alloc_i64(1)\n", "unsafe"),
            ("unsafe_extern", "extern fun pf_checked_add(a: i64, b: i64) -> i64\nfun main():\n    print(pf_checked_add(1, 2))\n", "unsafe"),
            ("loop_else", "fun main():\n    while false:\n        pass\n    else:\n        print(1)\n", "not part"),
            ("pattern_scope", "fun main():\n    match [1]:\n        case [bound]:\n            pass\n    print(bound)\n", "unknown name"),
            ("word_contract", "word bad(i64) -> i64:\n    dup\nfun main():\n    return\n", "undeclared stack outputs"),
            ("loop_stack", "fun main():\n    while false:\n        stack(1):\n            pass\n", "different stack heights"),
        ]
        for name, source_text, fragment in negative_cases:
            source = directory / f"invalid_{name}.pf"
            source.write_text(source_text, encoding="utf-8")
            result = run([compiler, "check", str(source)])
            require(result.returncode != 0, f"invalid {name}: unexpectedly accepted")
            require(fragment in result.stderr, f"invalid {name}: missing {fragment!r}: {result.stderr}")
            require(str(source) in result.stderr, f"invalid {name}: diagnostic lost source filename")
            completed += 1

        recovered = directory / "two parser errors.pf"
        recovered.write_text("fun first():\n    let x =\nfun second():\n    let y =\n", encoding="utf-8")
        result = run([compiler, "check", str(recovered)])
        require(result.returncode != 0 and result.stderr.count(": error:") >= 2 and ":4:" in result.stderr,
                "CLI lost recovered parser diagnostics: " + result.stderr)
        completed += 1

        runtime_failures = [
            ("overflow", "print(9223372036854775807 + 1)", "overflow"),
            ("divide_zero", "print(1 / 0)", "zero"),
            ("modulo_zero", "print(1 % 0)", "zero"),
            ("divide_overflow", "print(-9223372036854775808 / -1)", "overflow"),
            ("negate_overflow", "let x = -9223372036854775808\n    print(-x)", "overflow"),
            ("bounds", "let xs = [1]\n    print(xs[1])", "bounds"),
            ("range_step", "for i in range(0, 3, 0):\n        print(i)", "zero"),
            ("unwrap_error", "let failure: Result[i64, str] = err(\"expected failure\")\n    print(unwrap(failure))", "expected failure"),
            ("raw_bounds", "unsafe:\n        let p = alloc_i64(1)\n        print(load(p, 1))", "bounds"),
            ("result_error_access", "let good: Result[i64, str] = ok(1)\n    print(error(good))", "error"),
        ]
        for name, body, fragment in runtime_failures:
            source = directory / f"failure_{name}.pf"
            source.write_text("fun main() -> unit:\n    " + body + "\n", encoding="utf-8")
            commands = [[compiler, "interpret", str(source)]]
            if llvm:
                commands += [[compiler, "run", str(source), optimization] for optimization in ("-O0", "-O2")]
            for command in commands:
                result = run(command)
                require(result.returncode != 0, f"runtime {name}: unexpectedly succeeded")
                require(fragment.lower() in result.stderr.lower(), f"runtime {name}: {result.stderr}")
                completed += 1

        for command, marker in (("dump-tokens", "NAME"), ("dump-ast", "Function"), ("dump-ir", "fun ")):
            result = run([compiler, command, str(examples / "hello.pf")])
            require(result.returncode == 0 and marker in result.stdout, f"{command} failed: {result.stderr}")
            completed += 1

        # Import cycles and private symbols must diagnose rather than recurse or leak.
        (directory / "a.pf").write_text('import "b.pf"\nfun main():\n    return\n', encoding="utf-8")
        (directory / "b.pf").write_text('import "a.pf"\n', encoding="utf-8")
        cycle = run([compiler, "check", str(directory / "a.pf")])
        require(cycle.returncode != 0 and "cycl" in cycle.stderr.lower(), "import cycle was not diagnosed")
        private = directory / "private.pf"
        private.write_text('import "' + (examples / "modules/math.pf").as_posix() + '"\nfun main():\n    print(private_helper())\n', encoding="utf-8")
        result = run([compiler, "check", str(private)])
        require(result.returncode != 0 and "private_helper" in result.stderr, "private module symbol leaked")
        completed += 2

        if llvm:
            ffi = str(examples / "ffi.pf")
            successful(run([compiler, "run", ffi, "-O2"]), "42\n", "C ABI runtime call")
            ir_file = directory / "hello optimized.ll"
            emitted = run([compiler, "emit-ir", str(examples / "hello.pf"), "-O2", "-o", str(ir_file)])
            require(emitted.returncode == 0 and ir_file.exists(), f"IR emission failed: {emitted.stderr}")
            require("define" in ir_file.read_text(encoding="utf-8"), "LLVM IR has no functions")
            object_file = directory / ("hello.obj" if sys.platform == "win32" else "hello.o")
            emitted = run([compiler, "emit-obj", str(examples / "hello.pf"), "-o", str(object_file)])
            require(emitted.returncode == 0 and object_file.stat().st_size > 0, f"object emission failed: {emitted.stderr}")
            completed += 3
        else:
            result = run([compiler, "run", str(examples / "hello.pf")])
            require(result.returncode != 0 and "disabled" in result.stderr,
                    "LLVM-disabled executable silently accepted JIT command")
            completed += 1

    print(f"{completed} compiler integration checks passed (LLVM {'enabled' if llvm else 'disabled'})")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, subprocess.TimeoutExpired, OSError) as error:
        print(f"compiler test failure: {error}", file=sys.stderr)
        raise SystemExit(1)
