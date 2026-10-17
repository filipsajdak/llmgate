#!/usr/bin/env python3
import argparse
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import cpp_source
import lint_common
from lint_common import Finding, is_allowed, load_allowances

ALLOW_FILE = pathlib.Path(__file__).with_name("cpp26_lint_allow.txt")
DEFAULT_PATHS = ["src", "app", "tests"]
EXCLUDED = ("tests/constexpr/capabilities/",)
MIN_APPEND_RUN = 3
EXCEPTION_MARKER = re.compile(r"^//\s*Exception:\s*\S")
NAMESPACE_CLOSER = re.compile(r"^\}\s*$")
NAMESPACE_COMMENT = re.compile(r"^//\s*namespace\b[\w:\s]*$")

STD_HEADERS = frozenset(
    "algorithm any array atomic barrier bit bitset charconv chrono codecvt compare complex concepts condition_variable coroutine debugging deque exception execution expected "
    "filesystem flat_map flat_set format forward_list fstream functional future generator hazard_pointer hive initializer_list inplace_vector iomanip ios iosfwd istream iterator "
    "latch limits linalg list locale map mdspan memory memory_resource mutex new numbers numeric optional ostream print queue random ranges ratio rcu regex scoped_allocator "
    "semaphore set shared_mutex source_location span spanstream sstream stack stacktrace stdexcept stdfloat stop_token streambuf string string_view strstream syncstream "
    "system_error text_encoding thread tuple type_traits typeindex typeinfo unordered_map unordered_set utility valarray variant vector version".split()
)
BESIDE_IMPORT_STD = frozenset({"meta", "contracts"})
INCLUDE = re.compile(r"^[ \t]*#[ \t]*include[ \t]*<([^>]+)>", re.MULTILINE)
LEGACY_IO = re.compile(r"#[ \t]*include[ \t]*<iostream>|\bstd::(?:cout|cerr|clog|cin)\b|(?<![\w.])(?:std::|::)?v?[fs]?n?printf\s*\(")
APPEND_STATEMENT = re.compile(r"(\w+)\s*(?:\+=|\.push_back\s*\()[^;]*;")
CONTROL_HEADER = re.compile(r"^(?:else\b\s*)?(?:if|for|while|switch|catch|do|try|else)\b")
FUNCTION_TAIL = re.compile(r"\)\s*(?:const\b|noexcept\b(?:\s*\([^)]*\))?|override\b|final\b|mutable\b|&&?|->[^{;]*|requires\b[^{;]*|\s)*$")
CONSTEXPR_WORD = re.compile(r"\b(?:constexpr|consteval)\b")
TEMPLATE_START = re.compile(r"\btemplate\s*<")
BARE_PARAMETER = re.compile(r"^(?:class|typename)\s*(?:\.\.\.)?\s*(\w+)\s*(?:=.*)?$", re.DOTALL)
NON_TEMPLATE_DECLARATION = re.compile(r"^(?:struct|class|union|using|concept|enum|friend)\b")
REQUIRES = re.compile(r"\brequires\b")
TYPE_QUALIFIERS = re.compile(r"\b(?:const|volatile)\b|[&*]")
ROLE_NAMES = frozenset({"sink", "handler", "callback", "visitor", "stream", "reader", "writer", "predicate"})
SINK_VERBS = (
    "read write send receive recv post emit submit append put close flush accept connect resume wait schedule spawn invoke on_event handle push_back "
    "next advance begin_stream write_chunk"
).split()

RULES = {
    "stale-allowance": "remove the line from tools/cpp26_lint_allow.txt, the code it excused is gone",
    "unconstrained-template": "a template parameter that is invoked, awaited or used as a stream or sink is unconstrained; name the requirement with a concept or a requires clause",
    "std-include": "#include of a standard header in a module-based project; use import std; (C and POSIX headers stay in the global module fragment)",
    "legacy-io": "iostream, printf and std::cout are legacy I/O; use std::print or std::println for output and std::format for text",
    "append-chain": "a run of += or push_back on one variable builds text or a container step by step; state it once: std::format or format_to at run time, text::append_all in constexpr code, a braced list for a container",
    "comment": "comments are for rules that are broken: only a line starting '// Exception: <reason>' or a '}  // namespace x' closer is allowed; put contract text in the doc comment's home (name, type, test, requirement, commit)",
}
lint_common.ADVICE.update(RULES)
OWN_RULES = frozenset(RULES) - {"stale-allowance"}


