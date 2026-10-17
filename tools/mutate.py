import argparse
import atexit
import hashlib
import pathlib
import random
import re
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD_DIR = pathlib.Path("build/mutate")
TRIAGE_FILE = pathlib.Path("tools/mutation_triage.txt")
IGNORE_FILE = pathlib.Path("tools/mutation_ignore.txt")
BUILD_TIMEOUT_SECONDS = 600
TEST_TIMEOUT_SECONDS = 30
DEFAULT_SAMPLE = 200
DEFAULT_SEED = 1
HASH_LENGTH = 8
TRIAGE_KINDS = ("equivalent", "arid")
STATIC_ASSERT_MARK = "static assertion failed"
HUNK = re.compile(r"^@@ -\S+ \+(\d+)(?:,(\d+))? @@")

KILLED_COMPILE_TIME = "killed_compile_time"
KILLED = "killed"
TIMEOUT = "timeout"
UNCOMPILABLE = "uncompilable"
SURVIVED = "survived"


def run(command, timeout):
    try:
        done = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as expired:
        return None, (expired.stdout or b"").__str__()
    return done.returncode, done.stdout + done.stderr


def resolve_source(target):
    path = pathlib.Path(target)
    if path.is_file():
        return path
    found = sorted(pathlib.Path("src").glob(f"*/{target}.cppm")) + sorted(pathlib.Path("src").glob(f"*/{target}.cpp"))
    if len(found) != 1:
        sys.exit(f"mutate: cannot resolve '{target}' to exactly one source file (found {len(found)})")
    return found[0]


def tests_for(source):
    cmake = pathlib.Path("CMakeLists.txt").read_text()
    module = re.search(rf"llmgate_module\((\w+)\s+INTERFACE\s+{re.escape(source.as_posix())}\b", cmake)
    if not module:
        sys.exit(f"mutate: {source} is not an INTERFACE file of a llmgate_module; pass --tests")
    pattern = rf"^llmgate_test\((\w+)\s+unit\s+([^)]*)\)"
    return [name for name, libs in re.findall(pattern, cmake, re.M) if module.group(1) in libs.split()]


def changed_lines(source, base):
    diff = subprocess.run(["git", "diff", "-U0", base, "--", source.as_posix()], capture_output=True, text=True, check=True).stdout
    lines = set()
    for row in diff.splitlines():
        hunk = HUNK.match(row)
        if hunk:
            start, count = int(hunk.group(1)), int(hunk.group(2) or 1)
            lines.update(range(start, start + count))
    return sorted(lines)


def generate(source, lines, scratch):
    directory = scratch / "mutants"
    directory.mkdir()
    command = ["mutate", source.as_posix(), "cpp", "--noCheck", "--mutantDir", str(directory)]
    if IGNORE_FILE.exists():
        command += ["--ignore", str(IGNORE_FILE)]
    if lines is not None:
        selection = scratch / "lines.txt"
        selection.write_text("".join(f"{number}\n" for number in lines))
        command += ["--lines", str(selection)]
    subprocess.run(command, capture_output=True, text=True, check=True)
    return sorted(directory.iterdir(), key=lambda path: int(path.name.split(".")[-2]))


def first_difference(original, mutant):
    for number, (before, after) in enumerate(zip(original, mutant), start=1):
        if before != after:
            return number, before.strip(), after.strip()
    return None


def load_triage():
    table = {}
    if TRIAGE_FILE.exists():
        for row in TRIAGE_FILE.read_text().splitlines():
            if row.strip() and not row.startswith("#"):
                key, kind, reason = (part.strip() for part in row.split("|", 2))
                table[key] = kind
    return table


def triage_key(source, line, mutated):
    return f"{source.as_posix()}:{line}:{hashlib.sha1(mutated.encode()).hexdigest()[:HASH_LENGTH]}"


class Restorer:
    def __init__(self, source):
        self.source = source
        self.original = source.read_bytes()

    def restore(self, *_):
        if self.source.read_bytes() != self.original:
            self.source.write_bytes(self.original)

    def install_on_exit(self):
        atexit.register(self.restore)
        for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            signal.signal(number, lambda *_: sys.exit(1))


