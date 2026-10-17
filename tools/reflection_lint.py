#!/usr/bin/env python3
import argparse
import dataclasses
import pathlib
import re
import sys

SOURCE_SUFFIXES = {".cpp", ".cppm"}
ALLOW_FILE = pathlib.Path(__file__).with_name("reflection_lint_allow.txt")
MIN_RUN = 3
MIN_CHAIN = 4
CHAIN_WINDOW = 12

ENUM_STRING_CASE = re.compile(r'^\s*case\s+[\w:]+::\w+\s*:\s*return\s+(?:"[^"]*"|std::string(?:_view)?\{[^}]*\})\s*;')
ENUM_STRING_TERNARY = re.compile(r'\w+\s*==\s*[\w:]+::\w+\s*\?\s*"[^"]*"\s*:')
NAME_ENUM_PAIR = re.compile(r'^\s*\{\s*"[^"]+"\s*,\s*[\w:]+::\w+\s*\}\s*,?\s*$')
WORD_COMPARISON = re.compile(r'\bif\s*\(\s*(?:[\w.\[\]]+)\s*==\s*"[^"]+"')
KEY_FROM_MEMBER = re.compile(r'\{\s*"(\w+)"\s*,.*\.\1\b')

RULES = {
    "stale-allowance": "remove the line from tools/reflection_lint_allow.txt, the code it excused is gone",
    "enum-name-switch": "an enum-to-string switch or ternary chain repeats the enumerator names; use reflect::enum_name<E>(value)",
    "name-enum-table": "a table of {\"word\", Type::member} pairs repeats the enumerator names; use reflect::enum_from_name<E>(word)",
    "word-chain": "a chain of if (word == \"...\") comparisons repeats member or flag names; annotate the members and walk them with std::meta (see json::read_aggregate)",
    "key-member-block": "a block of {\"key\", x.key} pairs repeats member names; derive the object from the aggregate with reflection",
}


@dataclasses.dataclass(frozen=True)
class Finding:
    path: str
    line: int
    rule: str
    text: str

    def render(self) -> str:
        return f"{self.path}:{self.line}: [{self.rule}] {RULES[self.rule]}\n    {self.text.strip()}"


def runs(lines, matches):
    start = None
    count = 0
    for index, line in enumerate(lines):
        if matches(line):
            if start is None:
                start = index
            count += 1
            continue
        if start is not None and count >= MIN_RUN:
            yield start, count
        start, count = None, 0
    if start is not None and count >= MIN_RUN:
        yield start, count


def chains(lines):
    hits = [index for index, line in enumerate(lines) if WORD_COMPARISON.search(line)]
    group = []
    for index in hits:
        if group and index - group[-1] > CHAIN_WINDOW:
            if len(group) >= MIN_CHAIN:
                yield group[0]
            group = []
        group.append(index)
    if len(group) >= MIN_CHAIN:
        yield group[0]


def scan_text(path, text):
    lines = text.splitlines()
    for start, _ in runs(lines, ENUM_STRING_CASE.match):
        yield Finding(path, start + 1, "enum-name-switch", lines[start])
    ternaries = [i for i, line in enumerate(lines) if len(ENUM_STRING_TERNARY.findall(line)) >= 1 and line.count("?") >= 2]
    for index in ternaries:
        yield Finding(path, index + 1, "enum-name-switch", lines[index])
    for start, _ in runs(lines, NAME_ENUM_PAIR.match):
        yield Finding(path, start + 1, "name-enum-table", lines[start])
    for start in chains(lines):
        yield Finding(path, start + 1, "word-chain", lines[start])
    for start, _ in runs(lines, KEY_FROM_MEMBER.search):
        yield Finding(path, start + 1, "key-member-block", lines[start])


def load_allowances(allow_file):
    allowed = []
    if not allow_file.exists():
        return allowed
    for raw in allow_file.read_text().splitlines():
        if not raw.strip() or raw.startswith("#"):
            continue
        parts = raw.split("|", 3)
        if len(parts) != 4 or not parts[3].strip():
            raise SystemExit(f"{allow_file}: every line needs path|rule|text|reason, got: {raw}")
        allowed.append(tuple(part.strip() for part in parts))
    return allowed


def is_allowed(finding, allowances):
    return any(path == finding.path and rule == finding.rule and fragment in finding.text for path, rule, fragment, _ in allowances)


def stale_allowances(allowances, raw_findings):
    return [
        Finding(path, 0, "stale-allowance", f"{rule}|{fragment}")
        for path, rule, fragment, _ in allowances
        if not any(f.path == path and f.rule == rule and fragment in f.text for f in raw_findings)
    ]


def scan_paths(root, paths, allow_file=ALLOW_FILE):
    allowances = load_allowances(allow_file)
    raw = []
    for base in paths:
        target = root / base
        files = [target] if target.is_file() else sorted(target.rglob("*"))
        for file in files:
            if file.suffix in SOURCE_SUFFIXES and file.is_file():
                relative = file.relative_to(root).as_posix()
                raw.extend(scan_text(relative, file.read_text()))
    return [f for f in raw if not is_allowed(f, allowances)] + stale_allowances(allowances, raw)


FIXTURES = {
    "enum-name-switch": 'constexpr auto f(E e) {\n  switch (e) {\n    case E::a: return "a";\n    case E::b: return "b";\n    case E::c: return "c";\n  }\n}\n',
    "name-enum-table": 'constexpr auto t = {\n  {"a", E::a},\n  {"b", E::b},\n  {"c", E::c},\n};\n',
    "word-chain": 'void p(std::string_view w) {\n if (w == "--a") {}\n else if (w == "--b") {}\n else if (w == "--c") {}\n else if (w == "--d") {}\n}\n',
    "key-member-block": 'auto j = make({\n {"input", n(s.input)},\n {"output", n(s.output)},\n {"cache", n(s.cache)},\n});\n',
}
CLEAN = 'void ok(E e) {\n  switch (e) {\n    case E::a: return 1;\n    case E::b: return 2;\n    case E::c: return 3;\n  }\n  if (w == "x") {}\n  if (w == "y") {}\n}\n{"a", 1},\n{"b", 2},\n'


def self_test():
    failures = 0
    for rule, text in FIXTURES.items():
        found = {f.rule for f in scan_text("fixture.cppm", text)}
        if found != {rule}:
            print(f"self-test: {rule} fixture found {sorted(found)}")
            failures += 1
    if list(scan_text("clean.cppm", CLEAN)):
        print("self-test: the clean fixture produced findings")
        failures += 1
    ternary = 'return s == level::a ? "a" : s == level::b ? "b" : "c";\n'
    if {f.rule for f in scan_text("t.cppm", ternary)} != {"enum-name-switch"}:
        print("self-test: the ternary chain was not found")
        failures += 1
    return failures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("paths", nargs="*", default=["src", "app"])
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--root", default=str(pathlib.Path(__file__).resolve().parent.parent))
    args = parser.parse_args()
    if args.self_test:
        failures = self_test()
        print("reflection_lint self-test:", "ok" if failures == 0 else f"{failures} failed")
        return 1 if failures else 0
    findings = scan_paths(pathlib.Path(args.root), args.paths)
    for finding in findings:
        print(finding.render())
    print(f"{len(findings)} hand-written mirrors of a definition to review")
    return 1 if args.strict and findings else 0


if __name__ == "__main__":
    sys.exit(main())
