"""Exercise the real LLVM 24 plugin with opt, FileCheck, lli and native linking."""
from __future__ import annotations
import json
import pathlib
import re
import subprocess
import sys
import tempfile


def remark_records(text):
    """Read the bounded scalar/inline-location subset emitted by our LLVM test."""
    if len(text) > 65536:
        raise AssertionError("unexpectedly large optimization record")
    def scalar(value):
        value = value.strip()
        if value.startswith("'") and value.endswith("'"):
            return value[1:-1].replace("''", "'")
        if value.startswith('"') and value.endswith('"'):
            return json.loads(value)
        return value
    records = []
    for kind, body in re.findall(r"^--- !(Passed|Missed)\s*\n(.*?)^\.\.\.\s*$", text, re.M | re.S):
        record = {"kind": kind}
        for key in ("Pass", "Name", "Function"):
            match = re.search(r"^" + key + r":\s*(.+)$", body, re.M)
            if not match:
                raise AssertionError(f"missing {key} in optimization record: {body}")
            record[key] = scalar(match.group(1))
        location = re.search(r"^DebugLoc:\s*\{\s*File:\s*(.*?),\s*Line:\s*(\d+),\s*Column:\s*(\d+)\s*\}$", body, re.M)
        if not location:
            raise AssertionError(f"missing source location in optimization record: {body}")
        record.update(file=scalar(location.group(1)), line=int(location.group(2)), column=int(location.group(3)))
        record["args"] = {key: scalar(value) for key, value in
            re.findall(r"^\s*-\s*(Operation|Contract|Replacement|Reason):\s*(.*?)\s*$", body, re.M)}
        records.append(record)
    return records


