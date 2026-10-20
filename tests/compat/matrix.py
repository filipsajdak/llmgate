#!/usr/bin/env python3
"""Render the compatibility matrix into README.md between markers, or check that it is current."""
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
README = HERE.parent.parent / "README.md"
BEGIN, END = "<!-- compat-matrix:begin -->", "<!-- compat-matrix:end -->"
ORDER = ["copilot", "anthropic", "switching", "fallback", "scripted"]
NOT_RUN = "not run"


def cell(spec, backend, expected):
    if spec["name"] not in backend.get("only", [spec["name"]]):
        return "-"
    if set(spec.get("needs", [])) - set(backend["provides"]):
        return "unsupported"
    return expected.get(backend["name"], {}).get(spec["name"], NOT_RUN).lower().replace("_", " ")


def render():
    backends = []
    for name in ORDER:
        backends.append(dict(json.loads((HERE / "backends" / f"{name}.json").read_text()), name=name))
    expected = {}
    for backend in backends:
        path = HERE / "expected" / f"{backend['name']}.json"
        expected[backend["name"]] = json.loads(path.read_text()) if path.exists() else {}
    scenarios = []
    for path in sorted((HERE / "scenarios").iterdir()):
        if (path / "scenario.json").exists():
            scenarios.append(dict(json.loads((path / "scenario.json").read_text()), name=path.name))
    lines = ["| Scenario | " + " | ".join(b["title"] for b in backends) + " |", "|---|" + "---|" * len(backends)]
    for spec in scenarios:
        lines.append(f"| {spec['title']} (`{spec['name']}`) | " + " | ".join(cell(spec, b, expected) for b in backends) + " |")
    return "\n".join(lines)


def main():
    text = README.read_text()
    begin, end = text.index(BEGIN) + len(BEGIN), text.index(END)
    wanted = text[:begin] + "\n" + render() + "\n" + text[end:]
    if "--check" in sys.argv:
        if wanted != text:
            sys.exit("README.md compatibility matrix is stale: run python3 tests/compat/matrix.py")
        return
    README.write_text(wanted)


main()
