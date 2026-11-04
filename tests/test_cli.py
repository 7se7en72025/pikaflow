"""End-to-end compiler checks; no third-party Python dependencies.

Usage: python tests/test_cli.py path/to/pikaflow[.exe]
LLVM builds additionally compare the reference interpreter with JIT and native
execution at O0/O2. Every subprocess has a timeout, including malformed inputs.
"""
from __future__ import annotations

import os
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
        ("stdlib.pf", "42\nfalse\n10\n42\nfalse\n49\n[1, 1, 3, 4, 5]\n14\n3\n-1\n1\nminimum of empty sequence\n9223372036854775807\n-9223372036854775808\nfalse\nfalse\nfalse\n42\n"),
        ("modules/main.pf", "42\n"),
    ]
    native_cases = {"hello.pf", "shared_stack.pf", "closures.pf", "arrays.pf", "results.pf", "patterns.pf", "pointers.pf", "stdlib.pf"}

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
            ("short_circuit_stack", """
                fun main() -> unit:
                    stack(10):
                        pass
                    let first = false and stack() -> bool:
                        1 + true
                    let first_value = stack() -> i64:
                        pass
                    print(first)
                    print(first_value)
                    stack(10):
                        pass
                    let second = true and stack() -> bool:
                        1 + true
                    let second_value = stack() -> i64:
                        pass
                    print(second)
                    print(second_value)
            """, "false\n10\ntrue\n11\n"),
            ("loop_stack_exits", """
                fun main() -> unit:
                    stack(0):
                        pass
                    for number in range(1, 10):
                        if number == 2:
                            continue
                        stack:
                            number +
                        if number == 5:
                            break
                    let total = stack() -> i64:
                        pass
                    print(total)
                    let count = stack(0) -> i64:
                        dup 10 <
                        while:
                            1 + dup 3 ==
                            if:
                                break
                            dup 10 <
                    print(count)
            """, "13\n3\n"),
            ("closure_shadow_and_loop", """
                fun zero() -> i64:
                    return 0
                fun main() -> unit:
                    let value = 1
                    let read = fun() -> i64:
                        return value
                    if true:
                        let value = 2
                        print(read())
                        print(value)
                    print(read())
                    let functions = [zero, zero]
                    for index in range(2):
                        let capture = fun() -> i64:
                            return index
                        functions[index] = capture
                    gc()
                    print(functions[0]())
                    print(functions[1]())
            """, "1\n2\n1\n0\n1\n"),
            ("conversion_limits", """
                fun main() -> unit:
                    print(i64(-9223372036854775808.0))
                    print(i64(9223372036854774784.0))
            """, "-9223372036854775808\n9223372036854774784\n"),
            ("panic_terminates", """
                fun fail() -> i64:
                    panic("unused failure")
                fun choose(value: bool) -> i64:
                    if value:
                        return 42
                    else:
                        panic("unused failure")
                fun main() -> unit:
                    print(choose(true))
            """, "42\n"),
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

        stdlib_directory = examples.parent / "stdlib"
        string_import = (stdlib_directory / "strings.pf").as_posix()
        numeric_import = (stdlib_directory / "numeric.pf").as_posix()
        sequence_import = (stdlib_directory / "sequences.pf").as_posix()
        standard_source = directory / "standard library boundaries.pf"
        parse_cases = [
            ("0", 0), ("-0", 0), ("+0", 0), ("000001", 1), ("-000012", -12),
            ("9223372036854775807", 9223372036854775807),
            ("-9223372036854775808", -9223372036854775808),
            ("9223372036854775808", None), ("-9223372036854775809", None),
            ("9999999999999999999999999999999999999999", None),
            ("", None), ("-", None), ("+", None), (" 1", None), ("1 ", None),
            ("1_2", None), ("1.2", None), ("--1", None), ("+1x", None),
            ("0x10", None), ("١", None),
        ]
        lines = [f'import "{string_import}"', f'import "{numeric_import}"',
                 f'import "{sequence_import}"', "fun main() -> unit:"]
        expected_lines = []
        for index, (text, value) in enumerate(parse_cases):
            lines += [f'    let parsed_{index} = parse_i64("{text}")',
                      f"    print(is_ok(parsed_{index}))"]
            expected_lines.append("true" if value is not None else "false")
            if value is not None:
                lines.append(f"    print(unwrap(parsed_{index}))")
                expected_lines.append(str(value))
        lines += [
            "    print(is_ascii_digit(48))", "    print(is_ascii_digit(-1))",
            "    print(is_ascii_alpha(65))", "    print(is_ascii_alpha(196))",
            "    print(min_i64(-9223372036854775808, 1))",
            "    print(max_i64(9223372036854775807, 1))",
            "    print(is_ok(clamp_i64(2, 5, 1)))",
            "    print(is_ok(safe_divide(-9223372036854775808, -1)))",
            "    print(unwrap(safe_divide(-7, 3)))",
            "    let empty: [i64] = []", "    sort_in_place(empty)",
            "    print(sum(empty))", "    print(binary_search(empty, 1))",
            "    let reversed = [5, 4, 3, 2, 1]", "    sort_in_place(reversed)",
            "    print(reversed)", "    print(binary_search(reversed, 1))",
            "    print(binary_search(reversed, 5))",
        ]
        expected_lines += ["true", "false", "true", "false", "-9223372036854775808",
                           "9223372036854775807", "false", "false", "-2", "0", "-1",
                           "[1, 2, 3, 4, 5]", "0", "4"]
        standard_source.write_text("\n".join(lines) + "\n", encoding="utf-8")
        successful(run([compiler, "interpret", str(standard_source)]), "\n".join(expected_lines) + "\n", "standard library boundaries")
        completed += 1
        if llvm:
            for optimization in ("-O0", "-O2"):
                successful(run([compiler, "run", str(standard_source), optimization]),
                           "\n".join(expected_lines) + "\n", f"JIT standard library boundaries {optimization}")
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
            ("stack_local_scope", "fun main():\n    stack:\n        let hidden = 42\n    print(hidden)\n", "unknown name"),
            ("builtin_function", "fun len(value: i64) -> i64:\n    return value\nfun main():\n    return\n", "reserved"),
            ("builtin_binding", "fun main():\n    let print = 1\n", "reserved"),
            ("unit_parameter", "fun invalid(value: unit):\n    return\nfun main():\n    return\n", "unit"),
            ("unit_array_parameter", "fun invalid(values: [unit]):\n    return\nfun main():\n    return\n", "unit"),
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
            ("conversion_nan", "print(i64(0.0 / 0.0))", "outside i64"),
            ("conversion_infinity", "print(i64(1.0 / 0.0))", "outside i64"),
            ("conversion_upper", "print(i64(9223372036854775808.0))", "outside i64"),
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

        first_module = directory / "first extern module.pf"
        second_module = directory / "second extern module.pf"
        first_module.write_text('extern fun hidden_c(value: f64) -> f64 = "fabs"\nexport fun first() -> i64:\n    unsafe:\n        return i64(hidden_c(-20.0))\n', encoding="utf-8")
        second_module.write_text('extern fun hidden_c(value: f64) -> f64 = "fabs"\nexport fun second() -> i64:\n    unsafe:\n        return i64(hidden_c(-22.0))\n', encoding="utf-8")
        private_extern = directory / "private extern.pf"
        private_extern.write_text('import "first extern module.pf"\nfun main():\n    unsafe:\n        print(hidden_c(-1.0))\n', encoding="utf-8")
        result = run([compiler, "check", str(private_extern)])
        require(result.returncode != 0 and "unknown function" in result.stderr and "hidden_c" in result.stderr,
                "private extern alias leaked into importer: " + result.stderr)
        completed += 1
        duplicate_extern = directory / "duplicate private externs.pf"
        duplicate_extern.write_text('import "first extern module.pf"\nimport "second extern module.pf"\nfun main():\n    print(first())\n    print(second())\n', encoding="utf-8")
        successful(run([compiler, "interpret", str(duplicate_extern)]), "20\n22\n", "module-local extern aliases")
        completed += 1
        if llvm:
            successful(run([compiler, "run", str(duplicate_extern), "-O2"]), "20\n22\n", "JIT module-local extern aliases")
            completed += 1

        standard_import = directory / "standard prefix.pf"
        standard_import.write_text('import "std:numeric"\nfun main():\n    print(unwrap(abs_checked(-42)))\n', encoding="utf-8")
        successful(run([compiler, "interpret", str(standard_import)]), "42\n", "std: import prefix")
        completed += 1
        unicode_source = directory / "पिका_日本語.pf"
        unicode_source.write_text("fun main():\n    print(42)\n", encoding="utf-8")
        successful(run([compiler, "interpret", str(unicode_source)]), "42\n", "Unicode source path")
        completed += 1

        protected = directory / "protected source.pf"
        original_source = "fun main():\n    print(42)\n"
        protected.write_text(original_source, encoding="utf-8")
        if sys.platform.startswith("linux") and pathlib.Path("/dev/full").is_char_device():
            result = run([compiler, "dump-ir", str(protected), "-o", "/dev/full"])
            require(result.returncode != 0 and "failed writing output file" in result.stderr,
                    "buffered output failure was reported as success: " + result.stderr)
            completed += 1
        for command in ("dump-tokens", "dump-ast", "dump-ir", "emit-ir", "emit-obj", "build"):
            result = run([compiler, command, str(protected), "-o", str(protected)])
            require(result.returncode != 0 and "overwrite" in result.stderr,
                    command + " accepted source overwrite: " + result.stderr)
            require(protected.read_text(encoding="utf-8") == original_source,
                    command + " damaged protected source")
            completed += 1
        hard_link = directory / "hard linked output.pf"
        try:
            os.link(protected, hard_link)
        except (OSError, NotImplementedError):
            pass
        else:
            result = run([compiler, "dump-tokens", str(protected), "-o", str(hard_link)])
            require(result.returncode != 0 and "overwrite" in result.stderr,
                    "hard-linked source overwrite was accepted: " + result.stderr)
            require(protected.read_text(encoding="utf-8") == original_source,
                    "hard-linked output damaged protected source")
            completed += 1
        imported_source = directory / "protected dependency.pf"
        imported_text = "export fun dependency() -> i64:\n    return 42\n"
        imported_source.write_text(imported_text, encoding="utf-8")
        importer = directory / "protected importer.pf"
        importer.write_text('import "protected dependency.pf"\nfun main():\n    print(dependency())\n', encoding="utf-8")
        result = run([compiler, "dump-ir", str(importer), "-o", str(imported_source)])
        require(result.returncode != 0 and "overwrite" in result.stderr,
                "imported-source overwrite was accepted: " + result.stderr)
        require(imported_source.read_text(encoding="utf-8") == imported_text,
                "output damaged imported source")
        completed += 1

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
