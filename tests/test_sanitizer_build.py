"""Check actual sanitizer flags/probes and the real compiler/runtime bridge."""
from __future__ import annotations

import json
import os
import pathlib
import re
import shlex
import subprocess
import sys
import tempfile


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def instrumentation(build: pathlib.Path, native: bool) -> int:
    project = pathlib.Path(__file__).resolve().parents[1]
    commands = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    found: set[str] = set()
    count = 0
    for item in commands:
        source = pathlib.Path(item["file"])
        if not source.is_absolute():
            source = pathlib.Path(item["directory"]) / source
        try:
            relative = source.resolve().relative_to(project).as_posix()
        except ValueError:
            continue
        if not relative.startswith(("src/", "tests/")) or source.suffix not in (".c", ".cpp"):
            continue
        arguments = item.get("arguments")
        if arguments is None:
            arguments = shlex.split(item["command"])
        sanitizers = set()
        for argument in arguments:
            if argument.startswith("-fsanitize="):
                sanitizers.update(argument.removeprefix("-fsanitize=").split(","))
            elif argument.startswith("-fno-sanitize="):
                disabled = set(argument.removeprefix("-fno-sanitize=").split(","))
                sanitizers.difference_update(disabled)
                if "all" in disabled:
                    sanitizers.clear()
        require({"address", "undefined"} <= sanitizers,
                f"missing actual ASan/UBSan flags for {relative}: {arguments}")
        require("-fno-sanitize-recover=all" in arguments,
                f"missing fatal sanitizer behavior for {relative}")
        require("-fno-omit-frame-pointer" in arguments,
                f"missing sanitizer frame pointers for {relative}")
        found.add(relative)
        count += 1
    expected = {"src/common.cpp", "src/lexer.cpp", "src/parser.cpp", "src/compiler.cpp",
                "src/ir.cpp", "src/optimizer.cpp", "src/interpreter.cpp", "src/runtime.cpp",
                "src/main.cpp", "tests/sanitizer_probe.cpp", "tests/ffi_fixture.c",
                "tests/ffi_host_tests.c", "tests/frontend_tests.cpp", "tests/ir_tests.cpp",
                "tests/optimizer_tests.cpp", "tests/runtime_tests.cpp"}
    if native:
        expected.update({"src/backend.cpp", "src/llvm/CheckedArithmeticPass.cpp",
                         "tests/backend_tests.cpp"})
    require(expected <= found, f"sanitizer compile database omitted {sorted(expected - found)}")
    return count


def execute(arguments: list[str], **kwargs) -> subprocess.CompletedProcess[str]:
    return subprocess.run(arguments, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=90, **kwargs)


def check_probes(probe: str, directory: pathlib.Path) -> None:
    for mode, diagnostic in (("address", "heap-buffer-overflow"),
                             ("undefined", "signed integer overflow")):
        private = directory / mode
        private.mkdir()
        environment = os.environ.copy()
        environment["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1:log_path=" + str(private / "asan")
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1:log_path=" + str(private / "ubsan")
        result = execute([probe, mode], env=environment, cwd=private)
        report_files = [path for path in sorted(private.iterdir()) if path.is_file()]
        reports = "\n".join(path.read_text(encoding="utf-8", errors="replace") for path in report_files)
        output = result.stderr + reports
        require(result.returncode != 0 and diagnostic in output,
                f"{mode} probe failed to demonstrate instrumentation: exit={result.returncode}\n{output}")
        require(("AddressSanitizer" if mode == "address" else "runtime error:") in output,
                f"{mode} probe did not produce its expected sanitizer diagnostic")
        print(f"Expected {mode} probe: exit={result.returncode}, {diagnostic}, "
              f"{len(report_files)} private report files")


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit("usage: test_sanitizer_build.py COMPILER PROBE BUILDDIR")
    compiler, probe, build = (pathlib.Path(value).resolve() for value in sys.argv[1:])
    version = execute([str(compiler), "--version"])
    require(version.returncode == 0, version.stderr)
    native = "LLVM enabled" in version.stdout
    count = instrumentation(build, native)
    with tempfile.TemporaryDirectory(prefix="pikaflow sanitizer validation ") as temporary:
        directory = pathlib.Path(temporary)
        check_probes(str(probe), directory)
        source = directory / "main.pf"
        source.write_text("""fun main() -> unit:
    var value = 20
    let data = [value, 22]
    fun bump() -> i64:
        value = value + 1
        return value
    print(bump() + data[1] - 1)
    gc()
    print(value)
    print(data[0] + data[1])
""", encoding="utf-8")

        def success(arguments: list[str], expected: str | None = None) -> None:
            result = execute(arguments, cwd=directory)
            require(result.returncode == 0,
                    f"{arguments}\nexit={result.returncode}\n{result.stdout}\n{result.stderr}")
            require(not re.search(r"AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer|runtime error:",
                                  result.stderr), f"unexpected sanitizer diagnostic: {result.stderr}")
            if expected is not None:
                require(result.stdout.replace("\r\n", "\n") == expected,
                        f"{arguments}\nexpected {expected!r}, got {result.stdout!r}")

        expected = "42\n21\n42\n"
        success([str(compiler), "check", str(source)])
        success([str(compiler), "interpret", str(source)], expected)
        if native:
            success([str(compiler), "run", str(source), "-O2"], expected)
            executable = directory / "native-program"
            # The configured backend bridge must supply sanitizer linking for its runtime.
            success([str(compiler), "build", str(source), "-O2", "-o", str(executable)])
            success([str(executable)], expected)
    print(version.stdout.strip())
    print(f"Sanitizer flags verified on {count} project C/C++ compile commands; actual ASan/UBSan probes passed")
    print("Instrumented project code exercised through interpreter" +
          (", LLVM lowering/JIT and native-O2 runtime linking" if native else " (LLVM disabled)"))
    print("Prebuilt LLVM and generated JIT/native instructions are outside the full-instrumentation claim")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
