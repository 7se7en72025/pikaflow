"""Install/relocate the actual compiler and test installed assets and tooling."""
from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile


def run(arguments, *, env=None, success=True):
    result = subprocess.run([str(item) for item in arguments], capture_output=True, text=True,
                            encoding="utf-8", errors="replace", env=env, timeout=90)
    assert (result.returncode == 0) == success, (arguments, result.returncode, result.stdout, result.stderr)
    return result.stdout.replace("\r\n", "\n")


def main():
    build, cmake = pathlib.Path(sys.argv[1]).resolve(), sys.argv[2]
    with tempfile.TemporaryDirectory(prefix="pikaflow-installed-") as directory:
        root = pathlib.Path(directory)
        original, relocated = root / "original", root / "relocated-包"
        run([cmake, "--install", build, "--prefix", original])
        shutil.copytree(original, relocated)
        extension = ".exe" if os.name == "nt" else ""
        compiler = relocated / "bin" / ("pikaflow" + extension)
        numeric = relocated / "share/pikaflow/stdlib/numeric.pf"
        with numeric.open("a", encoding="utf-8") as stream:
            stream.write("\nexport fun relocation_marker() -> i64:\n    return 42\n")
        source = root / "installed.pf"
        source.write_text('import "std:numeric"\nfun worker() -> i64:\n'
            '    return relocation_marker()\nfun main() -> unit:\n'
            '    print(join(spawn(worker)))\n', encoding="utf-8", newline="\n")
        assert run([compiler, "interpret", source]) == "42\n"
        tools = relocated / "share/pikaflow/tools/pikaflow.py"
        run([sys.executable, tools, "fmt", source, "--check"])
        run([sys.executable, tools, "init", root / "application", "--name", "installed_example"])
        assert run([sys.executable, tools, "interpret", "--project", root / "application"]) == "Hello, PikaFlow!\n"
        environment = os.environ.copy()
        if os.name == "nt":
            import ctypes
            for key in list(environment):
                if key.upper() in ("LIB", "LIBPATH", "INCLUDE"):
                    del environment[key]
            windows = ctypes.create_unicode_buffer(32768)
            assert ctypes.windll.kernel32.GetWindowsDirectoryW(windows, len(windows))
            environment["PATH"] = str(pathlib.Path(windows.value) / "System32")
        executable = root / ("installed-native" + extension)
        run([compiler, "build", source, "-O2", "-o", executable], env=environment)
        assert run([executable]) == "42\n"
        library = relocated / "lib" / ("pikaflow_runtime.lib" if os.name == "nt" else "libpikaflow_runtime.a")
        library.write_bytes(b"invalid installed runtime\n")
        run([compiler, "build", source, "-o", root / ("must-fail" + extension)],
            env=environment, success=False)
    print("Installed/relocated stdlib, tasks, developer tools, native linking and runtime preference passed")


if __name__ == "__main__":
    main()
