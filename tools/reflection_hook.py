#!/usr/bin/env python3
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import reflection_lint as lint

SKILL = "reflect-dont-hand-write"


def advice(root, relative):
    allowances = lint.load_allowances(lint.ALLOW_FILE)
    file = root / relative
    found = [f for f in lint.scan_text(relative, file.read_text()) if not lint.is_allowed(f, allowances)]
    if not found:
        return ""
    lines = [f"Hand-written copy of a definition in {relative}. Derive it with reflection instead (skill: {SKILL}):"]
    lines.extend(f.render() for f in found)
    lines.append("If the text is a wire or protocol name that is NOT an enumerator or member name, add a justified line to tools/reflection_lint_allow.txt.")
    return "\n".join(lines)


def main():
    try:
        payload = json.load(sys.stdin)
        tool_input = payload.get("tool_input") or payload.get("toolInput") or {}
        path = pathlib.Path(tool_input.get("file_path", ""))
        root = pathlib.Path(payload.get("cwd") or ".").resolve()
        if path.suffix not in lint.SOURCE_SUFFIXES or not path.is_file():
            return 0
        relative = path.resolve().relative_to(root).as_posix()
        if not relative.startswith(("src/", "app/")):
            return 0
        context = advice(root, relative)
        if context:
            print(json.dumps({"hookSpecificOutput": {"hookEventName": "PostToolUse", "additionalContext": context}}))
    except Exception:
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
