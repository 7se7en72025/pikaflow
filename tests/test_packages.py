"""Actual pinned Git dependency workflows using local repositories only."""
from __future__ import annotations

import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import git_sources
import packages

COMPILER = pathlib.Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else None
GIT = shutil.which("git")


class ProjectTemplate(unittest.TestCase):
    def test_init_preserves_existing_tests_and_rejects_nondirectory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "tests").mkdir()
            source = root / "tests/test_main.pf"
            source.write_text("user-owned test\n")
            with self.assertRaisesRegex(ValueError, "replace"):
                packages.initialize(root, "preserved")
            self.assertEqual(source.read_text(), "user-owned test\n")
            self.assertFalse((root / "pika.json").exists())
            source.unlink()
            (root / "tests").rmdir()
            (root / "tests").write_text("user file")
            with self.assertRaisesRegex(ValueError, "tests"):
                packages.initialize(root, "preserved")
            self.assertEqual((root / "tests").read_text(), "user file")

    def test_init_ignores_generated_cache_without_replacing_existing_rules(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            fresh, existing = root / "fresh", root / "existing"
            packages.initialize(fresh, "fresh")
            self.assertEqual((fresh / ".gitignore").read_text(), ".pika/\nbuild/\nout/\n")
            existing.mkdir()
            (existing / ".gitignore").write_text("personal-rule\n", encoding="utf-8")
            packages.initialize(existing, "existing")
            self.assertEqual((existing / ".gitignore").read_text(), "personal-rule\n")


@unittest.skipUnless(GIT, "Git is required for local repository fixtures")
class GitPackages(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="pikaflow pinned git ")
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name).resolve()
        self.app = self.root / "app"
        packages.initialize(self.app, "app")

    def git(self, repo, *arguments, data=None):
        env = {key: value for key, value in os.environ.items() if not key.upper().startswith("GIT_")}
        env.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1", GIT_TERMINAL_PROMPT="0")
        result = subprocess.run([GIT, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                                 "-c", "commit.gpgSign=false", *arguments], cwd=repo,
                                input=data, capture_output=True, timeout=30, env=env)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        return result.stdout.decode("utf-8").strip()

    def repository(self, name, dependencies=None, object_format="sha1"):
        repo = self.root / name
        packages.initialize(repo, name)
        (repo / "lib.pf").write_text('export fun answer() -> i64:\n    return 42\n', encoding="utf-8")
        self.dependencies(repo, dependencies or {})
        self.git(repo, "init", "--quiet", "--object-format=" + object_format)
        return repo, self.commit(repo)

    def commit(self, repo):
        self.git(repo, "add", "--all")
        self.git(repo, "commit", "--quiet", "-m", "fixture")
        return self.git(repo, "rev-parse", "HEAD")

    def dependencies(self, repo, dependencies):
        data = packages.manifest(repo)
        data["dependencies"] = dependencies
        (repo / "pika.json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")

    def dependency(self, repo, revision):
        return {"git": "../" + repo.name, "rev": revision}

    def cached(self, alias="dep", project=None):
        project = project or self.app
        return project / packages.read_lock(project)[alias][0]

    def test_fetch_pins_commit_and_locked_clone_restores_cache(self):
        repo, revision = self.repository("origin")
        self.dependencies(self.app, {"dep": self.dependency(repo, revision)})
        packages.lock(self.app)
        original = (self.app / "pika.lock").read_bytes()
        cache = self.cached()
        self.assertEqual((cache / "lib.pf").read_bytes(), (repo / "lib.pf").read_bytes())
        (repo / "lib.pf").write_text('export fun answer() -> i64:\n    return 99\n', encoding="utf-8")
        self.commit(repo)
        packages.lock(self.app, verify=True)
        self.assertIn("42", (cache / "lib.pf").read_text())
        clone = self.root / "fresh_clone"
        shutil.copytree(self.app, clone, ignore=shutil.ignore_patterns(".pika"))
        with self.assertRaisesRegex(ValueError, "cache is missing"):
            packages.lock(clone, verify=True)
        restored = subprocess.run([sys.executable, str(ROOT / "tools/pikaflow.py"), "deps", "fetch", "--locked",
                                   "--project", str(clone)], capture_output=True, text=True, timeout=60)
        self.assertEqual(restored.returncode, 0, restored.stderr)
        self.assertEqual((clone / "pika.lock").read_bytes(), original)
        self.assertIn("42", (self.cached(project=clone) / "lib.pf").read_text())
        repo.rename(self.root / "origin_offline")
        with mock.patch("git_sources.subprocess.run", side_effect=AssertionError("offline check spawned Git")):
            packages.lock(clone, verify=True)
            packages.lock(clone, verify=True, offline=True)
        checked = subprocess.run([sys.executable, str(ROOT / "tools/pikaflow.py"), "deps", "check", "--locked", "--offline",
                                  "--project", str(clone)], env={**os.environ, "PATH": ""},
                                 capture_output=True, text=True, timeout=30)
        self.assertEqual(checked.returncode, 0, checked.stderr)
        if COMPILER:
            (clone / "src/main.pf").write_text('import "pkg:dep"\nfun main() -> unit:\n    print(answer())\n', encoding="utf-8")
            result = subprocess.run([sys.executable, str(ROOT / "tools/pikaflow.py"), "--compiler", str(COMPILER),
                                     "interpret", "--locked", "--project", str(clone)], capture_output=True, text=True, timeout=30)
            self.assertEqual((result.returncode, result.stdout), (0, "42\n"), result.stderr)

    def test_transitive_git_and_pinned_monorepo_path_dependencies(self):
        leaf, leaf_revision = self.repository("leaf")
        middle, _ = self.repository("middle")
        support = middle / "support"
        packages.initialize(support, "support")
        self.dependencies(support, {"leaf": {"git": "../../leaf", "rev": leaf_revision}})
        self.dependencies(middle, {"leaf": self.dependency(leaf, leaf_revision), "support": {"path": "support"}})
        revision = self.commit(middle)
        self.dependencies(self.app, {"middle": self.dependency(middle, revision)})
        packages.lock(self.app)
        self.assertEqual(set(packages.read_lock(self.app)), {"middle", "leaf", "support"})
        self.assertTrue(self.cached("support").is_relative_to(self.cached("middle")))
        with mock.patch("git_sources.subprocess.run", side_effect=AssertionError("offline check spawned Git")):
            packages.lock(self.app, verify=True)

    def test_cache_cannot_forge_its_own_content_or_origin(self):
        repo, revision = self.repository("origin")
        self.dependencies(self.app, {"dep": self.dependency(repo, revision)})
        packages.lock(self.app)
        cache = self.cached()
        metadata = cache / git_sources.META
        initial = metadata.read_text()
        source = cache / "lib.pf"
        source.write_bytes(b"forged content")
        with self.assertRaisesRegex(ValueError, "content differs"):
            packages.lock(self.app, verify=True)
        record = json.loads(initial)
        for entry in record["entries"]:
            if entry[1] == "lib.pf":
                entry[2] = git_sources._oid(b"blob", source.read_bytes(), revision)
        metadata.write_text(json.dumps(record))
        with self.assertRaisesRegex(ValueError, "tree does not belong"):
            packages.lock(self.app, verify=True)
        record = json.loads(initial)
        record["origin"] = "local:../another_origin"
        metadata.write_text(json.dumps(record))
        with self.assertRaisesRegex(ValueError, "origin or pinned revision"):
            packages.lock(self.app, verify=True)

    def test_floating_revisions_and_unsafe_transports_are_rejected(self):
        for revision in ("HEAD", "main", "v1.0", "abc123", "a" * 41):
            with self.subTest(revision=revision), self.assertRaisesRegex(ValueError, "exact full"):
                self.dependencies(self.app, {"dep": {"git": "../unused", "rev": revision}})
                packages.lock(self.app)
        for address in ("ssh://example/repo", "git@example.invalid:repo", "123.123:repo",
                        "file:///tmp/repo", "ext::evil", "https://user:secret@example/repo"):
            with self.subTest(address=address), self.assertRaises(ValueError):
                git_sources.origin(self.app, self.app, address)
        for address in ("https://example.invalid/repo.git", "../local_repo"):
            self.assertTrue(git_sources.origin(self.app, self.app, address)[0])

    def test_locked_manifest_change_does_not_fetch_an_unlocked_origin(self):
        repo, revision = self.repository("origin")
        self.dependencies(self.app, {"dep": self.dependency(repo, revision)})
        packages.lock(self.app)
        original = (self.app / "pika.lock").read_bytes()
        self.dependencies(self.app, {"dep": {"git": "https://example.invalid/new", "rev": "a" * 40}})
        with mock.patch("git_sources.subprocess.run", side_effect=AssertionError("unlocked origin fetched")):
            with self.assertRaisesRegex(ValueError, "not locked"):
                packages.lock(self.app, verify=True, fetch=True)
        self.assertEqual((self.app / "pika.lock").read_bytes(), original)

    def test_alias_conflicts_cycles_and_git_path_escape(self):
        first, first_revision = self.repository("first")
        second, second_revision = self.repository("second")
        middle, revision = self.repository("middle", {"shared": self.dependency(first, first_revision)})
        self.dependencies(self.app, {"middle": self.dependency(middle, revision), "shared": self.dependency(second, second_revision)})
        with self.assertRaisesRegex(ValueError, "conflicting"):
            packages.lock(self.app)
        for path, diagnostic in ((".", "cyclic"), ("../outside", "cannot escape")):
            self.dependencies(middle, {"loop": {"path": path}})
            revision = self.commit(middle)
            self.dependencies(self.app, {"middle": self.dependency(middle, revision)})
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, diagnostic):
                packages.lock(self.app)

    def test_git_symlinks_and_submodules_are_not_materialized(self):
        repo, revision = self.repository("origin")
        self.dependencies(self.app, {"dep": self.dependency(repo, revision)})
        for mode in ("120000", "160000"):
            oid = self.git(repo, "hash-object", "-w", "--stdin", data=b"../../outside") if mode == "120000" else revision
            self.git(repo, "update-index", "--add", "--cacheinfo", f"{mode},{oid},unsafe")
            self.git(repo, "commit", "--quiet", "-m", "unsafe fixture")
            bad_revision = self.git(repo, "rev-parse", "HEAD")
            self.dependencies(self.app, {"dep": self.dependency(repo, bad_revision)})
            with self.subTest(mode=mode), self.assertRaisesRegex(ValueError, "symlinks, submodules"):
                packages.lock(self.app)
            self.git(repo, "update-index", "--force-remove", "unsafe")
        self.assertFalse((self.root / "outside").exists())

    def test_attributes_hooks_and_git_environment_do_not_transform_sources(self):
        repo, _ = self.repository("origin")
        (repo / ".gitattributes").write_text("*.pf filter=boom\nlib.pf export-ignore\n", encoding="utf-8")
        revision = self.commit(repo)
        marker = self.root / "hook-executed"
        poison = self.root / "poison.gitconfig"
        poison.write_text(f'[filter "boom"]\n    smudge = echo BAD\n[core]\n    hooksPath = {marker.as_posix()}\n', encoding="utf-8")
        self.dependencies(self.app, {"dep": self.dependency(repo, revision)})
        with mock.patch.dict(os.environ, {"GIT_CONFIG_GLOBAL": str(poison), "GIT_CONFIG_COUNT": "1",
                                         "GIT_CONFIG_KEY_0": "uploadpack.packObjectsHook", "GIT_CONFIG_VALUE_0": "exit 123"}):
            packages.lock(self.app)
        self.assertEqual((self.cached() / "lib.pf").read_bytes(), (repo / "lib.pf").read_bytes())
        self.assertFalse(marker.exists())

    def test_sha256_git_commit_and_cache_symlink_guard(self):
        repo, revision = self.repository("sha256origin", object_format="sha256")
        self.assertEqual(len(revision), 64)
        self.dependencies(self.app, {"dep": self.dependency(repo, revision)})
        packages.lock(self.app)
        packages.lock(self.app, verify=True)
        another = self.root / "other_app"
        packages.initialize(another, "other_app")
        self.dependencies(another, {"dep": self.dependency(repo, revision)})
        outside = self.root / "outside"
        outside.mkdir()
        try:
            (another / ".pika").symlink_to(outside, target_is_directory=True)
        except OSError:
            return  # SHA-256 fixture still verified on hosts without symlink privileges.
        with self.assertRaisesRegex(ValueError, "cache paths"):
            packages.lock(another)
        self.assertEqual(list(outside.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
