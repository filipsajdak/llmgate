#!/usr/bin/env python3
import argparse
import json
import pathlib
import sys

TOLERANCE_FACTOR = 3.0
LATENCY_FLOOR_MS = 0.5
CPU_FLOOR_MS = 0.1
MIN_REUSE_RATIO = 0.9
GATEWAY_LABEL = "gateway ledger on"
COMPARED_SCENARIO = "burst"
QUIET_CONCURRENCY = 1
NOISY_CONCURRENCY = 32


def key_of(row):
    return f"{row['scenario']}/c{row['concurrency']}/{row['body_kb']}KB"


def cells(results):
    direct = {key_of(r): r for r in results if r["label"] == "direct"}
    out = {}
    for row in results:
        if row["label"] != GATEWAY_LABEL or row["scenario"] != COMPARED_SCENARIO or row["concurrency"] >= NOISY_CONCURRENCY:
            continue
        base = direct.get(key_of(row))
        if base is None:
            continue
        opened, reused = row.get("upstream_opened", 0), row.get("upstream_reused", 0)
        out[key_of(row)] = {
            "added_p50_ms": round(row["total_ms"]["p50"] - base["total_ms"]["p50"], 3),
            "added_p99_ms": round(row["total_ms"]["p99"] - base["total_ms"]["p99"], 3),
            "cpu_ms_per_request": row.get("cpu_ms_per_request", 0.0),
            "reuse_ratio": round(reused / (reused + opened), 4) if reused + opened else 0.0,
            "errors": row["errors"],
            "concurrency": row["concurrency"],
        }
    return out


def judge(current, baseline):
    hard, advisory = [], []
    for key, now in current.items():
        if now["errors"]:
            hard.append(f"{key}: {now['errors']} failed requests")
        if now["concurrency"] == QUIET_CONCURRENCY and now["reuse_ratio"] < MIN_REUSE_RATIO:
            hard.append(f"{key}: only {now['reuse_ratio']:.0%} of upstream connections were reused (need {MIN_REUSE_RATIO:.0%})")
        was = baseline.get(key)
        if not was:
            continue
        for metric, floor in (("added_p50_ms", LATENCY_FLOOR_MS), ("added_p99_ms", LATENCY_FLOOR_MS), ("cpu_ms_per_request", CPU_FLOOR_MS)):
            limit = was[metric] * TOLERANCE_FACTOR + floor
            if now[metric] > limit:
                advisory.append(f"{key}: {metric} {now[metric]} is above {limit:.3f} (baseline {was[metric]})")
    return hard, advisory


def summary(current, baseline):
    lines = ["| cell | added p50 ms | added p99 ms | cpu ms/req | reuse | baseline p50 / p99 / cpu |", "|---|---|---|---|---|---|"]
    for key, now in sorted(current.items()):
        was = baseline.get(key, {})
        lines.append(f"| {key} | {now['added_p50_ms']} | {now['added_p99_ms']} | {now['cpu_ms_per_request']} | {now['reuse_ratio']:.0%} | "
                     f"{was.get('added_p50_ms', '-')} / {was.get('added_p99_ms', '-')} / {was.get('cpu_ms_per_request', '-')} |")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description="Compare a bench.py result with the committed baseline")
    parser.add_argument("result")
    parser.add_argument("--baseline", default=str(pathlib.Path(__file__).with_name("baseline.json")))
    parser.add_argument("--write-baseline", action="store_true", help="replace the baseline with this result")
    parser.add_argument("--strict", action="store_true", help="fail on advisory findings too")
    args = parser.parse_args()
    current = cells(json.loads(pathlib.Path(args.result).read_text())["results"])
    if args.write_baseline:
        pathlib.Path(args.baseline).write_text(json.dumps(current, indent=1, sort_keys=True) + "\n")
        print(f"wrote {args.baseline} with {len(current)} cells")
        return 0
    baseline = json.loads(pathlib.Path(args.baseline).read_text())
    hard, advisory = judge(current, baseline)
    print(summary(current, baseline))
    for line in hard:
        print(f"::error::{line}")
    for line in advisory:
        print(f"::warning::{line}")
    return 1 if hard or (args.strict and advisory) else 0


if __name__ == "__main__":
    sys.exit(main())
