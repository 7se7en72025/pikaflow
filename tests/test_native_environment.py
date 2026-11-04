"""Native linking works without a developer shell and preserves adjacent files."""
from __future__ import annotations
import os
import pathlib
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) not in (2, 3):
        return 2
    compiler = str(pathlib.Path(sys.argv[1]).resolve())
    export_fixture = pathlib.Path(sys.argv[2]).resolve() if len(sys.argv) == 3 else None
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

        if os.name == "nt":
            if export_fixture is None:
                raise AssertionError("Windows linker regressions require the static export fixture argument")
            payload = b"fun main() -> unit:\n    print(42)\n"

            def built(command):
                result = run(command)
                if result.returncode:
                    raise AssertionError(result.stdout + result.stderr)
                return result

            def refused(command, diagnostic):
                result = run(command)
                if result.returncode == 0 or diagnostic not in result.stderr:
                    raise AssertionError(f"expected protected-output refusal {diagnostic!r}: {result.stdout}\n{result.stderr}")

            # /DEBUG used to imply incremental linking and delete this loaded source.
            ilk_source = directory / "loaded.ilk"
            ilk_source.write_bytes(payload)
            ilk_output = directory / "loaded.exe"
            built([compiler, "build", str(ilk_source), "--debug", "-o", str(ilk_output)])
            assert ilk_source.read_bytes() == payload, "debug link deleted a loaded .ilk source"
            assert pathlib.Path(str(ilk_output) + ".pdb").read_bytes().startswith(b"Microsoft C/C++ MSF"), "debug database missing"
            original_pdb_source = directory / "source.pdb"
            original_pdb_source.write_bytes(payload)
            built([compiler, "build", str(original_pdb_source), "--debug", "-o", str(directory / "source.exe")])
            assert original_pdb_source.read_bytes() == payload, "debug link changed a loaded .pdb source"

            # A legacy sibling PDB/ILK is user data; only the explicitly planned PDB is written.
            legacy_pdb = output.with_suffix(".pdb")
            legacy_ilk = output.with_suffix(".ilk")
            legacy_pdb.write_bytes(original)
            legacy_ilk.write_bytes(original)
            built([compiler, "build", str(source), "--debug", "-o", str(output)])
            assert legacy_pdb.read_bytes() == original and legacy_ilk.read_bytes() == original
            assert b"hello.exe.pdb" in output.read_bytes(), "executable does not reference the planned debug database"

            # cl must not append .exe behind the caller's output-protection check.
            extension_source = directory / "extensionless.exe"
            extension_source.write_bytes(payload)
            extension_output = directory / "extensionless"
            built([compiler, "build", str(extension_source), "-o", str(extension_output)])
            assert extension_source.read_bytes() == payload, "implicit .exe suffix overwrote loaded source"
            assert extension_output.read_bytes().startswith(b"MZ"), "explicit extensionless output was not honored"

            # Export directives in a linked object used to overwrite sibling .lib/.exp files.
            foreign = (b'extern fun echo(value: c_i64) -> c_i64 = "pf_ffi_i64"\n'
                       b'fun main() -> unit:\n    unsafe:\n        print(echo(42))\n')
            for suffix in (".lib", ".exp"):
                name = "exported_" + suffix[1:]
                loaded = directory / (name + suffix)
                loaded.write_bytes(foreign)
                executable = directory / (name + ".exe")
                result = built([compiler, "build", str(loaded), "--link", str(export_fixture), "-o", str(executable)])
                assert loaded.read_bytes() == foreign, f"implicit {suffix} output overwrote a loaded source"
                executed = run([str(executable)])
                assert executed.returncode == 0 and executed.stdout.replace("\r\n", "\n") == "42\n", executed.stderr
                assert "exports.lib" in result.stdout, "export fixture did not exercise import-library generation"
                sibling = executable.with_suffix(".exp" if suffix == ".lib" else ".lib")
                assert not sibling.exists(), "linker wrote an unplanned sibling export artifact"

            pdb_source = directory / "protected.exe.pdb"
            pdb_source.write_bytes(payload)
            refused([compiler, "build", str(pdb_source), "--debug", "-o", str(directory / "protected.exe")], "overwrite")
            assert pdb_source.read_bytes() == payload, "planned PDB protection changed its loaded source"

            alias_exe = directory / "alias.exe"
            alias_pdb = pathlib.Path(str(alias_exe) + ".pdb")
            alias_exe.write_bytes(original)
            try:
                os.link(alias_exe, alias_pdb)
            except OSError:
                pass
            else:
                refused([compiler, "build", str(source), "--debug", "-o", str(alias_exe)], "outputs must differ")
                assert alias_exe.read_bytes() == original and alias_pdb.read_bytes() == original

            library_payload = export_fixture.read_bytes()
            input_library = directory / "protected.lib"
            input_library.write_bytes(library_payload)
            refused([compiler, "build", str(source), "--link", str(input_library), "-o", str(input_library)], "input library")
            assert input_library.read_bytes() == library_payload
            pdb_library = directory / "library-output.exe.pdb"
            pdb_library.write_bytes(library_payload)
            refused([compiler, "build", str(source), "--debug", "--link", str(pdb_library),
                     "-o", str(directory / "library-output.exe")], "input library")
            assert pdb_library.read_bytes() == library_payload
            assert not list(objects.glob("pikaflow-*.obj")), "native link leaked a temporary object"
            assert not list(objects.glob("pikaflow-link-*")), "native link leaked an export-artifact directory"
    print("Native environment, Unicode output, protected sidecars and cleanup checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
