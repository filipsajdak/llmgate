#!/usr/bin/env python3
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import cpp26_lint
import lint_common
import reflection_lint as lint

REFLECTION_SKILL = "reflect-dont-hand-write"
IDIOMS_SKILL = "cpp26-idioms"
CHECKED_ROOTS = ("src/", "app/", "tests/")


def advice(root, relative):
    text = (root / relative).read_text()
    reflection = [f for f in lint.scan_text(relative, text) if not lint_common.is_allowed(f, lint_common.load_allowances(lint.ALLOW_FILE))]
    idioms = [f for f in cpp26_lint.scan_text(relative, text) if not lint_common.is_allowed(f, lint_common.load_allowances(cpp26_lint.ALLOW_FILE))]
    if not reflection and not idioms:
        return ""
    lines = []
    if reflection:
        lines.append(f"Hand-written copy of a definition in {relative}. Derive it with reflection instead (skill: {REFLECTION_SKILL}):")
        lines.extend(f.render() for f in reflection)
        lines.append("If the text is a wire or protocol name that is NOT an enumerator or member name, add a justified line to tools/reflection_lint_allow.txt.")
    if idioms:
        lines.append(f"{relative} misses a C++26 idiom or breaks the comment rule (skill: {IDIOMS_SKILL}):")
        lines.extend(f.render() for f in idioms)
        lines.append("A real exception gets a '// Exception: <reason>' comment or a justified line in tools/cpp26_lint_allow.txt.")
    return "\n".join(lines)


def main():
    try:
        payload = json.load(sys.stdin)
        tool_input = payload.get("tool_input") or payload.get("toolInput") or {}
        path = pathlib.Path(tool_input.get("file_path", ""))
        root = pathlib.Path(payload.get("cwd") or ".").resolve()
        if path.suffix not in lint_common.SOURCE_SUFFIXES or not path.is_file():
            return 0
        relative = path.resolve().relative_to(root).as_posix()
        if not relative.startswith(CHECKED_ROOTS) or relative.startswith(cpp26_lint.EXCLUDED):
            return 0
        context = advice(root, relative)
        if context:
            print(json.dumps({"hookSpecificOutput": {"hookEventName": "PostToolUse", "additionalContext": context}}))
    except Exception:
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