class Block:
    def __init__(self, begin, end, header):
        self.begin = begin
        self.end = end
        self.header = header

    def is_function(self):
        return bool(FUNCTION_TAIL.search(self.header)) and not CONTROL_HEADER.match(self.header.strip())


def blocks(skeleton):
    stack = []
    found = []
    boundary = 0
    for offset, char in enumerate(skeleton):
        if char == "{":
            stack.append((offset, skeleton[boundary:offset]))
            boundary = offset + 1
        elif char == "}":
            if stack:
                begin, header = stack.pop()
                found.append(Block(begin, offset, header.strip()))
            boundary = offset + 1
        elif char == ";":
            boundary = offset + 1
    return found


def outermost_function(all_blocks, offset):
    inside = [b for b in all_blocks if b.begin < offset < b.end and b.is_function()]
    return min(inside, key=lambda b: b.begin) if inside else None


def comment_findings(path, source):
    for comment in source.comments:
        if not comment.block and comment.whole_line and EXCEPTION_MARKER.match(comment.text):
            continue
        if not comment.block and NAMESPACE_CLOSER.match(comment.before.strip()) and NAMESPACE_COMMENT.match(comment.text):
            continue
        yield Finding(path, comment.line, "comment", source.line_text(comment.line))


def include_findings(path, source):
    for match in INCLUDE.finditer(source.skeleton):
        header = match.group(1)
        if header in STD_HEADERS and header not in BESIDE_IMPORT_STD:
            yield Finding(path, source.line(match.start()), "std-include", source.text_at(match.start()))


def legacy_io_findings(path, source):
    for match in LEGACY_IO.finditer(source.skeleton):
        yield Finding(path, source.line(match.start()), "legacy-io", source.text_at(match.start()))


def append_runs(source):
    group = []
    for match in APPEND_STATEMENT.finditer(source.skeleton):
        before = source.skeleton[: match.start()].rstrip()[-1:]
        if before and before not in ";{}":
            continue
        if group and (group[-1].group(1) != match.group(1) or source.skeleton[group[-1].end() : match.start()].strip()):
            if len(group) >= MIN_APPEND_RUN:
                yield group[0]
            group = []
        group.append(match)
    if len(group) >= MIN_APPEND_RUN:
        yield group[0]


def append_findings(path, source, all_blocks):
    for match in append_runs(source):
        function = outermost_function(all_blocks, match.start())
        if function is None or CONSTEXPR_WORD.search(function.header):
            continue
        yield Finding(path, source.line(match.start()), "append-chain", source.text_at(match.start()))


def matching_close(text, begin, opener, closer):
    depth = 0
    for offset in range(begin, len(text)):
        depth += text[offset] == opener
        depth -= text[offset] == closer
        if depth == 0:
            return offset
    return -1


def split_top_level(text):
    pieces, current, depth = [], [], 0
    for char in text:
        depth += char in "<([{"
        depth -= char in ">)]}"
        if char == "," and depth == 0:
            pieces.append("".join(current))
            current = []
        else:
            current.append(char)
    pieces.append("".join(current))
    return [piece.strip() for piece in pieces if piece.strip()]


def bare_parameters(skeleton, open_angle):
    close = matching_close(skeleton, open_angle, "<", ">")
    if close < 0:
        return close, []
    names = []
    for piece in split_top_level(skeleton[open_angle + 1 : close]):
        found = BARE_PARAMETER.match(piece)
        if found:
            names.append(found.group(1))
    return close, names


def declaration_end(skeleton, begin):
    depth = 0
    for offset in range(begin, len(skeleton)):
        char = skeleton[offset]
        depth += char in "(["
        depth -= char in ")]"
        if depth == 0 and char in "{;":
            return offset
    return len(skeleton)


