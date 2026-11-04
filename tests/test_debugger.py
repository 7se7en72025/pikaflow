"""Exercise source breakpoints, stepping, backtraces and cell watches with GDB."""
from __future__ import annotations
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile


def main() -> int:
    compiler = str(pathlib.Path(sys.argv[1]).resolve())
    debugger = sys.argv[2] if len(sys.argv) > 2 else shutil.which("gdb")
    if not debugger:
        raise RuntimeError("GDB is required for the debugger integration test")
    cases = [
        ("locals", """fun calculate(seed: i64) -> i64:
    var value = seed + 1
    value = value + 1
    let fraction = 1.5
    let flag = true
    let twice = value * 2
    print(twice)
    return twice

fun main() -> unit:
    print(calculate(19))
""", 7, {"seed": "19", "value": "21", "fraction": "1.5", "flag": "true", "twice": "42"}),
        ("capture", """fun main() -> unit:
    var captured = 40
    fun inner() -> i64:
        captured = captured + 2
        print(captured)
        return captured
    print(inner())
""", 5, {"captured": "42"}),
    ]
    with tempfile.TemporaryDirectory(prefix="pikaflow-debugger-") as temporary:
        directory = pathlib.Path(temporary)
        for name, source, line, watches in cases:
            path = directory / (name + ".pf")
            executable = directory / name
            path.write_text(source, encoding="utf-8")
            built = subprocess.run([compiler, "build", str(path), "--debug", "-O0", "-o", str(executable)],
                                   capture_output=True, text=True, timeout=90)
            if built.returncode:
                raise AssertionError(built.stdout + built.stderr)
            commands = ["set pagination off", "set confirm off", f"break {path.name}:{line}", "run", "bt",
                        "info args", "info locals", "next", "continue"]
            arguments = [debugger, "-q", "-batch"]
            for command in commands:
                arguments += ["-ex", command]
            result = subprocess.run(arguments + [str(executable)], capture_output=True, text=True, timeout=30)
            transcript = result.stdout + result.stderr
            if result.returncode:
                raise AssertionError(transcript)
            for variable, value in watches.items():
                if not re.search(rf"\b{variable} = {re.escape(value)}(?:\s|$)", transcript):
                    raise AssertionError(f"watch {variable} should be {value}\n{transcript}")
            for expected in (f"at {path.name}:{line}", "#1", "pf_fn_main", "exited normally"):
                if expected not in transcript:
                    raise AssertionError(f"missing debugger evidence {expected!r}\n{transcript}")
            if not re.search(rf"(?m)^{line + 1}\s+return\b", transcript):
                raise AssertionError(f"next did not reach the following return statement\n{transcript}")
            print(f"{name}: source breakpoint, backtrace, watches and next passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
