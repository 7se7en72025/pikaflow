"""Run compiler-resolved project tests in separate, bounded processes."""
from __future__ import annotations

import concurrent.futures
import fnmatch
import hashlib
import json
import math
import os
import pathlib
import signal
import subprocess
import tempfile
import threading
import time
import xml.etree.ElementTree as ET

import packages
from process_jobs import ProcessJob

OUTPUT_LIMIT = 64 * 1024
MAX_MODULES = 128
MAX_TESTS = 512


def _stop(process, job):
    if os.name == "nt":
        try:
            job.terminate()
        except OSError:
            pass
    else:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    if process.poll() is None:
        process.kill()


def bounded_run(command, timeout, output_limit=OUTPUT_LIMIT, cwd=None):
    start = time.monotonic()
    job = ProcessJob()
    process = None
    try:
        process = subprocess.Popen([str(item) for item in command], stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0,
            start_new_session=os.name != "nt", creationflags=job.creation_flags, cwd=cwd)
        job.start(process)
    except BaseException:
        if process is not None:
            _stop(process, job)
            process.wait(timeout=5)
            process.stdout.close()
            process.stderr.close()
        job.close()
        raise
    buffers = [bytearray(), bytearray()]
    lock, overflow = threading.Lock(), threading.Event()

    def collect(stream, target):
        try:
            while data := stream.read(8192):
                with lock:
                    remaining = output_limit - sum(map(len, buffers))
                    target.extend(data[:remaining])
                    if len(data) > remaining:
                        overflow.set()
                if overflow.is_set():
                    _stop(process, job)
                    break
        finally:
            stream.close()

    readers = [threading.Thread(target=collect, args=(stream, target), daemon=True)
               for stream, target in zip((process.stdout, process.stderr), buffers)]
    for reader in readers:
        reader.start()
    timed_out = False
    try:
        process.wait(timeout=max(0, timeout - (time.monotonic() - start)))
    except subprocess.TimeoutExpired:
        timed_out = True
        _stop(process, job)
        process.wait(timeout=5)
    finally:
        job.close()  # Also stops descendants when their original parent already exited.
        if os.name != "nt":
            _stop(process, job)
    for reader in readers:
        reader.join(timeout=1)
    incomplete = any(reader.is_alive() for reader in readers)
    if incomplete:
        _stop(process, job)
    return {"command": list(map(str, command)), "exit_code": process.returncode,
        "seconds": round(time.monotonic() - start, 6), "timeout": timed_out,
        "output_limit": overflow.is_set(), "incomplete_output": incomplete,
        "stdout": bytes(buffers[0]).decode("utf-8", errors="replace"),
        "stderr": bytes(buffers[1]).decode("utf-8", errors="replace")}


def modules(root):
    directory = root / "tests"
    if not directory.exists():
        return []
    if packages.linked(directory):
        raise ValueError("test source directories cannot be links")
    found, visited = [], 0
    for folder, subdirs, files in os.walk(directory, followlinks=False):
        parent = pathlib.Path(folder)
        visited += 1
        if visited > 10000:
            raise ValueError("test discovery exceeds 10,000 directories")
        subdirs[:] = sorted(name for name in subdirs if not name.startswith(".")
            and name.lower() not in ("build", "out", "node_modules")
            and not packages.linked(parent / name))
        for name in sorted(files):
            if name.startswith("test_") and name.endswith(".pf"):
                source = parent / name
                if packages.linked(source):
                    raise ValueError("test sources cannot be links")
                found.append(source.resolve())
                if len(found) > MAX_MODULES:
                    raise ValueError("test discovery exceeds 128 modules")
    return sorted(found)


def fingerprint(path, limit=16 * 1024 * 1024):
    if not path.is_file() or path.stat().st_size > limit:
        raise ValueError("test input is not a regular file within its size limit")
    digest = hashlib.sha256()
    total = 0
    with path.open("rb") as stream:
        while data := stream.read(65536):
            total += len(data)
            if total > limit:
                raise ValueError("test input grew beyond its size limit while reading")
            digest.update(data)
    return digest.hexdigest()


