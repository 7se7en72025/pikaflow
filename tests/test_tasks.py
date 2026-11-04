"""Task snapshot/lifetime semantics across interpreter, JIT and native execution."""
from __future__ import annotations

import pathlib
import re
import subprocess
import sys
import tempfile
import textwrap

from test_cli import require


def run(command):
    return subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=120)


def main():
    compiler = str(pathlib.Path(sys.argv[1]).resolve())
    version = run([compiler, "--version"])
    llvm = "LLVM enabled" in version.stdout
    count = 0
    rejected = 0
    examples = pathlib.Path(__file__).resolve().parents[1] / "examples"
    with tempfile.TemporaryDirectory(prefix="pikaflow tasks ") as temporary:
        root = pathlib.Path(temporary)

        def execute(name, source, expected=None, diagnostic=None, unordered=False, location=None):
            nonlocal count
            path = root / (name + ".pf")
            path.write_text(textwrap.dedent(source).lstrip(), encoding="utf-8")
            commands = [([compiler, "interpret", str(path)], "interpreter")]
            if llvm:
                for optimization in ("-O0", "-O2"):
                    commands.append(([compiler, "run", str(path), optimization], "JIT " + optimization))
                    output = root / (name + optimization + (".exe" if sys.platform == "win32" else ""))
                    built = run([compiler, "build", str(path), optimization, "-o", str(output)])
                    require(built.returncode == 0, f"{name}: native build failed: {built.stderr}\n{built.stdout}")
                    commands.append(([str(output)], "native " + optimization))
            for command, mode in commands:
                result = run(command)
                context = f"{name} ({mode})"
                if diagnostic is None:
                    require(result.returncode == 0, f"{context}: {result.stderr}")
                    actual = result.stdout.replace("\r\n", "\n")
                    if unordered:
                        require(sorted(actual.splitlines()) == sorted(expected.splitlines()), f"{context}: {actual!r}")
                    else:
                        require(actual == expected, f"{context}: expected {expected!r}, got {actual!r}")
                else:
                    require(result.returncode == 1 and diagnostic in result.stderr,
                            f"{context}: exit {result.returncode}: {result.stderr}")
                    if location is not None:
                        require(re.search(re.escape(path.name) + rf":{location}:\d+: runtime error:", result.stderr),
                                f"{context}: missing worker source location: {result.stderr}")
                count += 1

        execute("snapshot", (examples / "tasks.pf").read_text(encoding="utf-8"),
                "[42, 8]\n[42, 8]\n[9, 8]\n[42, 8]\n")
        execute("offsets", (examples / "pointer_offsets.pf").read_text(encoding="utf-8"), "42\n42\n")
        execute("scope_order", """
            fun launch() -> unit:
                let worker = fun() -> unit:
                    print("child")
                spawn(worker)
            fun main() -> unit:
                launch()
                print("parent")
        """, "child\nparent\n")
        execute("nested", """
            fun inner() -> i64:
                return 21
            fun outer() -> i64:
                let job = spawn(inner)
                return join(job) * 2
            fun main() -> unit:
                print(join(spawn(outer)))
        """, "42\n")
        execute("recursive_capture", """
            fun main() -> unit:
                fun factorial(n: i64) -> i64:
                    if n == 0:
                        return 1
                    return n * factorial(n - 1)
                let worker = fun() -> i64:
                    return factorial(6)
                print(join(spawn(worker)))
        """, "720\n")
        execute("returned_handle", """
            fun start() -> task[str]:
                let worker = fun() -> str:
                    return "complete"
                return spawn(worker)
            fun main() -> unit:
                let job = start()
                gc()
                print(join(job))
                gc()
                print(join(job))
        """, "complete\ncomplete\n")
        execute("unit_join", """
            fun worker() -> unit:
                print("unit result")
            fun main() -> unit:
                let job: task[unit] = spawn(worker)
                join(job)
        """, "unit result\n")
        execute("atomic_lines", """
            fun first() -> unit:
                var i = 0
                while i < 20:
                    print("first")
                    i = i + 1
            fun second() -> unit:
                var i = 0
                while i < 20:
                    print("second")
                    i = i + 1
            fun main() -> unit:
                spawn(first)
                spawn(second)
        """, "first\n" * 20 + "second\n" * 20, unordered=True)
        execute("hidden_pointer", """
            fun hide(memory: ptr[i64]) -> fn() -> i64:
                return fun() -> i64:
                    unsafe:
                        return load(memory, 0)
            fun main() -> unit:
                unsafe:
                    let memory = alloc_i64(1)
                    let worker = hide(memory)
                    spawn(worker)
                    free(memory)
        """, diagnostic="task snapshots cannot contain native pointers")
        execute("worker_failure", """
            fun worker() -> unit:
                panic("task exploded")
            fun main() -> unit:
                spawn(worker)
        """, diagnostic="task exploded", location=2)

        negative = [
            ("arity", """
                fun worker(n: i64) -> i64:
                    return n
                fun main() -> unit:
                    spawn(worker)
            """, "zero-argument"),
            ("pointer_return", """
                fun worker() -> ptr[i64]:
                    unsafe:
                        return alloc_i64(1)
                fun main() -> unit:
                    spawn(worker)
            """, "result cannot contain"),
            ("pointer_capture", """
                fun main() -> unit:
                    unsafe:
                        let memory = alloc_i64(1)
                        let worker = fun() -> i64:
                            unsafe:
                                return load(memory, 0)
                        spawn(worker)
            """, "captures cannot contain"),
            ("safe_offset", """
                fun move(memory: ptr[i64]) -> ptr[i64]:
                    return offset(memory, 1)
                fun main() -> unit:
                    pass
            """, "unsafe"),
        ]
        for name, source, diagnostic in negative:
            path = root / (name + ".pf")
            path.write_text(textwrap.dedent(source).lstrip(), encoding="utf-8")
            result = run([compiler, "check", str(path)])
            require(result.returncode != 0 and diagnostic in result.stderr, f"{name}: {result.stderr}")
            rejected += 1
    print(f"{count} task/pointer execution checks and {rejected} rejected programs passed")


if __name__ == "__main__":
    main()