def variables_of_type(declaration, names):
    open_paren = declaration.find("(")
    close_paren = matching_close(declaration, open_paren, "(", ")") if open_paren >= 0 else -1
    variables = {}
    if close_paren < 0:
        return variables
    for parameter in split_top_level(declaration[open_paren + 1 : close_paren]):
        found = re.match(r"^(.*?)(\w+)\s*(?:=.*)?$", parameter, re.DOTALL)
        if found:
            kind = " ".join(TYPE_QUALIFIERS.sub(" ", found.group(1)).split())
            if kind in names:
                variables[found.group(2)] = kind
    return variables


def is_used_as_callable(body, variable):
    name = re.escape(variable)
    if variable in ROLE_NAMES and re.search(rf"\b{name}\b", body):
        return True
    verbs = "|".join(SINK_VERBS)
    uses = (
        rf"(?<![\w.>:]){name}\s*\(",
        rf"\bco_await\s+(?:std::move\(\s*)?{name}\b",
        rf"\b{name}\s*(?:\.|->)\s*(?:{verbs})\s*\(",
        rf"std::invoke\(\s*(?:std::(?:move|forward<[^>]*>)\(\s*)?{name}\b",
        rf"std::(?:move|forward<[^>]*>)\(\s*{name}\s*\)\s*\(",
    )
    return any(re.search(use, body) for use in uses)


def template_findings(path, source, all_blocks):
    skeleton = source.skeleton
    ends = {b.begin: b.end for b in all_blocks}
    for start in TEMPLATE_START.finditer(skeleton):
        close, names = bare_parameters(skeleton, start.end() - 1)
        if not names:
            continue
        stop = declaration_end(skeleton, close + 1)
        declaration = skeleton[close + 1 : stop]
        if REQUIRES.search(declaration) or NON_TEMPLATE_DECLARATION.match(declaration.strip()):
            continue
        body = skeleton[stop : ends[stop] + 1] if stop in ends else ""
        variables = variables_of_type(declaration, names)
        if any(is_used_as_callable(body, variable) for variable in variables):
            head = " ".join(skeleton[start.start() : declaration.find("(") + close + 1 if "(" in declaration else stop].split())
            yield Finding(path, source.line(start.start()), "unconstrained-template", head)


def scan_text(path, text):
    source = cpp_source.scan(text)
    all_blocks = blocks(source.skeleton)
    yield from comment_findings(path, source)
    yield from include_findings(path, source)
    yield from legacy_io_findings(path, source)
    yield from append_findings(path, source, all_blocks)
    yield from template_findings(path, source, all_blocks)


def scan_paths(root, paths, allow_file=ALLOW_FILE):
    allowances = load_allowances(allow_file)
    raw = []
    for relative, file in lint_common.source_files(root, paths, EXCLUDED):
        raw.extend(scan_text(relative, file.read_text()))
    return [f for f in raw if not is_allowed(f, allowances)] + lint_common.stale_allowances(allowances, raw, OWN_RULES)


