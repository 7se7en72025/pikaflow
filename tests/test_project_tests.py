"""Project test collection, isolation, backends and report preservation."""
from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import packages
import project_tests

COMPILER = pathlib.Path(sys.argv.pop(1)).resolve()
TOOL = ROOT / "tools/pikaflow.py"


class ProjectTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="pika project tests ")
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name).resolve()
        packages.initialize(self.root, "checks")
        (self.root / "tests").mkdir(exist_ok=True)
        (self.root / "tests/test_main.pf").unlink(missing_ok=True)
        self.source = self.root / "tests/test_core.pf"
        self.source.write_text('import "std:testing"\n'
            'fun main() -> unit:\n    panic("ordinary main must not execute")\n'
            'fun test_add() -> unit:\n    assert_equal(20 + 22, 42)\n'
            'fun test_float() -> unit:\n    assert_close(0.1 + 0.2, 0.3, 0.000001)\n', encoding="utf-8")

    def tool(self, *arguments, timeout=120):
        return subprocess.run([sys.executable, str(TOOL), "--compiler", str(COMPILER),
            "test", "--project", str(self.root), *map(str, arguments)],
            capture_output=True, text=True, encoding="utf-8", timeout=timeout)

    def test_collect_filter_and_real_interpreter_reports(self):
        before = self.source.read_bytes()
        listed = self.tool("--list", "--filter", "*add")
        self.assertEqual(listed.returncode, 0, listed.stderr)
        self.assertEqual(listed.stdout.strip(), "tests/test_core.pf::test_add")
        report, xml = self.root / "results.json", self.root / "results.xml"
        result = self.tool("--jobs", 2, "--json-report", report, "--junit", xml)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        value = json.loads(report.read_text())
        self.assertTrue(value["success"])
        self.assertEqual((value["passed"], value["failed"]), (2, 0))
        self.assertEqual([test["name"] for test in value["tests"]], ["test_add", "test_float"])
        suite = ET.parse(xml).getroot()
        self.assertEqual((suite.attrib["tests"], suite.attrib["failures"]), ("2", "0"))
        self.assertEqual(self.source.read_bytes(), before)

    def test_failed_case_isolated_and_next_case_runs(self):
        self.source.write_text('import "std:testing"\n'
            'fun test_00_fail() -> unit:\n    assert_equal(1, 2)\n'
            'fun test_10_pass() -> unit:\n    assert_true(true)\n', encoding="utf-8")
        path = self.root / "results.json"
        result = self.tool("--json-report", path)
        self.assertNotEqual(result.returncode, 0)
        value = json.loads(path.read_text())
        self.assertEqual((value["passed"], value["failed"]), (1, 1))
        self.assertIn("assert_equal failed", value["tests"][0]["stderr"])
        self.assertEqual(value["tests"][1]["status"], "passed")

    def test_wrong_signature_missing_filter_and_bad_module_fail_honestly(self):
        self.source.write_text('fun test_argument(value: i64) -> unit:\n    pass\n'
                               'fun test_generic[T]() -> unit:\n    pass\n', encoding="utf-8")
        report = self.root / "results.json"
        result = self.tool("--json-report", report)
        self.assertNotEqual(result.returncode, 0)
        value = json.loads(report.read_text())
        self.assertEqual(value["failed"], 2)
        self.assertTrue(all("non-generic" in test["message"] for test in value["tests"]))
        result = self.tool("--filter", "absent", "--json-report", report)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("No matching tests", result.stdout)
        self.assertFalse(json.loads(report.read_text())["success"])
        self.source.write_text('fun test_broken() -> unit:\n    print(missing)\n', encoding="utf-8")
        result = self.tool("--json-report", report)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(json.loads(report.read_text())["collection_errors"])

    def test_timeout_and_output_bounds(self):
        self.source.write_text('fun test_loop() -> unit:\n    while true:\n        pass\n', encoding="utf-8")
        report = self.root / "results.json"
        result = self.tool("--timeout", 0.25, "--json-report", report, timeout=20)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(json.loads(report.read_text())["tests"][0]["stages"][0]["timeout"])
        self.source.write_text('fun test_output() -> unit:\n    while true:\n'
                               '        print("0123456789012345678901234567890123456789")\n', encoding="utf-8")
        result = self.tool("--timeout", 10, "--json-report", report, timeout=25)
        self.assertNotEqual(result.returncode, 0)
        stage = json.loads(report.read_text())["tests"][0]["stages"][0]
        self.assertTrue(stage["output_limit"])
        self.assertLessEqual(len(stage["stdout"].encode()), project_tests.OUTPUT_LIMIT)

    def test_reports_never_replace_source_metadata_cache_or_each_other(self):
        source, manifest = self.source.read_bytes(), (self.root / "pika.json").read_bytes()
        for path in (self.source, self.root / "pika.json", self.root / "pika.lock", self.root / ".pika/report.json"):
            result = self.tool("--json-report", path)
            self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.source.read_bytes(), source)
        self.assertEqual((self.root / "pika.json").read_bytes(), manifest)
        alias = self.root / "source-alias.json"
        try:
            os.link(self.source, alias)
        except OSError:
            pass
        else:
            result = self.tool("--json-report", alias)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(alias.read_bytes(), source)
        report = self.root / "same.json"
        report.write_text("user report")
        result = self.tool("--json-report", report, "--junit", report)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(report.read_text(), "user report")

    def test_source_mutation_cannot_produce_successful_report(self):
        report = self.root / "results.json"
        args = argparse.Namespace(project=self.root, timeout=10, jobs=1, filter="*", list=False,
            backend="interpret", O="0", json_report=report, junit=None)
        def changed(compiler, case, *unused):
            self.source.write_text(self.source.read_text() + "# changed during execution\n")
            return {**case, "status": "passed", "seconds": 0, "message": "", "stdout": "", "stderr": ""}
        with mock.patch.object(project_tests, "execute", side_effect=changed), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(project_tests.run_project(COMPILER, args), 1)
        value = json.loads(report.read_text())
        self.assertFalse(value["success"])
        self.assertIn("sources changed", str(value["collection_errors"]))

    def test_report_destinations_are_rechecked_after_execution(self):
        report, xml = self.root / "results.json", self.root / "results.xml"
        report.write_text("original JSON")
        xml.write_text("original XML")
        args = argparse.Namespace(project=self.root, timeout=10, jobs=1, filter="*", list=False,
            backend="interpret", O="0", json_report=report, junit=xml)
        real_resolve, changed = pathlib.Path.resolve, [False]
        def resolve(path, *arguments, **keywords):
            return report if changed[0] and path == xml else real_resolve(path, *arguments, **keywords)
        def executed(compiler, case, *unused):
            changed[0] = True
            return {**case, "status": "passed", "seconds": 0, "message": "", "stdout": "", "stderr": ""}
        with mock.patch.object(pathlib.Path, "resolve", new=resolve), mock.patch.object(project_tests,
                "execute", side_effect=executed), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(ValueError, "different files"):
                project_tests.run_project(COMPILER, args)
        self.assertEqual(report.read_text(), "original JSON")
        self.assertEqual(xml.read_text(), "original XML")

    def test_junit_sanitizes_forbidden_xml_characters(self):
        report = {"project": "checks", "failed": 1, "collection_errors": [], "tests": [{
            "id": "test\x00name", "seconds": 0, "status": "failed", "message": "bad\x01message",
            "stdout": "valid \U0001f600\x00", "stderr": "bad\uffff"}]}
        suite = ET.fromstring(project_tests.junit_report(report))
        self.assertEqual(suite.find("testcase/system-out").text, "valid \U0001f600\ufffd")

    def test_exited_worker_does_not_leave_background_children(self):
        script = ('import subprocess,sys; '
            'child=subprocess.Popen([sys.executable,"-c","import time;time.sleep(30)"]); '
            'print(child.pid,flush=True)')
        result = project_tests.bounded_run([sys.executable, "-c", script], 5)
        self.assertEqual(result["exit_code"], 0, result)
        self.assertFalse(result["incomplete_output"], result)
        child = int(result["stdout"].strip())
        if os.name == "nt":
            import ctypes
            from ctypes import wintypes
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
            kernel.OpenProcess.restype = wintypes.HANDLE
            kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
            kernel.WaitForSingleObject.restype = wintypes.DWORD
            kernel.CloseHandle.argtypes = [wintypes.HANDLE]
            handle = kernel.OpenProcess(0x100000, False, child)
            if handle:
                try:
                    self.assertEqual(kernel.WaitForSingleObject(handle, 2000), 0, "background child survived its worker")
                finally:
                    kernel.CloseHandle(handle)
        elif sys.platform.startswith("linux"):
            status = pathlib.Path(f"/proc/{child}/stat")
            for _ in range(20):
                try:
                    gone = not status.exists() or status.read_text().split()[2] == "Z"
                except FileNotFoundError:
                    gone = True
                if gone:
                    break
                time.sleep(0.01)
            else:
                self.fail("background child survived its worker")

    def test_configuration_bounds(self):
        for option, value in (("--timeout", "nan"), ("--timeout", "inf"), ("--timeout", "0"),
                              ("--jobs", "0"), ("--jobs", "9")):
            result = self.tool(option, value)
            self.assertNotEqual(result.returncode, 0)
        ignored = self.root / "ignored.json"
        result = self.tool("--list", "--json-report", ignored)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(ignored.exists())

    def test_initialized_project_has_a_runnable_test(self):
        project = self.root / "fresh"
        packages.initialize(project, "fresh")
        result = subprocess.run([sys.executable, str(TOOL), "--compiler", str(COMPILER), "test",
            "--project", str(project)], capture_output=True, text=True, encoding="utf-8", timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("1 passed, 0 failed", result.stdout)

    def test_compiler_entry_contract_and_exit_status(self):
        self.source.write_text('fun main()->unit:\n    panic("wrong entry")\n'
            'fun status()->i64:\n    return 7\nfun generic[T]()->unit:\n    pass\n', encoding="utf-8")
        status = subprocess.run([str(COMPILER), "interpret", str(self.source), "--entry", "status"],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual((status.returncode, status.stdout), (7, ""), status.stderr)
        for command, name in (("check", "missing"), ("check", "generic"), ("check", "a.b"), ("symbols", "main")):
            result = subprocess.run([str(COMPILER), command, str(self.source), "--entry", name],
                                    capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0, (command, name, result.stdout))

    def test_transparent_unit_return_alias_is_collected_as_a_test(self):
        self.source.write_text('type Void = unit\nfun test_alias() -> Void:\n    return\n', encoding="utf-8")
        report = self.root / "results.json"
        result = self.tool("--json-report", report)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(report.read_text())["passed"], 1)

    def test_jit_and_native_use_selected_test_entries(self):
        version = subprocess.check_output([str(COMPILER), "--version"], text=True)
        if "LLVM enabled" not in version:
            self.skipTest("LLVM backend unavailable")
        for backend in ("jit", "native"):
            with self.subTest(backend=backend):
                result = self.tool("--backend", backend, "-O2", "--jobs", 2)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("2 passed, 0 failed", result.stdout)


if __name__ == "__main__":
    unittest.main()
