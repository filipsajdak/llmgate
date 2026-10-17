#!/usr/bin/env python3
import json
import os
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import cpp26_lint
import lint_common
import reflection_lint

CHECKED_ROOTS = ("src/", "app/", "tests/")
REFLECTION_SKILL = "reflect-dont-hand-write"
IDIOMS_SKILL = "cpp26-idioms"
CONTENT_KEYS = ("content", "new_string", "newString")
SKILL_OF_RULE = {**dict.fromkeys(reflection_lint.OWN_RULES, REFLECTION_SKILL), **dict.fromkeys(cpp26_lint.OWN_RULES, IDIOMS_SKILL)}
ALLOWANCES = (reflection_lint.ALLOW_FILE, cpp26_lint.ALLOW_FILE)


def new_texts(tool_input):
    for key in CONTENT_KEYS:
        if isinstance(tool_input.get(key), str):
            yield tool_input[key]
    for edit in tool_input.get("edits") or []:
        if isinstance(edit, dict):
            yield from (edit[key] for key in CONTENT_KEYS if isinstance(edit.get(key), str))


def relative_path(raw, root):
    path = pathlib.Path(raw)
    return (path.resolve().relative_to(root) if path.is_absolute() else path).as_posix()


def findings_in(relative, text):
    allowances = [a for file in ALLOWANCES for a in lint_common.load_allowances(file)]
    found = list(reflection_lint.scan_text(relative, text)) + list(cpp26_lint.scan_text(relative, text))
    return [f for f in found if not lint_common.is_allowed(f, allowances)]


def advice(relative, texts):
    found = [f for text in texts for f in findings_in(relative, text)]
    if not found:
        return ""
    skills = sorted({SKILL_OF_RULE[f.rule] for f in found})
    lines = [f"The new text for {relative} breaks repo rules (line numbers count inside the new text). Rewrite it before it lands; skills: {', '.join(skills)}."]
    lines.extend(f"[{SKILL_OF_RULE[f.rule]}] {f.render()}" for f in found)
    lines.append("A real exception gets a '// Exception: <reason>' comment, or a justified line in tools/reflection_lint_allow.txt or tools/cpp26_lint_allow.txt.")
    return "\n".join(lines)


def main():
    try:
        payload = json.load(sys.stdin)
        tool_input = payload.get("tool_input") or payload.get("toolInput") or {}
        raw = tool_input.get("file_path") or tool_input.get("path") or ""
        root = pathlib.Path(payload.get("cwd") or os.environ.get("CLAUDE_PROJECT_DIR") or ".").resolve()
        if pathlib.Path(raw).suffix not in lint_common.SOURCE_SUFFIXES:
            return 0
        relative = relative_path(raw, root)
        if not relative.startswith(CHECKED_ROOTS) or relative.startswith(cpp26_lint.EXCLUDED):
            return 0
        context = advice(relative, list(new_texts(tool_input)))
        if context:
            print(json.dumps({"hookSpecificOutput": {"hookEventName": "PreToolUse", "additionalContext": context}}))
    except Exception:
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
