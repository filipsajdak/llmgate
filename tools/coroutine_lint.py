import argparse
import pathlib
import re
import sys

import lizard

COROUTINE_KEYWORDS = re.compile(r"\bco_(await|return|yield)\b")
SOURCE_SUFFIXES = {".cpp", ".cppm"}
LONG_LIVED_TYPES = {
    "reactor",
    "listener",
    "tcp_stream",
    "tls_stream",
    "chaos_tcp",
    "Stream",
    "Reader",
    "Handler",
    "response_writer",
    "message_reader",
    "mutex",
    "client_context",
    "server_context",
    "echo_handler",
    "stream_handler",
    "failing_handler",
    "parser",
}
VIEW_TYPES = ("string_view", "span<")


def split_parameters(signature):
    start = signature.index("(")
    depth = 0
    pieces, current = [], []
    for character in signature[start + 1 : signature.rindex(")")]:
        depth += character in "<(["
        depth -= character in ">)]"
        if character == "," and depth == 0:
            pieces.append("".join(current))
            current = []
        else:
            current.append(character)
    pieces.append("".join(current))
    return [" ".join(piece.split()) for piece in pieces if piece.strip()]


def referent(parameter):
    words = re.sub(r"\b(const|std|::|llmgate|net|async|httpio|http|tls|chaos|detail)\b", " ", parameter)
    names = re.findall(r"[A-Za-z_][A-Za-z_0-9]*", words)
    return names[-2] if len(names) >= 2 else ""


def classify(parameter):
    by_reference = "&" in parameter
    is_view = any(view in parameter for view in VIEW_TYPES)
    if not by_reference and not is_view:
        return None
    named = {name for name in re.findall(r"[A-Za-z_][A-Za-z_0-9]*", parameter)}
    if by_reference and named & LONG_LIVED_TYPES:
        return None
    if is_view:
        return "view"
    return "reference"


def coroutines_in(path):
    text = path.read_text()
    lines = text.splitlines()
    analysed = lizard.analyze_file.analyze_source_code(str(path.with_suffix(".cpp")), text)
    for function in analysed.function_list:
        body = "\n".join(lines[function.start_line - 1 : function.end_line])
        if COROUTINE_KEYWORDS.search(body):
            yield function


ALLOWLIST = pathlib.Path(__file__).with_name("coroutine_lint_allow.txt")


def allowed_entries():
    entries = []
    for line in ALLOWLIST.read_text().splitlines():
        if line.strip():
            path, function, parameter, _reason = line.split("|", 3)
            entries.append((path, function, parameter))
    return entries


def is_allowed(entries, path, function, parameter):
    for allowed_path, allowed_function, allowed_parameter in entries:
        path_matches = path.startswith(allowed_path) if allowed_path.endswith("/") else path == allowed_path
        function_matches = allowed_function in ("*", function)
        parameter_matches = allowed_parameter == "*" or allowed_parameter in parameter
        if path_matches and function_matches and parameter_matches:
            return True
    return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("roots", nargs="+")
    parser.add_argument("--strict", action="store_true")
    arguments = parser.parse_args()
    entries = allowed_entries()
    findings = []
    for root in arguments.roots:
        for path in sorted(pathlib.Path(root).rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES:
                continue
            for function in coroutines_in(path):
                for parameter in split_parameters(function.long_name):
                    kind = classify(parameter)
                    name = function.name.split("::")[-1]
                    if kind and not is_allowed(entries, path.as_posix(), name, parameter):
                        findings.append((path.as_posix(), function.start_line, function.name.split("::")[-1], kind, parameter))
    for path, line, name, kind, parameter in findings:
        print(f"{path}:{line}  {name}  [{kind}]  {parameter}")
    print(f"{len(findings)} coroutine parameters to review")
    return 1 if findings and arguments.strict else 0


sys.exit(main())