def build_and_test(tests):
    code, output = run(["cmake", "--build", str(BUILD_DIR), "--target", *tests], BUILD_TIMEOUT_SECONDS)
    if code is None:
        return TIMEOUT
    if code != 0:
        return KILLED_COMPILE_TIME if STATIC_ASSERT_MARK in output else UNCOMPILABLE
    for test in tests:
        code, _ = run([str(BUILD_DIR / test)], TEST_TIMEOUT_SECONDS)
        if code is None:
            return TIMEOUT
        if code != 0:
            return KILLED
    return SURVIVED


def configure():
    subprocess.run(["cmake", "--preset", "host", "-B", str(BUILD_DIR)], check=True, capture_output=True)


def main():
    parser = argparse.ArgumentParser(description="Mutation testing of one source file with universalmutator")
    parser.add_argument("target")
    parser.add_argument("--changed-lines", metavar="BASE_REF")
    parser.add_argument("--tests", help="comma separated unit test targets; default: derived from CMakeLists.txt")
    parser.add_argument("--sample", type=int, default=DEFAULT_SAMPLE, help="mutants to run; 0 runs all of them")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument("--new-tests", type=int, default=0, help="tests added to kill survivors, for the summary line")
    parser.add_argument("--list", action="store_true", help="print survivors that are not triaged yet")
    arguments = parser.parse_args()

    source = resolve_source(arguments.target)
    tests = arguments.tests.split(",") if arguments.tests else tests_for(source)
    if not tests:
        sys.exit(f"mutate: no unit test links the module of {source}; pass --tests")
    lines = changed_lines(source, arguments.changed_lines) if arguments.changed_lines else None
    scope = "changed lines" if lines is not None else "all lines"

    scratch = pathlib.Path(tempfile.mkdtemp(prefix="mutate-"))
    restorer = Restorer(source)
    restorer.install_on_exit()
    atexit.register(shutil.rmtree, scratch, True)

    mutants = generate(source, lines, scratch) if lines != [] else []
    total = len(mutants)
    if arguments.sample and total > arguments.sample:
        mutants = random.Random(arguments.seed).sample(mutants, arguments.sample)
        mutants.sort(key=lambda path: int(path.name.split(".")[-2]))
    print(f"{source}: {total} mutants, running {len(mutants)}, tests {','.join(tests)}", flush=True)

    configure()
    baseline = build_and_test(tests)
    if baseline != SURVIVED:
        sys.exit(f"mutate: the unmutated tree does not build and pass ({baseline})")

    original_lines = source.read_text().splitlines()
    triage = load_triage()
    counts = {KILLED: 0, KILLED_COMPILE_TIME: 0, TIMEOUT: 0, UNCOMPILABLE: 0, SURVIVED: 0}
    survivors = []
    for index, mutant in enumerate(mutants, start=1):
        text = mutant.read_text()
        difference = first_difference(original_lines, text.splitlines())
        source.write_bytes(mutant.read_bytes())
        try:
            verdict = build_and_test(tests)
        finally:
            restorer.restore()
        counts[verdict] += 1
        if verdict == SURVIVED and difference:
            survivors.append((triage_key(source, difference[0], difference[2]), difference))
        print(f"[{index}/{len(mutants)}] {mutant.name} line {difference[0] if difference else '?'}: {verdict}", flush=True)

    equivalent = sum(1 for key, _ in survivors if triage.get(key) == "equivalent")
    arid = sum(1 for key, _ in survivors if triage.get(key) == "arid")
    for key, (line, before, after) in survivors:
        state = triage.get(key, "UNTRIAGED")
        print(f"SURVIVOR {state:11s} {key}\n    - {before}\n    + {after}")
    compiled = len(mutants) - counts[UNCOMPILABLE]
    print(f"{len(mutants)} run: {counts[KILLED]} killed by tests, {counts[KILLED_COMPILE_TIME]} killed by static_assert, "
          f"{counts[TIMEOUT]} timeout, {counts[UNCOMPILABLE]} uncompilable, {counts[SURVIVED]} survived; {compiled} compiled")
    killed = counts[KILLED] + counts[KILLED_COMPILE_TIME] + counts[TIMEOUT]
    print(f"Mutation: universalmutator, {scope}: {killed} killed, {counts[SURVIVED]} survived "
          f"({arguments.new_tests} new tests, {equivalent} equivalent, {arid} arid)")
    return 0


sys.exit(main())
