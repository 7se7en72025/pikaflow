"""Benchmark-report checks without compiling or timing synthetic fake programs."""
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("pikaflow_benchmark", ROOT / "scripts/benchmark.py")
BENCHMARK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCHMARK)


class BenchmarkTests(unittest.TestCase):
    def test_output_mismatch_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "output mismatch"):
            BENCHMARK.check_output(subprocess.CompletedProcess([], 0, "wrong\n", ""), "42\n", "case")

    def test_disabled_llvm_is_rejected_before_building(self):
        response = subprocess.CompletedProcess([], 0, "PikaFlow; LLVM disabled\n", "")
        with mock.patch.object(sys, "argv", ["benchmark.py", sys.executable]), mock.patch.object(BENCHMARK, "execute", return_value=response) as execute:
            with self.assertRaisesRegex(RuntimeError, "LLVM enabled"):
                BENCHMARK.main()
            self.assertEqual(execute.call_count, 1)

    def test_report_contains_verified_results_and_repeated_timings(self):
        built = {}

        def fake_execute(command, timeout=120):
            if command[0] == "git":
                text = "test-source-revision\n"
            elif "--version" in command:
                text = "PikaFlow; LLVM enabled; test-target\n"
            elif "interpret" in command:
                text = BENCHMARK.WORKLOADS[pathlib.Path(command[2]).stem][1]
            elif "build" in command:
                built[command[-1]] = BENCHMARK.WORKLOADS[pathlib.Path(command[2]).stem][1]
                text = "built\n"
            else:
                text = built[command[0]]
            return subprocess.CompletedProcess(command, 0, text, "")

        with tempfile.TemporaryDirectory(prefix="pikaflow benchmark test ") as temporary:
            directory = pathlib.Path(temporary)
            compiler = directory / "compiler"
            compiler.write_bytes(b"test compiler")
            report = directory / "results.json"
            arguments = ["benchmark.py", str(compiler), "--repeats", "2", "--output", str(report)]
            with mock.patch.object(sys, "argv", arguments), mock.patch.object(BENCHMARK, "execute", side_effect=fake_execute):
                self.assertEqual(BENCHMARK.main(), 0)
            data = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(data["compiler"]["source_revision"], "test-source-revision")
            self.assertEqual(len(data["compiler"]["sha256"]), 64)
            self.assertEqual(len(data["workloads"]), 4)
            for workload in data["workloads"]:
                self.assertTrue(workload["output_verified"])
                self.assertEqual(len(workload["run_seconds"]), 2)
                self.assertIn(workload["flags"], [["-O0"], ["-O2"]])
                self.assertGreaterEqual(workload["median_seconds"], 0)


if __name__ == "__main__":
    unittest.main()