def main() -> int:
    if len(sys.argv) != 9:
        raise SystemExit("arguments: plugin opt FileCheck lli llc pikaflow runtime-library C++-driver")
    plugin, opt, filecheck, lli, llc, compiler, runtime, driver = sys.argv[1:]
    fixtures = pathlib.Path(__file__).resolve().parent / "llvm"

    def run(arguments, expected_exit=0, expected_output=None, input_text=None, expected_error=None):
        result = subprocess.run(arguments, input=input_text, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=90)
        if (result.returncode != expected_exit or (expected_output is not None and result.stdout != expected_output)
                or (expected_error is not None and expected_error not in result.stderr)):
            raise AssertionError(f"{arguments}\nexit {result.returncode}, expected {expected_exit}\n{result.stdout}\n{result.stderr}")
        return result

    for tool in (opt, lli, llc):
        version = run([tool, "--version"]).stdout
        if not re.search(r"LLVM version 24\.", version):
            raise AssertionError(f"plugin tests require matching LLVM 24 tools: {version}")
    execution_runs = 0
    filecheck_runs = 0
    with tempfile.TemporaryDirectory(prefix="pikaflow LLVM plugin ") as temporary:
        directory = pathlib.Path(temporary)

        def transform(original, name, pipeline="pikaflow-checked-simplify"):
            destination = directory / (name + ".ll")
            run([opt, "-load-pass-plugin", plugin, "-passes=" + pipeline,
                 "-verify-each", "-S", str(original), "-o", str(destination)])
            return destination

        for name in ("checked_simplify", "checked_invalid_contract", "checked_untrusted",
                     "checked_legacy_effects", "checked_constants"):
            fixture = fixtures / (name + ".ll")
            transformed = transform(fixture, name)
            run([filecheck, str(fixture)], input_text=transformed.read_text(encoding="utf-8"))
            filecheck_runs += 1
        wrapped = transform(fixtures / "checked_simplify.ll", "wrapped", "function(pikaflow-checked-simplify)")
        run([filecheck, str(fixtures / "checked_simplify.ll")], input_text=wrapped.read_text(encoding="utf-8"))
        filecheck_runs += 1
        version2 = directory / "identities-v2.ll"
        version2.write_text((fixtures / "checked_simplify.ll").read_text(encoding="utf-8")
                           .replace('"v1"', '"v2"'), encoding="utf-8")
        changed = transform(version2, "identities-v2-transformed")
        run([filecheck, str(fixtures / "checked_simplify.ll")], input_text=changed.read_text(encoding="utf-8"))
        filecheck_runs += 1
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
        for name in ("checked_untrusted", "checked_legacy_effects"):
            original = fixtures / (name + ".ll")
            changed = transform(original, name + "-execution")
            for module in (original, changed):
                run([lli, "-O0", str(module)])
                execution_runs += 1

        # Execute both modules against the actual C++ runtime, through lli and
        # native llc objects. Each argv count selects one located fatal path.
        runtime_shared = directory / "runtime.so"
        run([driver, "-shared", "-Wl,--whole-archive", runtime, "-Wl,--no-whole-archive",
             "-o", str(runtime_shared), "-pthread"])
        constants = fixtures / "checked_constants.ll"
        changed = transform(constants, "constants-execution")
        diagnostics = ("integer addition overflow", "integer addition overflow",
            "integer subtraction overflow", "integer subtraction overflow",
            "integer multiplication overflow", "integer multiplication overflow",
            "integer multiplication overflow", "integer multiplication overflow",
            "integer division by zero", "integer division by zero", "integer division overflow",
            "integer remainder by zero", "integer remainder by zero", "integer remainder overflow")
        for index, module in enumerate((constants, changed)):
            object_path = directory / f"constants-{index}.o"
            executable = directory / f"constants-{index}"
            run([llc, "-filetype=obj", "-relocation-model=pic", str(module), "-o", str(object_path)])
            run([driver, str(object_path), runtime, "-o", str(executable), "-pthread"])
            for command in ([lli, "-O0", "--dlopen=" + str(runtime_shared), str(module)], [str(executable)]):
                run(command)
                execution_runs += 1
                for argument_count, diagnostic in enumerate(diagnostics, 1):
                    run(command + ["trap"] * argument_count, expected_exit=1,
                        expected_error="checked-plugin.ll:77:9: runtime error: " + diagnostic)
                    execution_runs += 1

        source = directory / "program.pf"
        source.write_text("fun simplify(value:i64)->i64:\n    return (value * 1) + 0\n"
                          "fun main()->unit:\n    print(simplify(42))\n"
                          "    print((20+22)*3-84)\n    print(-85/2)\n    print(-85%2)\n", encoding="utf-8")
        expected = "42\n42\n-42\n-1\n"
        run([compiler, "interpret", str(source)], expected_output=expected)
        execution_runs += 1
        original = directory / "program-original.ll"
        run([compiler, "emit-ir", str(source), "-O0", "-o", str(original)])
        original_text = original.read_text(encoding="utf-8")
        if '"pikaflow.checked-arithmetic"="v2"' not in original_text:
            raise AssertionError("backend did not emit the shared pass's trusted runtime contract")
        # The O0 backend deliberately uses register slots; expose their constants
        # to the function pass without running the compiler's custom IR optimizer.
        optimized = transform(original, "program-optimized", "function(mem2reg,pikaflow-checked-simplify)")
        checked_calls = r"call i64 @pf_checked_(add|sub|mul|div|mod)\b"
        if not re.search(checked_calls, original_text) or re.search(checked_calls, optimized.read_text(encoding="utf-8")):
            raise AssertionError("plugin did not simplify the actual backend-emitted checked calls")
        for name, module in (("original", original), ("optimized", optimized)):
            object_path = directory / (name + ".o")
            executable = directory / name
            run([llc, "-filetype=obj", "-relocation-model=pic", str(module), "-o", str(object_path)])
            run([driver, str(object_path), runtime, "-o", str(executable), "-lm", "-ldl", "-pthread"])
            run([str(executable)], expected_output=expected)
            execution_runs += 1
        remark_fixture = fixtures / "checked_remarks.ll"
        pass_filter = "^pikaflow-checked-simplify$"
        passed_flag = "-pass-remarks=" + pass_filter
        missed_flag = "-pass-remarks-missed=" + pass_filter

        def remarks(name, flags, pipeline="pikaflow-checked-simplify"):
            destination = directory / (name + ".ll")
            result = run([opt, "-load-pass-plugin", plugin, "-passes=" + pipeline, "-verify-each",
                          "-S", str(remark_fixture), "-o", str(destination)] + flags)
            return destination, result.stderr

        quiet, diagnostics = remarks("remarks-quiet", [])
        if diagnostics:
            raise AssertionError("pass emitted diagnostics by default: " + diagnostics)
        records_path = directory / "remarks.yaml"
        serialization = ["-pass-remarks-output=" + str(records_path), "-pass-remarks-format=yaml",
                         "-pass-remarks-filter=" + pass_filter]
        enabled, diagnostics = remarks("remarks-enabled", [passed_flag, missed_flag] + serialization)
        expected_records = {
            "identity": ("Passed", "IdentityFold", "pf_checked_sub", "v1", 11),
            "constant": ("Passed", "ConstantFold", "pf_checked_add", "v2", 21),
            "overflow": ("Missed", "CheckedFailure", "pf_checked_add", "v2", 31),
            "zero": ("Missed", "CheckedFailure", "pf_checked_div", "v2", 41),
        }
        records = remark_records(records_path.read_text(encoding="utf-8"))
        if len(records) != 4 or {record["Function"] for record in records} != set(expected_records):
            raise AssertionError(f"unexpected remarks, including a skipped call or function: {records}")
        for record in records:
            kind, name, operation, contract, line = expected_records[record["Function"]]
            if ((record["kind"], record["Name"], record["args"]["Operation"], record["args"]["Contract"], record["line"])
                    != (kind, name, operation, contract, line) or record["column"] != 7
                    or not record["file"].endswith("remarks.pf") or record["Pass"] != "pikaflow-checked-simplify"):
                raise AssertionError(f"incorrect structured remark: {record}")
            if not re.search(r"remarks\.pf:" + str(line) + r":7:.*" + operation + r".*" + contract, diagnostics):
                raise AssertionError(f"missing source-located stderr remark: {record}\n{diagnostics}")
        by_function = {record["Function"]: record for record in records}
        if (by_function["overflow"]["args"]["Reason"] != "signed overflow"
                or by_function["zero"]["args"]["Reason"] != "zero divisor"
                or by_function["constant"]["args"]["Replacement"] != "42"
                or by_function["identity"]["args"]["Replacement"] != "x"):
            raise AssertionError(f"incorrect remark values: {records}")
        for name, flags, folded, retained in (
                ("passed-only", [passed_flag], 2, 0), ("missed-only", [missed_flag], 0, 2),
                ("filtered", ["-pass-remarks=^unrelated$", "-pass-remarks-missed=^unrelated$"], 0, 0),
                ("serialized-only", serialization, 0, 0)):
            module, text = remarks(name, flags)
            if text.count("folded ") != folded or text.count("retained ") != retained or module.read_bytes() != quiet.read_bytes():
                raise AssertionError(f"remark flags changed diagnostics or IR unexpectedly: {name}\n{text}")
        if enabled.read_bytes() != quiet.read_bytes() or remark_records(records_path.read_text(encoding="utf-8")) != records:
            raise AssertionError("remark enablement changed IR or serialized-only records")
        skipped_records = directory / "remarks-not-run.yaml"
        _, text = remarks("remarks-not-run", [passed_flag, missed_flag,
            "-pass-remarks-output=" + str(skipped_records), "-pass-remarks-filter=" + pass_filter], "verify")
        if text or remark_records(skipped_records.read_text(encoding="utf-8")):
            raise AssertionError("loading the plugin emitted remarks without running its pass")
        for module in (quiet, enabled):
            for entry, diagnostic in (("main", None), ("overflow", "integer addition overflow"),
                                      ("zero", "integer division by zero")):
                run([lli, "-O0", "--dlopen=" + str(runtime_shared), "--entry-function=" + entry, str(module)],
                    expected_exit=0 if diagnostic is None else 1, expected_error=diagnostic)
                execution_runs += 1
    print(f"LLVM plugin: {filecheck_runs} FileCheck pipelines and {execution_runs} execution checks passed; "
          "v1 effects, v2 constants/fatal locations, real PikaFlow IR, standard stderr/YAML remarks verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
