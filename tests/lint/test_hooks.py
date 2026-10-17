import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))
import cpp26_lint
import reflection_lint

PRECHECK = TOOLS / "cpp26_precheck.py"
POSTCHECK = TOOLS / "reflection_hook.py"
REFLECTION_SKILL = "reflect-dont-hand-write"
IDIOMS_SKILL = "cpp26-idioms"

HAND_SWITCH = 'constexpr auto f(E e) {\n  switch (e) {\n    case E::a: return "a";\n    case E::b: return "b";\n    case E::c: return "c";\n  }\n}\n'
BARE_TEMPLATE = "template <class Sink>\nvoid drain(Sink& out) {\n  out.write(1);\n}\n"
STD_INCLUDE = "module;\n#include <vector>\nexport module m;\n"
STRAY_COMMENT = "int value = 1;  // the answer\n"
CLEAN = "template <std::invocable F>\nauto run(F fn) { return fn(); }\n"


def run_hook(script, payload):
    done = subprocess.run([sys.executable, str(script)], input=json.dumps(payload), capture_output=True, text=True, cwd=ROOT)
    return done.returncode, done.stdout


def write_payload(path, content):
    return {"cwd": str(ROOT), "tool_name": "Write", "tool_input": {"file_path": str(ROOT / path), "content": content}}


def context_of(stdout):
    return json.loads(stdout)["hookSpecificOutput"]["additionalContext"]


class PrecheckTest(unittest.TestCase):
    def test_a_hand_written_switch_names_the_reflection_skill(self):
        code, out = run_hook(PRECHECK, write_payload("src/probe.cppm", HAND_SWITCH))
        self.assertEqual(code, 0)
        self.assertIn(REFLECTION_SKILL, context_of(out))
        self.assertIn("enum_name", context_of(out))

    def test_a_bare_called_template_names_the_idioms_skill(self):
        code, out = run_hook(PRECHECK, write_payload("src/probe.cppm", BARE_TEMPLATE))
        self.assertEqual(code, 0)
        self.assertIn(IDIOMS_SKILL, context_of(out))
        self.assertIn("unconstrained-template", context_of(out))

    def test_a_standard_include_is_named(self):
        _, out = run_hook(PRECHECK, write_payload("src/probe.cppm", STD_INCLUDE))
        self.assertIn("std-include", context_of(out))

    def test_a_stray_comment_is_named(self):
        _, out = run_hook(PRECHECK, write_payload("tests/unit/probe_test.cpp", STRAY_COMMENT))
        self.assertIn("[comment]", context_of(out))

    def test_clean_text_prints_nothing(self):
        self.assertEqual(run_hook(PRECHECK, write_payload("src/probe.cppm", CLEAN)), (0, ""))

    def test_an_edit_scans_only_the_new_string(self):
        payload = {"cwd": str(ROOT), "tool_input": {"file_path": str(ROOT / "src/probe.cppm"), "old_string": HAND_SWITCH, "new_string": CLEAN}}
        self.assertEqual(run_hook(PRECHECK, payload), (0, ""))

    def test_a_multi_edit_scans_every_new_string(self):
        edits = [{"old_string": "x", "new_string": CLEAN}, {"old_string": "y", "new_string": HAND_SWITCH}]
        payload = {"cwd": str(ROOT), "tool_input": {"file_path": str(ROOT / "src/probe.cppm"), "edits": edits}}
        self.assertIn(REFLECTION_SKILL, context_of(run_hook(PRECHECK, payload)[1]))

    def test_other_files_and_directories_pass_untouched(self):
        for path in ("README.md", "tools/probe.cppm", "src/probe.txt", "tests/constexpr/capabilities/supported/probe.cpp"):
            self.assertEqual(run_hook(PRECHECK, write_payload(path, HAND_SWITCH)), (0, ""), path)

    def test_a_broken_payload_never_blocks(self):
        done = subprocess.run([sys.executable, str(PRECHECK)], input="not json", capture_output=True, text=True)
        self.assertEqual((done.returncode, done.stdout), (0, ""))

    def test_an_allowed_exception_stays_quiet(self):
        text = "// Exception: wire word\nint value = 1;\n"
        self.assertEqual(run_hook(PRECHECK, write_payload("src/probe.cppm", text)), (0, ""))


class PostcheckTest(unittest.TestCase):
    def test_the_whole_file_report_covers_both_lints(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "src").mkdir()
            (root / "src/probe.cppm").write_text(HAND_SWITCH + BARE_TEMPLATE)
            payload = {"cwd": str(root), "tool_input": {"file_path": str(root / "src/probe.cppm")}}
            code, out = run_hook(POSTCHECK, payload)
        self.assertEqual(code, 0)
        self.assertIn(REFLECTION_SKILL, context_of(out))
        self.assertIn(IDIOMS_SKILL, context_of(out))


class SelfTestTest(unittest.TestCase):
    def test_the_reflection_lint_self_test_passes(self):
        self.assertEqual(reflection_lint.self_test(), 0)

    def test_the_cpp26_lint_self_test_passes(self):
        self.assertEqual(cpp26_lint.self_test(), 0)

    def test_a_stale_allowance_is_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "src").mkdir()
            (root / "src/a.cppm").write_text("int a;\n")
            allow = root / "allow.txt"
            allow.write_text("src/a.cppm|legacy-io|std::cout|pending: Tier 9\n")
            found = cpp26_lint.scan_paths(root, ["src"], allow)
        self.assertEqual([f.rule for f in found], ["stale-allowance"])

    def test_the_real_tree_is_clean_with_its_allowances(self):
        self.assertEqual(cpp26_lint.scan_paths(ROOT, cpp26_lint.DEFAULT_PATHS), [])
        self.assertEqual(reflection_lint.scan_paths(ROOT, reflection_lint.DEFAULT_PATHS), [])


if __name__ == "__main__":
    unittest.main()
