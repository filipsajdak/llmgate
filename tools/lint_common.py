import dataclasses

SOURCE_SUFFIXES = {".cpp", ".cppm"}
PENDING = "pending:"
EXCEPTION = "exception"
ADVICE = {}


@dataclasses.dataclass(frozen=True)
class Finding:
    path: str
    line: int
    rule: str
    text: str

    def render(self):
        return f"{self.path}:{self.line}: [{self.rule}] {ADVICE[self.rule]}\n    {self.text.strip()}"


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


def matches(allowance, finding):
    path, rule, fragment, _ = allowance
    in_scope = finding.path == path or (path.endswith("/") and finding.path.startswith(path))
    return in_scope and rule == finding.rule and fragment in finding.text


def is_allowed(finding, allowances):
    return any(matches(allowance, finding) for allowance in allowances)


def stale_allowances(allowances, raw_findings, rules):
    return [
        Finding(path, 0, "stale-allowance", f"{rule}|{fragment}")
        for path, rule, fragment, reason in allowances
        if rule in rules and not any(matches((path, rule, fragment, reason), f) for f in raw_findings)
    ]


def source_files(root, paths, excluded=()):
    for base in paths:
        target = root / base
        files = [target] if target.is_file() else sorted(target.rglob("*"))
        for file in files:
            relative = file.relative_to(root).as_posix()
            if file.suffix in SOURCE_SUFFIXES and file.is_file() and not relative.startswith(tuple(excluded)):
                yield relative, file


def debt_counts(allowances):
    counts = {}
    for _, _, _, reason in allowances:
        kind = PENDING if reason.startswith(PENDING) else EXCEPTION
        counts[kind] = counts.get(kind, 0) + 1
    return counts