FIXTURES = {
    "unconstrained-template": "template <class Sink>\nvoid drain(Sink& out, int n) {\n  out.write(n);\n}\n",
    "std-include": "module;\n#include <vector>\nexport module m;\n",
    "legacy-io": 'void f() {\n  std::cout << 1;\n}\n',
    "append-chain": "std::string render(int a) {\n  std::string out;\n  out += \"a\";\n  out += \"b\";\n  out += \"c\";\n  return out;\n}\n",
    "comment": "int a;  // why\n",
}
CLEAN = "\n".join(
    [
        "module;",
        "#include <unistd.h>",
        "#include <openssl/ssl.h>",
        "#include <cstdio>",
        "#include <meta>",
        "export module m;",
        "import std;",
        "template <class T>",
        "constexpr T twice(T value) { return value + value; }",
        "template <class F>",
        "  requires std::invocable<F>",
        "auto run(F fn) { return fn(); }",
        "template <std::invocable F>",
        "auto run_twice(F fn) { return fn(); }",
        "template <class T>",
        "struct holder { T value; };",
        "template <class F>",
        "auto later(F fn)",
        "  requires std::invocable<F> {",
        "  return fn();",
        "}",
        "// Exception: a marked rule exception",
        "}  // namespace m",
        "constexpr auto text(int a) {",
        "  std::string out;",
        '  out += "a";',
        '  out += "b";',
        '  out += "c";',
        "  return out;",
        "}",
        'auto json = R"({"a": 1, // not a comment\n "b": /* nor this */ 2})";',
        'auto url = "http://example.com//x"; auto quote = \'"\'; auto digits = 1\'000\'000;',
        "std::string two(int a) {",
        "  std::string out;",
        '  out += "a";',
        '  out += "b";',
        "  return out;",
        "}",
        "",
    ]
)
CLEAN_NAMESPACE = "namespace m {\nint x;\n}  // namespace m\n"
BLOCK_COMMENT = "/* block */\nint a;\n"
CONSTEXPR_CHAIN = "constexpr auto f() {\n  std::string out;\n  out += 'a';\n  out += 'b';\n  out += 'c';\n  return out;\n}\n"
CALLED_TEMPLATE = "template <typename F>\nauto call(F&& fn, int n) {\n  return fn(n);\n}\n"
AWAITED_TEMPLATE = "template <class Awaitable>\nasync::task<int> wait_for(Awaitable a) {\n  co_return co_await a;\n}\n"
FORWARDED_SINK = "template <class Sink>\nauto feed(Sink&& sink) {\n  return reader.feed(data, sink);\n}\n"
UTILITY_TEMPLATE = "template <class T>\nconstexpr auto name_of(T const& value) {\n  return value.size() + sizeof(T);\n}\n"
PUSH_CHAIN = "void fill(std::vector<int>& v) {\n  v.push_back(1);\n  v.push_back(2);\n  v.push_back(3);\n}\n"
PRINTF = 'void f() { std::printf("%d", 1); }\n'


def expect(label, findings, rules):
    got = {f.rule for f in findings}
    if got != rules:
        print(f"self-test: {label} found {sorted(got)}, expected {sorted(rules)}")
        return 1
    return 0


def self_test():
    failures = 0
    for rule, text in FIXTURES.items():
        failures += expect(rule, scan_text("fixture.cppm", text), {rule})
    failures += expect("clean", scan_text("clean.cppm", CLEAN), set())
    failures += expect("namespace closer", scan_text("c.cppm", CLEAN_NAMESPACE), set())
    failures += expect("block comment", scan_text("c.cppm", BLOCK_COMMENT), {"comment"})
    failures += expect("constexpr chain", scan_text("c.cppm", CONSTEXPR_CHAIN), set())
    failures += expect("called template", scan_text("c.cppm", CALLED_TEMPLATE), {"unconstrained-template"})
    failures += expect("awaited template", scan_text("c.cppm", AWAITED_TEMPLATE), {"unconstrained-template"})
    failures += expect("forwarded sink", scan_text("c.cppm", FORWARDED_SINK), {"unconstrained-template"})
    failures += expect("type utility template", scan_text("c.cppm", UTILITY_TEMPLATE), set())
    failures += expect("push_back chain", scan_text("c.cppm", PUSH_CHAIN), {"append-chain"})
    failures += expect("printf", scan_text("c.cppm", PRINTF), {"legacy-io"})
    return failures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("paths", nargs="*", default=DEFAULT_PATHS)
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--root", default=str(pathlib.Path(__file__).resolve().parent.parent))
    args = parser.parse_args()
    if args.self_test:
        failures = self_test()
        print("cpp26_lint self-test:", "ok" if failures == 0 else f"{failures} failed")
        return 1 if failures else 0
    findings = scan_paths(pathlib.Path(args.root), args.paths)
    for finding in findings:
        print(finding.render())
    print(f"{len(findings)} places that miss a C++26 idiom or break the comment rule")
    return 1 if args.strict and findings else 0


if __name__ == "__main__":
    sys.exit(main())
