"""Native linking works without a developer shell and preserves adjacent files."""
from __future__ import annotations
import os
import pathlib
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 2:
        return 2
    compiler = str(pathlib.Path(sys.argv[1]).resolve())
    source = pathlib.Path(__file__).resolve().parents[1] / "examples" / "hello.pf"
    environment = os.environ.copy()
    for key in ("LIB", "LIBPATH", "INCLUDE"):
        environment.pop(key, None)
    environment["PATH"] = os.pathsep.join(
        item for item in environment.get("PATH", "").split(os.pathsep)
        if "visual studio" not in item.lower() and "windows kits" not in item.lower()
    )
    with tempfile.TemporaryDirectory(prefix="pikaflow native environment ") as temporary:
        directory = pathlib.Path(temporary)
        output = directory / "Unicode-\u03bb" / ("hello.exe" if os.name == "nt" else "hello")
        output.parent.mkdir()
        objects = directory / "objects"
        objects.mkdir()
        environment["TMP"] = str(objects)
        environment["TEMP"] = str(objects)
        environment["TMPDIR"] = str(objects)
        adjacent = pathlib.Path(str(output) + ".obj")
        original = b"user-owned object file must not be overwritten"
        adjacent.write_bytes(original)

        def run(command: list[str]) -> subprocess.CompletedProcess[str]:
            return subprocess.run(command, env=environment, capture_output=True, text=True,
                                  encoding="utf-8", errors="replace", timeout=90)

        build = run([compiler, "build", str(source), "-O2", "-o", str(output)])
        if build.returncode:
            raise AssertionError("normal environment build failed:\n" + build.stdout + build.stderr)
        executed = run([str(output)])
        assert executed.returncode == 0 and executed.stdout.replace("\r\n", "\n") == "42\n36\n", executed.stderr
        assert adjacent.read_bytes() == original, "native compilation overwrote adjacent user file"
        assert not list(objects.glob("pikaflow-*.obj")), "successful link leaked a temporary object"
        bad_library = directory / "invalid.lib"
        bad_library.write_bytes(b"not a native library")
        failed = run([compiler, "build", str(source), "--link", str(bad_library), "-o", str(output.parent / "failed.exe")])
        assert failed.returncode != 0, "invalid native library was accepted"
        assert not list(objects.glob("pikaflow-*.obj")), "failed link leaked a temporary object"
        assert adjacent.read_bytes() == original
    print("Native environment, Unicode output, preservation and cleanup checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
