"""Deterministic CLI arithmetic differential checks against Python.

This is a bounded smoke test, not a coverage-guided fuzzing campaign. It varies
arithmetic trees, signs, parentheses, whitespace, comments, and structured/stack
syntax, then checks and interprets every generated program with a timeout.
"""
from __future__ import annotations

import argparse
import dataclasses
import pathlib
import random
import subprocess
import tempfile


@dataclasses.dataclass
class Expression:
    source: str
    postfix: list[str]
    value: int


def truncating_division(left: int, right: int) -> int:
    magnitude = abs(left) // abs(right)
    return -magnitude if (left < 0) != (right < 0) else magnitude


def generate(randomizer: random.Random, depth: int) -> Expression:
    if depth == 0 or randomizer.randrange(4) == 0:
        value = randomizer.randint(-20, 20)
        return Expression(str(value), [str(value)], value)
    left = generate(randomizer, depth - 1)
    right = generate(randomizer, depth - 1)
    operator = randomizer.choice(["+", "-", "*", "/", "%"])
    if operator in ("/", "%") and right.value == 0:
        right = Expression("1", ["1"], 1)
    if operator == "+":
        value = left.value + right.value
    elif operator == "-":
        value = left.value - right.value
    elif operator == "*":
        value = left.value * right.value
    else:
        quotient = truncating_division(left.value, right.value)
        value = quotient if operator == "/" else left.value - quotient * right.value
    return Expression(f"({left.source} {operator} {right.source})",
                      left.postfix + right.postfix + [operator], value)


def execute(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, encoding="utf-8", errors="replace",
                          capture_output=True, timeout=15, shell=False)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--seed", type=int, default=20261104)
    parser.add_argument("--cases", type=int, default=30)
    options = parser.parse_args()
    if not 1 <= options.cases <= 10000:
        parser.error("--cases must be between 1 and 10000")
    compiler = str(options.compiler.resolve())
    randomizer = random.Random(options.seed)
    with tempfile.TemporaryDirectory(prefix="pikaflow differential ") as temporary:
        directory = pathlib.Path(temporary)
        for index in range(options.cases):
            expression = generate(randomizer, 3)
            if index % 2:
                spacing = randomizer.choice([" ", "  ", "   "])
                body = "let result = stack() -> i64:\n        " + spacing.join(expression.postfix)
            else:
                body = "let result = " + expression.source
            source = f"# seed={options.seed} case={index}\nfun main() -> unit:\n    {body}\n    print(result)\n"
            path = directory / f"case {index}.pf"
            path.write_text(source, encoding="utf-8")
            for command in ("check", "interpret"):
                result = execute([compiler, command, str(path)])
                if result.returncode:
                    raise AssertionError(f"seed={options.seed} case={index} {command} failed\n{source}\n{result.stderr}")
                if command == "interpret" and result.stdout.replace("\r\n", "\n") != f"{expression.value}\n":
                    raise AssertionError(f"seed={options.seed} case={index} expected={expression.value}, got={result.stdout!r}\n{source}")
    print(f"{options.cases} arithmetic differential programs passed (seed={options.seed})")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        raise SystemExit(str(error))
