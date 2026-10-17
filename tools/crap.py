import argparse
import json
import pathlib
import sys

import lizard

MAX_CRAP = 8.0
SOURCE_SUFFIXES = {".cpp", ".cppm"}


def functions_of(path):
    source = path.read_text()
    analysed = lizard.analyze_file.analyze_source_code(str(path.with_suffix(".cpp")), source)
    return analysed.function_list


def coverage_by_line(report):
    table = {}
    for entry in report["files"]:
        hits = table.setdefault(entry["file"], {})
        for line in entry["lines"]:
            hits[line["line_number"]] = hits.get(line["line_number"], False) or line["count"] > 0
    return table


def crap(complexity, covered_fraction):
    return complexity**2 * (1 - covered_fraction) ** 3 + complexity


def score_file(path, lines):
    for function in functions_of(path):
        inside = [hit for number, hit in lines.items() if function.start_line <= number <= function.end_line]
        fraction = sum(inside) / len(inside) if inside else 1.0
        yield function.long_name, function.start_line, function.cyclomatic_complexity, fraction, crap(
            function.cyclomatic_complexity, fraction
        )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("coverage_json")
    parser.add_argument("roots", nargs="+")
    parser.add_argument("--max", type=float, default=MAX_CRAP)
    arguments = parser.parse_args()

    lines_by_file = coverage_by_line(json.load(open(arguments.coverage_json)))
    offenders = []
    total = 0
    for root in arguments.roots:
        for path in sorted(pathlib.Path(root).rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES:
                continue
            lines = lines_by_file.get(path.as_posix(), {})
            for name, start, complexity, fraction, score in score_file(path, lines):
                total += 1
                if score > arguments.max:
                    offenders.append((score, path.as_posix(), start, name, complexity, fraction))
    for score, path, start, name, complexity, fraction in sorted(offenders, reverse=True):
        print(f"CRAP {score:6.1f}  CC {complexity:2d}  cov {fraction:4.0%}  {path}:{start}  {name[:70]}")
    print(f"{total} functions, {len(offenders)} above CRAP {arguments.max:g}")
    return 1 if offenders else 0


sys.exit(main())