def discover(compiler, root, pattern):
    cases, failures, sources = [], [], {}
    for source in modules(root):
        result = bounded_run([compiler, "symbols", source, "--library"], 30, 4 * 1024 * 1024, cwd=root)
        if result["exit_code"] or result["timeout"] or result["output_limit"] or result["incomplete_output"]:
            failures.append({"file": source.relative_to(root).as_posix(),
                             "message": result["stderr"] or "compiler metadata collection failed"})
            continue
        value = json.loads(result["stdout"])
        for name in value["sources"]:
            path = pathlib.Path(name).resolve()
            if path not in sources:
                if len(sources) >= 2048:
                    raise ValueError("test source graph exceeds 2,048 files")
                sources[path] = fingerprint(path)
        for symbol in value["symbols"]:
            if (symbol["kind"] != "function" or not symbol["name"].startswith("test_")
                    or pathlib.Path(symbol["definition"]["file"]).resolve() != source):
                continue
            name = source.relative_to(root).as_posix() + "::" + symbol["name"]
            if not (fnmatch.fnmatchcase(name, pattern) or fnmatch.fnmatchcase(symbol["name"], pattern)):
                continue
            case = {"id": name, "name": symbol["name"], "file": str(source),
                    "line": symbol["definition"]["line"]}
            if symbol["type"] != "fn()->unit" or symbol.get("generic", False):
                case["collection_error"] = "test functions must be non-generic fun() -> unit"
            cases.append(case)
            if len(cases) > MAX_TESTS:
                raise ValueError("test discovery exceeds 512 functions")
    return sorted(cases, key=lambda case: case["id"]), failures, sources


def execute(compiler, case, backend, optimization, timeout, cwd=None):
    if "collection_error" in case:
        return {**case, "status": "failed", "seconds": 0,
                "message": case["collection_error"], "stdout": "", "stderr": ""}
    try:
        with tempfile.TemporaryDirectory(prefix="pikaflow-test-") as temporary:
            arguments = [compiler, "interpret" if backend == "interpret" else "run", case["file"],
                         "--entry", case["name"], "-O" + optimization]
            stages = []
            if backend == "native":
                binary = pathlib.Path(temporary) / ("test.exe" if os.name == "nt" else "test")
                built = bounded_run([compiler, "build", case["file"], "--entry", case["name"],
                                     "-O" + optimization, "-o", binary], timeout, cwd=cwd)
                stages.append(built)
                if (built["exit_code"] == 0 and not built["timeout"] and
                        not built["output_limit"] and not built["incomplete_output"]):
                    stages.append(bounded_run([binary], timeout, cwd=cwd))
            else:
                stages.append(bounded_run(arguments, timeout, cwd=cwd))
            success = all(stage["exit_code"] == 0 and not stage["timeout"] and
                not stage["output_limit"] and not stage["incomplete_output"] for stage in stages)
            message = ""
            if not success:
                stage = stages[-1]
                message = ("test timed out" if stage["timeout"] else "test exceeded output limit"
                    if stage["output_limit"] else "test output streams did not close"
                    if stage["incomplete_output"] else "process exited with status " + str(stage["exit_code"]))
            return {**case, "status": "passed" if success else "failed",
                "seconds": round(sum(stage["seconds"] for stage in stages), 6), "message": message,
                "stdout": "".join(stage["stdout"] for stage in stages),
                "stderr": "".join(stage["stderr"] for stage in stages), "stages": stages}
    except (OSError, subprocess.SubprocessError) as error:
        return {**case, "status": "failed", "seconds": 0, "message": str(error), "stdout": "", "stderr": ""}


def protect_report(path, root, sources):
    absolute = path.absolute()
    resolved = path.resolve()
    if (path.is_symlink() or resolved.suffix.lower() == ".pf"
            or resolved.name.lower() in ("pika.json", "pika.lock")):
        raise ValueError("test report cannot replace source or project metadata")
    for candidate in (absolute, resolved):
        if candidate.is_relative_to(root) and any(part.startswith(".") for part in candidate.relative_to(root).parts):
            raise ValueError("test report cannot overwrite project caches or hidden metadata")
    for source in sources:
        if resolved == source or (path.exists() and source.exists() and os.path.samefile(path, source)):
            raise ValueError("test report would overwrite a compiler source")


def atomic_report(path, text):
    if not path.parent.is_dir():
        raise ValueError("test report parent directory must exist")
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n",
                                         dir=path.parent, prefix=".pika-report-", delete=False) as stream:
            temporary = pathlib.Path(stream.name)
            stream.write(text)
        os.replace(temporary, path)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def protect_reports(paths, root, sources):
    for path in paths:
        protect_report(path, root, sources)
    if len(paths) == 2 and (paths[0].resolve() == paths[1].resolve() or
        (all(path.exists() for path in paths) and os.path.samefile(*paths))):
        raise ValueError("JSON and JUnit reports must use different files")


