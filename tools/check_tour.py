#!/usr/bin/env python3
import argparse
import pathlib
import re
import sys

LINK = re.compile(r"\[([^\]]*)\]\(([^)\s]+)\)")
SYMBOL_AFTER_LINK = re.compile(r"\[[^\]]*\]\(([^)\s]+)\)\s+`([^`]+)`")
SECTION_START = "## What the code uses today"
CODE_PREFIX = "- Code: "
TEST_PREFIX = "- Test: "


def repo_target(document, target):
    if re.match(r"^[a-z]+:", target) or target.startswith("#"):
        return None
    return (document.parent / target.split("#", 1)[0]).resolve()


def link_problems(document):
    problems = []
    lines = document.read_text(encoding="utf-8").splitlines()
    for number, line in enumerate(lines, 1):
        for _, target in LINK.findall(line):
            path = repo_target(document, target)
            if path is not None and not path.exists():
                problems.append(f"{document.name}:{number}: {target} does not exist")
        for target, symbol in SYMBOL_AFTER_LINK.findall(line):
            path = repo_target(document, target)
            if path is not None and path.is_file() and symbol not in path.read_text(encoding="utf-8", errors="replace"):
                problems.append(f"{document.name}:{number}: `{symbol}` does not occur in {target}")
    return problems


def feature_sections(lines):
    inside = False
    current = None
    sections = []
    for number, line in enumerate(lines, 1):
        if line.startswith("## "):
            inside = line.strip() == SECTION_START
            current = None
        elif inside and line.startswith("### "):
            current = {"title": line[4:].strip(), "line": number, "code": 0, "test": 0}
            sections.append(current)
        elif current is not None and LINK.search(line):
            current["code"] += line.startswith(CODE_PREFIX)
            current["test"] += line.startswith(TEST_PREFIX)
    return sections


def section_problems(document):
    sections = feature_sections(document.read_text(encoding="utf-8").splitlines())
    problems = [] if sections else [f"{document.name}: no feature sections under '{SECTION_START}'"]
    for section in sections:
        if not section["code"]:
            problems.append(f"{document.name}:{section['line']}: '{section['title']}' has no '{CODE_PREFIX}' link")
        if not section["test"]:
            problems.append(f"{document.name}:{section['line']}: '{section['title']}' has no '{TEST_PREFIX}' link")
    return problems


def check(document):
    return link_problems(document) + section_problems(document)


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=str(pathlib.Path(__file__).resolve().parents[1]))
    parser.add_argument("--document", default="docs/cpp26-tour.md")
    arguments = parser.parse_args(argv)
    document = pathlib.Path(arguments.root) / arguments.document
    if not document.is_file():
        print(f"{document} is missing", file=sys.stderr)
        return 1
    problems = check(document)
    for problem in problems:
        print(problem, file=sys.stderr)
    print(f"{len(problems)} problems in {arguments.document}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
