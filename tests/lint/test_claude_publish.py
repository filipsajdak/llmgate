import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "claude_publish.sh"
GIT_ENV = {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.com", "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.com",
           "GIT_CONFIG_NOSYSTEM": "1", "HOME": "/nonexistent"}
BRANCH = "claude/run-42"
MAX_COMMITS = 20


def git(cwd, *args, check=True):
    done = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True, env={**os.environ, **GIT_ENV}, check=False)
    if check and done.returncode != 0:
        raise AssertionError(f"git {args} failed: {done.stderr}")
    return done.stdout.strip()


class Publish(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(self.tmp.name)
        self.origin = base / "origin.git"
        git(base, "init", "-q", "--bare", "-b", "main", str(self.origin))
        self.agent = base / "agent"
        git(base, "clone", "-q", str(self.origin), str(self.agent))
        (self.agent / "src").mkdir()
        (self.agent / "src" / "a.txt").write_text("one\n")
        (self.agent / "tools").mkdir()
        (self.agent / "tools" / "t.sh").write_text("echo\n")
        git(self.agent, "add", ".")
        git(self.agent, "commit", "-q", "-m", "base")
        git(self.agent, "push", "-q", "origin", "HEAD:main")
        self.base_sha = git(self.agent, "rev-parse", "HEAD")
        self.publisher = base / "publisher"
        git(base, "clone", "-q", str(self.origin), str(self.publisher))

    def tearDown(self):
        self.tmp.cleanup()

    def commit(self, path, text, message="agent change"):
        target = self.agent / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
        git(self.agent, "add", path)
        git(self.agent, "commit", "-q", "-m", message)

    def bundle(self):
        out = pathlib.Path(self.tmp.name) / "claude.bundle"
        git(self.agent, "bundle", "create", str(out), "HEAD", f"^{self.base_sha}")
        return out

    def run_script(self, bundle=None, base=None, branch=BRANCH, mode="dry"):
        done = subprocess.run(["sh", str(SCRIPT), str(bundle or self.bundle()), base or self.base_sha, branch, mode], cwd=self.publisher,
                              capture_output=True, text=True, env={**os.environ, **GIT_ENV}, check=False)
        return done.returncode, done.stdout + done.stderr

    def test_capability_an_allowed_change_is_verified_and_pushed_to_the_named_branch(self):
        self.commit("src/b.txt", "two\n")
        code, text = self.run_script(mode="push")
        self.assertEqual(code, 0, text)
        self.assertEqual(git(self.origin, "log", "--format=%s", "-1", BRANCH), "agent change")

    def test_limit_a_protected_path_is_refused_and_nothing_is_pushed(self):
        for path in ("tools/t.sh", ".github/workflows/x.yml", ".claude/settings.json", "ops/ci/linux.sh", "CLAUDE.md", "CMakeLists.txt", "tests/lint/t.py", ".gitmodules",
                     ".gitattributes", "conftest.py", "scripts/x.sh", "install/install.sh", "SECURITY.md", "cmake/x.cmake", "tests/release/t.py", "tests/install/t.ps1", "AGENTS.md"):
            with self.subTest(path=path):
                self.setUp()
                self.commit(path, "changed\n")
                code, text = self.run_script(mode="push")
                self.assertNotEqual(code, 0)
                self.assertIn("outside the directories", text)
                self.assertEqual(git(self.origin, "branch", "--list", BRANCH), "")

    def test_limit_a_token_in_a_message_or_in_the_change_is_refused(self):
        token = "ghs_" + "a" * 36
        self.commit("src/b.txt", "two\n", message=f"note {token}")
        code, text = self.run_script()
        self.assertNotEqual(code, 0)
        self.assertIn("token", text)
        self.setUp()
        self.commit("src/c.txt", f"url https://x-access-token:{token}@github.com/o/r.git\n")
        code, text = self.run_script()
        self.assertNotEqual(code, 0)
        self.assertIn("token", text)

    def test_limit_a_path_git_would_quote_cannot_hide_a_protected_directory(self):
        for path in ("tools/\u00e9.sh", 'tools/a"b.sh', "tools/a\nb.sh", "tools/check\t.py", "src/sp ace.txt", "src/caf\u00e9.txt", "ops/\u00e9/x.sh"):
            with self.subTest(path=path):
                if sys.platform == "win32" and any(char in path for char in '"\n\t'):
                    continue
                self.setUp()
                self.commit(path, "x\n")
                code, text = self.run_script(mode="push")
                self.assertNotEqual(code, 0)
                self.assertIn("character outside", text)
                self.assertEqual(git(self.origin, "branch", "--list", BRANCH), "")

    def test_capability_ordinary_file_names_with_dots_dashes_and_underscores_pass(self):
        self.commit("src/my-file_v2.test.cpp", "x\n")
        self.commit("docs/notes/2026-11-09.md", "x\n")
        code, text = self.run_script()
        self.assertEqual(code, 0, text)

    def test_limit_a_token_cannot_hide_in_history_a_binary_file_or_an_author_field(self):
        token = "ghs_" + "b" * 36
        self.commit("src/leak.txt", f"{token}\n")
        self.commit("src/leak.txt", "gone\n")
        code, text = self.run_script()
        self.assertNotEqual(code, 0, "a token added in one commit and removed in the next must still be refused")
        self.assertIn("token", text)
        self.setUp()
        blob = self.agent / "src" / "blob.bin"
        blob.write_bytes(b"\x00\x01\x02" + token.encode() + b"\x00")
        git(self.agent, "add", "src/blob.bin")
        git(self.agent, "commit", "-q", "-m", "binary")
        self.assertNotEqual(self.run_script()[0], 0)
        self.setUp()
        (self.agent / "src" / "c.txt").write_text("x\n")
        git(self.agent, "add", "src/c.txt")
        done = subprocess.run(["git", "commit", "-q", "-m", "named"], cwd=self.agent, capture_output=True, text=True,
                              env={**os.environ, **GIT_ENV, "GIT_AUTHOR_NAME": token}, check=False)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertNotEqual(self.run_script()[0], 0)
        self.setUp()
        self.commit("src/e.txt", "gho_" + "d" * 36 + "\n")
        self.assertNotEqual(self.run_script()[0], 0)
        self.setUp()
        self.commit("src/d.txt", "sk-ant-oat01-" + "c" * 40 + "\n")
        self.assertNotEqual(self.run_script()[0], 0)

    def test_limit_a_protected_path_touched_in_an_earlier_commit_is_refused_even_if_restored(self):
        self.commit("tools/t.sh", "changed\n")
        self.commit("tools/t.sh", "echo\n")
        code, text = self.run_script()
        self.assertNotEqual(code, 0)
        self.assertIn("outside the directories", text)

    def test_limit_a_merge_commit_is_refused(self):
        git(self.agent, "checkout", "-q", "-b", "side")
        self.commit("src/side.txt", "s\n")
        git(self.agent, "checkout", "-q", "-")
        self.commit("src/main.txt", "m\n")
        git(self.agent, "merge", "-q", "--no-ff", "-m", "merge", "side")
        code, text = self.run_script()
        self.assertNotEqual(code, 0)
        self.assertIn("merge", text)

    def test_limit_a_dot_prefixed_component_is_refused_even_inside_an_allowed_directory(self):
        for path in ("src/.env", "src/.gitattributes", "docs/.claude/settings.json", "tests/unit/.hidden/t.cpp", "mods/llmgate/.eslintrc", "src/a/.b"):
            with self.subTest(path=path):
                self.setUp()
                self.commit(path, "x\n")
                code, text = self.run_script(mode="push")
                self.assertNotEqual(code, 0)
                self.assertIn("dot-prefixed", text)
                self.assertEqual(git(self.origin, "branch", "--list", BRANCH), "")

    def test_capability_the_directories_claude_may_change_are_allowed(self):
        for path in ("src/x/y.cppm", "app/main.cpp", "docs/notes.md", "examples/a.jsonc", "mods/llmgate/hooks/a.ts", "tests/unit/t.cpp", "tests/e2e/f.sh",
                     "tests/constexpr/c.cpp", "tests/fuzz/f/a.cpp", "tests/network/n.py", "tests/support/s.cppm", "tests/compat/r.py", "README.md", "config.example.jsonc"):
            with self.subTest(path=path):
                self.setUp()
                self.commit(path, "x\n")
                code, text = self.run_script()
                self.assertEqual(code, 0, text)

    def blob(self, text):
        return subprocess.run(["git", "hash-object", "-w", "--stdin"], cwd=self.agent, input=text, capture_output=True, text=True,
                              env={**os.environ, **GIT_ENV}, check=True).stdout.strip()

    def test_limit_a_symlink_is_refused(self):
        for target in ("../../.git/config", "/etc/passwd", "../tools/t.sh"):
            with self.subTest(target=target):
                self.setUp()
                git(self.agent, "update-index", "--add", "--cacheinfo", f"120000,{self.blob(target)},src/link")
                git(self.agent, "commit", "-q", "-m", "link")
                code, text = self.run_script(mode="push")
                self.assertNotEqual(code, 0)
                self.assertIn("symlink", text)
                self.assertEqual(git(self.origin, "branch", "--list", BRANCH), "")

    def test_capability_an_executable_regular_file_passes(self):
        git(self.agent, "update-index", "--add", "--cacheinfo", f"100755,{self.blob('#!/bin/sh')},src/run.sh")
        git(self.agent, "commit", "-q", "-m", "executable")
        code, text = self.run_script()
        self.assertEqual(code, 0, text)

    def test_limit_a_submodule_is_refused(self):
        git(self.agent, "update-index", "--add", "--cacheinfo", f"160000,{self.base_sha},src/vendor/x")
        git(self.agent, "commit", "-q", "-m", "submodule")
        code, text = self.run_script()
        self.assertNotEqual(code, 0)
        self.assertIn("submodule", text)

    def test_limit_a_bad_branch_name_a_bad_base_and_too_many_commits_are_refused(self):
        self.commit("src/b.txt", "two\n")
        bundle = self.bundle()
        self.assertNotEqual(self.run_script(bundle, branch="main")[0], 0)
        self.assertNotEqual(self.run_script(bundle, branch="claude/run-1; rm -rf x")[0], 0)
        self.assertNotEqual(self.run_script(bundle, base="not-a-sha")[0], 0)
        for index in range(MAX_COMMITS):
            self.commit(f"src/n{index}.txt", "x\n")
        code, text = self.run_script()
        self.assertNotEqual(code, 0)
        self.assertIn("commits", text)

    def test_limit_a_bundle_that_does_not_build_on_the_base_is_refused(self):
        self.commit("src/b.txt", "two\n")
        bundle = self.bundle()
        other = pathlib.Path(self.tmp.name) / "other"
        git(pathlib.Path(self.tmp.name), "clone", "-q", str(self.origin), str(other))
        (other / "src" / "z.txt").write_text("z\n")
        git(other, "add", ".")
        git(other, "commit", "-q", "-m", "elsewhere")
        wrong_base = git(other, "rev-parse", "HEAD")
        code, text = self.run_script(bundle, base=wrong_base)
        self.assertNotEqual(code, 0)


if __name__ == "__main__":
    unittest.main()