def junit_report(report):
    def xml_text(value):
        return "".join(char if char in "\t\r\n" or 32 <= ord(char) <= 0xD7FF or
                       0xE000 <= ord(char) <= 0xFFFD or 0x10000 <= ord(char) <= 0x10FFFF
                       else "\ufffd" for char in value)
    suite = ET.Element("testsuite", name=report["project"], tests=str(len(report["tests"]) + len(report["collection_errors"])),
        failures=str(report["failed"]), errors=str(len(report["collection_errors"])))
    for test in report["tests"]:
        case = ET.SubElement(suite, "testcase", name=xml_text(test["id"]), time=str(test["seconds"]))
        if test["status"] == "failed":
            ET.SubElement(case, "failure", message=xml_text(test["message"])).text = xml_text(test["stderr"])
        if test["stdout"]:
            ET.SubElement(case, "system-out").text = xml_text(test["stdout"])
        if test["stderr"]:
            ET.SubElement(case, "system-err").text = xml_text(test["stderr"])
    for error in report["collection_errors"]:
        case = ET.SubElement(suite, "testcase", name=xml_text(error["file"]))
        ET.SubElement(case, "error", message="test collection failed").text = xml_text(error["message"])
    return ET.tostring(suite, encoding="unicode", xml_declaration=True) + "\n"


def run_project(compiler, args):
    if not math.isfinite(args.timeout) or not 0 < args.timeout <= 600:
        raise ValueError("test timeout must be finite and in (0, 600] seconds")
    if not 1 <= args.jobs <= 8:
        raise ValueError("test jobs must be between 1 and 8")
    if args.list and (args.json_report is not None or args.junit is not None):
        raise ValueError("--list does not write execution reports")
    root = packages.project_root(args.project)
    packages.lock(root, verify=True)
    project_name = packages.manifest(root)["name"]
    compiler = pathlib.Path(compiler).resolve()
    compiler_hash = fingerprint(compiler, 512 * 1024 * 1024)
    version = bounded_run([compiler, "--version"], 10, cwd=root)
    if version["exit_code"] or version["timeout"] or version["output_limit"] or version["incomplete_output"]:
        raise ValueError("cannot identify the test compiler")
    cases, errors, sources = discover(compiler, root, args.filter)
    for metadata in (root / "pika.json", root / "pika.lock"):
        sources[metadata] = fingerprint(metadata)
    report_paths = [path for path in (args.json_report, args.junit) if path is not None]
    protect_reports(report_paths, root, {*sources, compiler})
    if args.list:
        for case in cases:
            print(case["id"])
        for error in errors:
            print(error["file"] + ": " + error["message"])
        if not cases and not errors:
            print("No matching tests found")
        return 0 if cases and not errors and not any("collection_error" in case for case in cases) else 1
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = [executor.submit(execute, compiler, case, args.backend, args.O, args.timeout, root) for case in cases]
        tests = [future.result() for future in futures]
    changed = []
    for path, digest in sources.items():
        try:
            if fingerprint(path) != digest:
                changed.append(str(path))
        except (OSError, ValueError):
            changed.append(str(path))
    if changed:
        errors.append({"file": "<source snapshot>", "message": "sources changed during testing: " + ", ".join(changed)})
    try:
        if fingerprint(compiler, 512 * 1024 * 1024) != compiler_hash:
            errors.append({"file": str(compiler), "message": "compiler changed during testing"})
    except (OSError, ValueError):
        errors.append({"file": str(compiler), "message": "compiler changed during testing"})
    if not cases and not errors:
        errors.append({"file": "<discovery>", "message": "No matching tests found"})
    try:
        packages.lock(root, verify=True)
    except (OSError, ValueError) as error:
        errors.append({"file": "pika.lock", "message": str(error)})
    report = {"schema_version": 1, "project": project_name, "backend": args.backend,
        "compiler": {"path": str(compiler), "version": version["stdout"].strip(), "sha256": compiler_hash},
        "optimization": "-O" + args.O, "tests": tests, "collection_errors": errors,
        "passed": sum(test["status"] == "passed" for test in tests),
        "failed": sum(test["status"] == "failed" for test in tests)}
    for test in tests:
        print(test["status"].upper() + " " + test["id"] + (": " + test["message"] if test["message"] else ""))
        if test["status"] == "failed" and test["stderr"]:
            print(test["stderr"].rstrip())
    for error in errors:
        print("ERROR " + error["file"] + ": " + error["message"])
    print(f"{report['passed']} passed, {report['failed']} failed, {len(errors)} collection errors")
    report["success"] = bool(cases) and not report["failed"] and not errors
    # Test code may have changed report destinations; check again before any replacement.
    protect_reports(report_paths, root, {*sources, compiler})
    if args.json_report is not None:
        atomic_report(args.json_report, json.dumps(report, indent=2, ensure_ascii=True) + "\n")
    if args.junit is not None:
        atomic_report(args.junit, junit_report(report))
    return 0 if report["success"] else 1
