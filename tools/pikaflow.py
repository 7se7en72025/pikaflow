#!/usr/bin/env python3
"""PikaFlow developer tools, using only Python's standard library."""
from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

from formatter import checked_format
import packages


def compiler_path(value: str | None) -> pathlib.Path:
    if value:
        path = pathlib.Path(value).resolve()
        if not path.is_file():
            raise ValueError(f"compiler not found: {path}")
        return path
    root = pathlib.Path(__file__).resolve().parents[1]
    extension = ".exe" if os.name == "nt" else ""
    for path in (root / "build/native" / ("pikaflow" + extension),
                 root / "build/interpreter" / ("pikaflow" + extension),
                 root.parent.parent / "bin" / ("pikaflow" + extension)):
        if path.is_file():
            return path
    installed = shutil.which("pikaflow")
    if installed:
        return pathlib.Path(installed).resolve()
    raise ValueError("compiler not found; pass --compiler PATH")


def atomic_write(path: pathlib.Path, text: str) -> None:
    if path.is_symlink():
        raise ValueError(f"formatting symbolic links is not supported: {path}")
    mode = path.stat().st_mode
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n",
                                         dir=path.parent, prefix=".pika-format-", delete=False) as out:
            temporary = pathlib.Path(out.name)
            out.write(text)
        os.chmod(temporary, mode)
        os.replace(temporary, path)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", help="compiled PikaFlow executable")
    commands = parser.add_subparsers(dest="command", required=True)
    fmt = commands.add_parser("fmt", help="format source files without changing lexer tokens")
    fmt.add_argument("files", nargs="+", type=pathlib.Path)
    fmt.add_argument("--check", action="store_true")
    init = commands.add_parser("init", help="create a project without replacing files")
    init.add_argument("directory", nargs="?", default=".", type=pathlib.Path)
    init.add_argument("--name")
    deps = commands.add_parser("deps", help="lock/fetch/check reproducible path and pinned Git dependencies")
    deps.add_argument("action", choices=("lock", "fetch", "check"))
    deps.add_argument("--project", default=".", type=pathlib.Path)
    deps.add_argument("--locked", action="store_true", help="preserve the existing dependency lock")
    deps.add_argument("--offline", action="store_true", help="never fetch a missing Git cache")
    commands.add_parser("lsp", help="start the stdio language server")
    tests = commands.add_parser("test", help="run compiler-resolved test_* functions in separate processes")
    tests.add_argument("--project", default=".", type=pathlib.Path)
    tests.add_argument("--backend", choices=("interpret", "jit", "native"), default="interpret")
    tests.add_argument("-O", choices=("0", "1", "2", "3"), default="0")
    tests.add_argument("--filter", default="*")
    tests.add_argument("--list", action="store_true")
    tests.add_argument("--timeout", type=float, default=30)
    tests.add_argument("--jobs", type=int, default=1)
    tests.add_argument("--json-report", type=pathlib.Path)
    tests.add_argument("--junit", type=pathlib.Path)
    for command in ("check", "interpret", "run", "build"):
        project = commands.add_parser(command, help=f"{command} the locked project entry")
        project.add_argument("--project", default=".", type=pathlib.Path)
        project.add_argument("-O", choices=("0", "1", "2", "3"), default="0")
        project.add_argument("-o", type=pathlib.Path)
        project.add_argument("--debug", action="store_true")
        project.add_argument("--locked", action="store_true", help="verify the lock offline (always enabled)")
    args = parser.parse_args()
    try:
        if args.command == "init":
            packages.initialize(args.directory, args.name)
            print(f"Created {args.directory.resolve()}")
        elif args.command == "deps":
            root = packages.project_root(args.project)
            verify = args.action != "lock" or args.locked
            packages.lock(root, verify=verify, fetch=args.action == "fetch", offline=args.offline)
            print("Dependencies verified" if verify else "Wrote pika.lock")
        elif args.command == "fmt":
            compiler = compiler_path(args.compiler)
            changes = []
            for path in dict.fromkeys(path.absolute() for path in args.files):
                if path.is_symlink():
                    raise ValueError(f"formatting symbolic links is not supported: {path}")
                source = path.read_bytes().decode("utf-8")
                formatted = checked_format(compiler, source)
                if source != formatted:
                    changes.append((path, formatted))
            for path, formatted in changes:
                print(path)
                if not args.check:
                    atomic_write(path, formatted)
            return 1 if args.check and changes else 0
        elif args.command == "lsp":
            from lsp import serve
            return serve(compiler_path(args.compiler))
        elif args.command == "test":
            from project_tests import run_project
            return run_project(compiler_path(args.compiler), args)
        else:
            root = packages.project_root(args.project)
            packages.lock(root, verify=True)
            data = packages.manifest(root)
            entry = (root / data.get("entry", "src/main.pf")).resolve()
            if not entry.is_relative_to(root):
                raise ValueError("entry must remain inside its project")
            command = [str(compiler_path(args.compiler)), args.command, str(entry), "-O" + args.O]
            if args.o:
                if pathlib.Path(os.path.abspath(args.o)).is_relative_to(root / ".pika") or args.o.resolve().is_relative_to((root / ".pika").resolve()):
                    raise ValueError("output cannot overwrite the dependency cache")
                command.extend(("-o", str(args.o.resolve())))
            if args.debug:
                command.append("--debug")
            return subprocess.run(command, timeout=300).returncode
        return 0
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"pikaflow-tools: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
