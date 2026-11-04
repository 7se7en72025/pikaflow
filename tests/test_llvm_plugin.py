"""Exercise the real LLVM 24 plugin with opt, FileCheck, lli and native linking."""
from __future__ import annotations
import pathlib
import re
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 9:
        raise SystemExit("arguments: plugin opt FileCheck lli llc pikaflow runtime-library C++-driver")
    plugin, opt, filecheck, lli, llc, compiler, runtime, driver = sys.argv[1:]
    fixtures = pathlib.Path(__file__).resolve().parent / "llvm"

    def run(arguments, expected_exit=0, expected_output=None, input_text=None):
        result = subprocess.run(arguments, input=input_text, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=90)
        if result.returncode != expected_exit or (expected_output is not None and result.stdout != expected_output):
            raise AssertionError(f"{arguments}\nexit {result.returncode}, expected {expected_exit}\n{result.stdout}\n{result.stderr}")
        return result

    for tool in (opt, lli, llc):
        version = run([tool, "--version"]).stdout
        if not re.search(r"LLVM version 24\.", version):
            raise AssertionError(f"plugin tests require matching LLVM 24 tools: {version}")
    execution_runs = 0
    with tempfile.TemporaryDirectory(prefix="pikaflow LLVM plugin ") as temporary:
        directory = pathlib.Path(temporary)

        def transform(original, name, pipeline="pikaflow-checked-simplify"):
            destination = directory / (name + ".ll")
            run([opt, "-load-pass-plugin", plugin, "-passes=" + pipeline,
                 "-verify-each", "-S", str(original), "-o", str(destination)])
            return destination

        for name in ("checked_simplify", "checked_invalid_contract", "checked_untrusted"):
            fixture = fixtures / (name + ".ll")
            transformed = transform(fixture, name)
            run([filecheck, str(fixture)], input_text=transformed.read_text(encoding="utf-8"))
        wrapped = transform(fixtures / "checked_simplify.ll", "wrapped", "function(pikaflow-checked-simplify)")
        run([filecheck, str(fixtures / "checked_simplify.ll")], input_text=wrapped.read_text(encoding="utf-8"))
        loaded_only = transform(fixtures / "checked_simplify.ll", "loaded_only", "verify")
        if "call i64 @pf_checked_add(i64 %x, i64 0)" not in loaded_only.read_text(encoding="utf-8"):
            raise AssertionError("loading the plugin unexpectedly enabled a transform without selecting its pipeline")
        invalid = subprocess.run([opt, "-load-pass-plugin", plugin,
            "-passes=pikaflow-checked-simplify(unknown)", "-disable-output", str(fixtures / "checked_simplify.ll")],
            capture_output=True, text=True, timeout=30)
        if invalid.returncode == 0:
            raise AssertionError("a leaf pass unexpectedly accepted an inner pipeline")

        trusted = fixtures / "checked_execution.ll"
        transformed = transform(trusted, "execution")
        main_body = transformed.read_text(encoding="utf-8").split("define i32 @main()", 1)[1].split("\n}", 1)[0]
        if re.search(r"call i64 @pf_checked_(add|sub|mul|div|mod)", main_body):
            raise AssertionError("safe checked identities survived the actual plugin pass")
        if "call i64 @producer()" not in main_body:
            raise AssertionError("plugin removed the effectful argument producer")
        traps = ("trap_add", "trap_sub", "trap_mul", "trap_div_zero", "trap_div_overflow",
                 "trap_mod_zero", "trap_mod_overflow", "trap_before_zero")
        for module in (trusted, transformed):
            for entry in ("main",) + traps:
                run([lli, "-O0", "--entry-function=" + entry, str(module)], expected_exit=0 if entry == "main" else 77)
                execution_runs += 1
        untrusted = fixtures / "checked_untrusted.ll"
        changed = transform(untrusted, "untrusted_execution")
        for module in (untrusted, changed):
            run([lli, "-O0", str(module)])
            execution_runs += 1

        source = directory / "program.pf"
        source.write_text("fun simplify(value:i64)->i64:\n    return (value * 1) + 0\n"
                          "fun main()->unit:\n    print(simplify(42))\n", encoding="utf-8")
        run([compiler, "interpret", str(source)], expected_output="42\n")
        execution_runs += 1
        original = directory / "program-original.ll"
        run([compiler, "emit-ir", str(source), "-O0", "-o", str(original)])
        original_text = original.read_text(encoding="utf-8")
        if '"pikaflow.checked-arithmetic"="v1"' not in original_text:
            raise AssertionError("backend did not emit the shared pass's trusted runtime contract")
        # The O0 backend deliberately uses register slots; expose their constants
        # to the function pass without running the compiler's custom IR optimizer.
        optimized = transform(original, "program-optimized", "function(mem2reg,pikaflow-checked-simplify)")
        checked_calls = r"call i64 @pf_checked_(add|mul)\b"
        if not re.search(checked_calls, original_text) or re.search(checked_calls, optimized.read_text(encoding="utf-8")):
            raise AssertionError("plugin did not simplify the actual backend-emitted checked calls")
        for name, module in (("original", original), ("optimized", optimized)):
            object_path = directory / (name + ".o")
            executable = directory / name
            run([llc, "-filetype=obj", "-relocation-model=pic", str(module), "-o", str(object_path)])
            run([driver, str(object_path), runtime, "-o", str(executable), "-lm", "-ldl", "-pthread"])
            run([str(executable)], expected_output="42\n")
            execution_runs += 1
    print(f"LLVM plugin: 4 FileCheck pipelines and {execution_runs} execution checks passed; real PikaFlow IR roundtrip verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
