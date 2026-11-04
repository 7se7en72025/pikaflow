"""Differential tests for typed-IR optimization and its LLVM/assembly effect."""
from __future__ import annotations
import pathlib
import random
import re
import subprocess
import sys
import tempfile
import textwrap


def expression(rng: random.Random, depth: int) -> tuple[str, int]:
    if not depth:
        value = rng.randint(-9, 9)
        return str(value), value
    left, a = expression(rng, depth - 1)
    operator = rng.choice(("+", "-", "*", "/", "%"))
    if operator in ("/", "%"):
        b = rng.choice((-3, -2, -1, 1, 2, 3))
        right = str(b)
        quotient = abs(a) // abs(b) * (-1 if (a < 0) != (b < 0) else 1)
        value = quotient if operator == "/" else a - quotient * b
    else:
        right, b = expression(rng, depth - 1)
        value = {"+": lambda: a + b, "-": lambda: a - b, "*": lambda: a * b}[operator]()
    return f"({left} {operator} {right})", value


def main() -> int:
    compiler = str(pathlib.Path(sys.argv[1]).resolve())
    def run(arguments: list[str]) -> subprocess.CompletedProcess[str]:
        return subprocess.run(arguments, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=90)
    version = run([compiler, "--version"])
    native = "LLVM enabled" in version.stdout
    cases = [
        ("constant_control", """
            fun main() -> unit:
                if (6 * 7) - 42 == 0:
                    print((20 + 22) * 2)
                else:
                    print(999)
                print(3 * 14)
        """, "84\n42\n"),
        ("effects", """
            fun emit(value: i64) -> i64:
                print(value)
                return value
            fun main() -> unit:
                if 1 + 1 == 2:
                    let data = [emit(20), emit(22)]
                    gc()
                    print(data[0] + data[1])
                else:
                    print(999)
        """, "20\n22\n42\n"),
        ("captures", """
            fun main() -> unit:
                var value = 20
                fun bump() -> i64:
                    value = value + 1
                    return value
                let callback = fun() -> i64:
                    return bump() + value
                if 21 * 2 == 42:
                    print(callback())
        """, "42\n"),
        ("tasks", """
            fun worker() -> [i64]:
                if 2 * 21 == 42:
                    return [42]
                return [0]
            fun main() -> unit:
                let job = spawn(worker)
                gc()
                print(join(job)[0])
        """, "42\n"),
        ("floating", """
            fun main() -> unit:
                let nan = 0.0 / 0.0
                print(nan != nan)
                print(nan == nan)
                print(-0.0 == 0.0)
        """, "true\nfalse\ntrue\n"),
    ]
    rng = random.Random(20261104)
    for index in range(12):
        a, av = expression(rng, 2)
        b, bv = expression(rng, 2)
        c, cv = expression(rng, 3)
        d, dv = expression(rng, 3)
        iterations = rng.randint(1, 4)
        source = f"""fun main() -> unit:
    var index = 0
    var total = 0
    while index < {iterations}:
        if {a} <= {b}:
            total = total + {c}
        else:
            total = total - {d}
        index = index + 1
    print(total)
    print({c})
"""
        cases.append((f"generated_{index}", source, f"{iterations * (cv if av <= bv else -dv)}\n{cv}\n"))
    traps = [
        ("add", "9223372036854775807 + 1", "integer addition overflow"),
        ("overflow_times_zero", "(9223372036854775807 + 1) * 0", "integer addition overflow"),
        ("division_zero", "42 / (1 - 1)", "integer division by zero"),
        ("division_overflow", "(-9223372036854775807 - 1) / -1", "integer division overflow"),
        ("remainder_overflow", "(-9223372036854775807 - 1) % -1", "integer remainder overflow"),
        ("negation_overflow", "-(-9223372036854775807 - 1)", "integer negation overflow"),
    ]
    completed = 0
    with tempfile.TemporaryDirectory(prefix="pikaflow optimizer ") as temporary:
        directory = pathlib.Path(temporary)
        def executions(path: pathlib.Path):
            yield [compiler, "interpret", str(path)]
            if native:
                for level in ("-O0", "-O2"):
                    yield [compiler, "run", str(path), level]
                    executable = directory / (path.stem + level + (".exe" if sys.platform == "win32" else ""))
                    built = run([compiler, "build", str(path), level, "-o", str(executable)])
                    if built.returncode:
                        raise AssertionError(built.stdout + built.stderr)
                    yield [str(executable)]
        for name, source, expected in cases:
            path = directory / (name + ".pf")
            path.write_text(textwrap.dedent(source).lstrip(), encoding="utf-8")
            for command in executions(path):
                result = run(command)
                if result.returncode or result.stdout.replace("\r\n", "\n") != expected:
                    raise AssertionError(f"{command}\nexpected {expected!r}\n{result.stdout}\n{result.stderr}")
                completed += 1
        for name, expression_text, diagnostic in traps:
            path = directory / ("trap_" + name + ".pf")
            path.write_text(f"fun main() -> unit:\n    print({expression_text})\n", encoding="utf-8")
            for command in executions(path):
                result = run(command)
                location = re.escape(path.name) + r":2:\d+: runtime error:"
                if result.returncode != 1 or result.stdout or diagnostic not in result.stderr or not re.search(location, result.stderr):
                    raise AssertionError(f"{command}\nmissing original checked failure/location\n{result.stdout}\n{result.stderr}")
                completed += 1
        proof = directory / "constant_control.pf"
        baseline = run([compiler, "dump-ir", str(proof), "-O0"])
        optimized = run([compiler, "dump-ir", str(proof), "-O2"])
        if baseline.returncode or optimized.returncode:
            raise AssertionError(baseline.stderr + optimized.stderr)
        if not re.search(r"\bmul\b", baseline.stdout) or "jump.false" not in baseline.stdout:
            raise AssertionError("baseline typed IR does not exercise arithmetic/control flow")
        if re.search(r"\b(add|sub|mul|div|mod)\b", optimized.stdout) or "jump.false" in optimized.stdout:
            raise AssertionError("custom typed IR pipeline did not fold arithmetic and the constant branch\n" + optimized.stdout)
        if native:
            for command in ("emit-ir", "dump-asm"):
                before = run([compiler, command, str(proof), "-O0"])
                after = run([compiler, command, str(proof), "-O2"])
                if before.returncode or after.returncode or "pf_checked_mul" not in before.stdout or "pf_checked_mul" in after.stdout:
                    raise AssertionError(f"{command}: constant checked multiplication survived optimized lowering\n{after.stderr}")
    print(f"{completed} reference/JIT/native optimizer comparisons passed; typed IR" +
          (", LLVM IR and assembly effects verified" if native else " effect verified"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
