"""Real CLI optimizer reports: verified analysis, no source execution or replacement."""
from __future__ import annotations

import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

COMPILER = pathlib.Path(sys.argv.pop(1)).resolve()
COUNTERS = {"constants_folded", "branches_folded", "unreachable_removed", "functions_skipped",
            "common_expressions_reused", "dead_results_removed", "liveness_skipped"}


class OptimizationReports(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="pikaflow optimizer report \u03bb ")
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name).resolve()
        self.library = self.root / "library.pf"
        self.library.write_text('export word repeated(i64,i64)->i64:\n    over over + rot rot + +\n'
            'export word discarded(i64)->i64:\n    dup 5 < drop\n', encoding="utf-8")
        self.source = self.root / "main-\U0001f600.pf"
        self.source.write_text('import "library.pf"\nfun main():\n    if 6*7==42:\n'
            '        let value=stack(20,22)->i64:\n            repeated\n        print(value)\n'
            '    else:\n        print(999)\n    let other=stack(42)->i64:\n        discarded\n'
            '    panic("report must not execute me")\n', encoding="utf-8")
        self.originals = {path: path.read_bytes() for path in (self.source,self.library)}

    def tearDown(self):
        for path, original in self.originals.items():
            self.assertEqual(path.read_bytes(), original)

    def run_cli(self, *arguments):
        return subprocess.run([str(COMPILER), *map(str,arguments)],capture_output=True,
                              text=True,encoding="utf-8",timeout=30)

    def report(self):
        result = self.run_cli("opt-report",self.source)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(result.stderr,"")
        value = json.loads(result.stdout)
        self.assertEqual(set(value),{"schema_version","source","before","after","statistics"})
        self.assertEqual(value["schema_version"],1)
        self.assertEqual(pathlib.Path(value["source"]),self.source)
        self.assertEqual(set(value["statistics"]),COUNTERS)
        for counts in (value["before"],value["after"]):
            self.assertEqual(set(counts),{"functions","instructions"})
            self.assertTrue(all(type(number) is int and number >= 0 for number in counts.values()))
        self.assertTrue(all(type(number) is int and number >= 0 for number in value["statistics"].values()))
        return value

    def test_schema_counters_and_exact_ir_counts_without_execution(self):
        report = self.report()
        for level, key in (("-O0","before"),("-O2","after")):
            result = self.run_cli("dump-ir",self.source,level)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(report[key],{"functions":len(re.findall(r"^fun ",result.stdout,re.M)),
                "instructions":len(re.findall(r"^\s*\d+\s",result.stdout,re.M))})
        stats = report["statistics"]
        self.assertGreater(stats["constants_folded"],0)
        self.assertGreater(stats["branches_folded"],0)
        self.assertGreater(stats["unreachable_removed"],0)
        self.assertGreater(stats["common_expressions_reused"],0)
        self.assertGreater(stats["dead_results_removed"],0)
        self.assertEqual((stats["functions_skipped"],stats["liveness_skipped"]),(0,0))
        self.assertEqual(report["before"]["functions"],report["after"]["functions"])
        self.assertEqual(report["before"]["instructions"]-report["after"]["instructions"],
                         stats["unreachable_removed"]+stats["dead_results_removed"])

    def test_file_stdout_and_repeated_reports_match(self):
        report = self.report()
        output = self.root/"report-\u03bb.json"
        result = self.run_cli("opt-report",self.source,"-o",output)
        self.assertEqual((result.returncode,result.stdout,result.stderr),(0,"",""))
        self.assertEqual(json.loads(output.read_text(encoding="utf-8")),report)
        self.assertEqual(self.report(),report)

    def test_rejects_all_incompatible_flags_without_replacing_output(self):
        output = self.root/"keep.json"
        output.write_bytes(b"keep")
        flags = [[f"-O{level}"] for level in range(4)] + [["--debug"],["-g"],["--library"],
            ["--entry","main"],["--overlay","unused"],["--index-source","unused"],
            ["--load","unused"],["--link","unused"],["--unknown"]]
        for flag in flags:
            with self.subTest(flag=flag):
                result=self.run_cli("opt-report",self.source,*flag,"-o",output)
                self.assertEqual(result.returncode,1)
                self.assertEqual(result.stdout,"")
                self.assertIn("opt-report supports only -o",result.stderr)
                self.assertEqual(output.read_bytes(),b"keep")
        for arguments in (["-o"],["-o",""]):
            result=self.run_cli("opt-report",self.source,*arguments)
            self.assertEqual(result.returncode,1)
            self.assertIn("output path",result.stderr)

    def test_protects_loaded_sources_hardlinks_metadata_and_cache(self):
        manifest, lock = self.root/"pika.json", self.root/"pika.lock"
        manifest.write_text('{"name":"reports","dependencies":{}}',encoding="utf-8")
        lock.write_text("pikaflow-lock-v1\n",encoding="utf-8")
        cache = self.root/".pika/report.json"
        cache.parent.mkdir()
        cache.write_bytes(b"cached")
        targets = [self.source,self.library,manifest,lock,cache]
        try:
            for index, target in enumerate(targets[:4]):
                alias=self.root/f"input-alias-{index}.json"
                os.link(target,alias)
                targets.append(alias)
        except OSError:
            pass  # Direct-input protections still run on filesystems without hard links.
        for target in targets:
            with self.subTest(target=target):
                original=target.read_bytes()
                result=self.run_cli("opt-report",self.source,"-o",target)
                self.assertEqual(result.returncode,1)
                self.assertIn("overwrite",result.stderr)
                self.assertEqual(target.read_bytes(),original)

    def test_invalid_source_and_write_failures_preserve_existing_files(self):
        output=self.root/"keep.json"
        output.write_bytes(b"keep")
        invalid=self.root/"invalid.pf"
        invalid.write_text("fun main()->i64:\n    return true\n",encoding="utf-8")
        result=self.run_cli("opt-report",invalid,"-o",output)
        self.assertEqual(result.returncode,1)
        self.assertEqual(result.stdout,"")
        self.assertIn("error:",result.stderr)
        self.assertEqual(output.read_bytes(),b"keep")
        for target in (self.root,self.root/"missing/report.json"):
            result=self.run_cli("opt-report",self.source,"-o",target)
            self.assertEqual(result.returncode,1)
            self.assertIn("cannot write output file",result.stderr)

    def test_checked_runtime_failure_remains_analysis_only(self):
        source=self.root/"overflow.pf"
        source.write_text("fun main():\n    print(9223372036854775807+1)\n",encoding="utf-8")
        result=self.run_cli("opt-report",source)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(json.loads(result.stdout)["statistics"]["constants_folded"],0)

    @unittest.skipUnless(sys.platform.startswith("linux") and pathlib.Path("/dev/full").exists(),"Linux full device")
    def test_buffered_output_failures_are_errors(self):
        result=self.run_cli("opt-report",self.source,"-o","/dev/full")
        self.assertEqual(result.returncode,1)
        self.assertIn("failed writing output file",result.stderr)
        with open("/dev/full","wb") as output:
            result=subprocess.run([str(COMPILER),"opt-report",str(self.source)],stdout=output,
                stderr=subprocess.PIPE,text=True,encoding="utf-8",timeout=30)
        self.assertEqual(result.returncode,1)
        self.assertIn("failed writing standard output",result.stderr)


if __name__ == "__main__":
    unittest.main()
