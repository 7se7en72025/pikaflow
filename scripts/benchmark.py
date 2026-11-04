"""Build, verify, and measure small native PikaFlow workloads at O0 and O2.

Usage: python scripts/benchmark.py path/to/pikaflow --repeats 3 --output results.json
Timings include process startup and output capture. No speedup claim is inferred.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import platform
import statistics
import subprocess
import sys
import tempfile
import time


WORKLOADS = {
    "checked_loop": (
        "fun main() -> unit:\n"
        "    var total = 0\n"
        "    for value in range(50000):\n"
        "        total = total + value\n"
        "    print(total)\n",
        "1249975000\n",
    ),
    "recursive_fibonacci": (
        "fun fibonacci(value: i64) -> i64:\n"
        "    if value < 2:\n"
        "        return value\n"
        "    return fibonacci(value - 1) + fibonacci(value - 2)\n"
        "fun main() -> unit:\n"
        "    print(fibonacci(20))\n",
        "6765\n",
    ),
}


def execute(command: list[str], timeout: int = 120) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, text=True, encoding="utf-8", errors="replace",
                            capture_output=True, timeout=timeout, shell=False)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}): {command!r}\n{result.stderr}")
    return result


def check_output(result: subprocess.CompletedProcess[str], expected: str, context: str) -> None:
    actual = result.stdout.replace("\r\n", "\n")
    if actual != expected:
        raise RuntimeError(f"{context}: output mismatch: expected {expected!r}, received {actual!r}")


def digest(file: pathlib.Path) -> str:
    result = hashlib.sha256()
    with file.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--output", type=pathlib.Path, help="write JSON here; otherwise print JSON")
    options = parser.parse_args()
    if not 1 <= options.repeats <= 100:
        parser.error("--repeats must be between 1 and 100")
    compiler = options.compiler.resolve()
    version = execute([str(compiler), "--version"]).stdout.strip()
    if "LLVM enabled" not in version:
        raise RuntimeError("native benchmarks require a compiler built with LLVM enabled")
    source_root = pathlib.Path(__file__).resolve().parents[1]
    try:
        revision = execute(["git", "-C", str(source_root), "rev-parse", "HEAD"]).stdout.strip()
        dirty = bool(execute(["git", "-C", str(source_root), "status", "--porcelain", "--untracked-files=no"]).stdout.strip())
    except (OSError, RuntimeError):
        revision = None
        dirty = None
    report = {
        "schema_version": 1,
        "recorded_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "compiler": {"path": str(compiler), "version": version, "sha256": digest(compiler)},
        "benchmark_checkout": {"revision": revision, "dirty": dirty},
        "machine": {"platform": platform.platform(), "machine": platform.machine(),
                    "processor": platform.processor(), "logical_cpus": os.cpu_count(),
                    "python": platform.python_version()},
        "measurement_scope": "wall-clock seconds per complete process, including startup and captured output",
        "repeats": options.repeats,
        "workloads": [],
    }
    with tempfile.TemporaryDirectory(prefix="pikaflow benchmark ") as temporary:
        directory = pathlib.Path(temporary)
        for name, (source_text, expected) in WORKLOADS.items():
            source = directory / f"{name}.pf"
            source.write_text(source_text, encoding="utf-8")
            # Establish the reference result before compiling or recording timings.
            check_output(execute([str(compiler), "interpret", str(source)]), expected, name + " interpreter")
            for optimization in ("-O0", "-O2"):
                executable = directory / (name + optimization + (".exe" if sys.platform == "win32" else ""))
                started = time.perf_counter()
                execute([str(compiler), "build", str(source), optimization, "-o", str(executable)])
                build_seconds = time.perf_counter() - started
                check_output(execute([str(executable)]), expected, name + " warmup")
                durations = []
                for _ in range(options.repeats):
                    started = time.perf_counter()
                    result = execute([str(executable)])
                    durations.append(time.perf_counter() - started)
                    check_output(result, expected, name + " measured run")
                report["workloads"].append({
                    "name": name, "source": source_text, "expected_stdout": expected,
                    "flags": [optimization], "output_verified": True,
                    "build_seconds": build_seconds, "run_seconds": durations,
                    "median_seconds": statistics.median(durations),
                    "min_seconds": min(durations), "max_seconds": max(durations),
                })
    rendered = json.dumps(report, indent=2, ensure_ascii=False) + "\n"
    if options.output:
        options.output.parent.mkdir(parents=True, exist_ok=True)
        options.output.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"benchmark failed: {error}", file=sys.stderr)
        raise SystemExit(1)
