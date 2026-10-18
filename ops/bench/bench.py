#!/usr/bin/env python3
import argparse
import asyncio
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import time
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
KEY = "bench-client-key"
sys.path.insert(0, str(HERE))
import load


def wait_for(predicate, what, seconds=15.0):
    deadline = time.time() + seconds
    while time.time() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.05)
    raise SystemExit(f"timed out waiting for {what}")


def read_text(path):
    try:
        return pathlib.Path(path).read_text()
    except OSError:
        return ""


def make_certificate(work):
    cert, key = work / "cert.pem", work / "key.pem"
    if not cert.exists():
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", str(key), "-out", str(cert), "-days", "2", "-subj", "/CN=localhost",
                        "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1"], check=True, capture_output=True)
    return cert, key


def start_upstream(work, events, delay_ms, tls=False):
    port_file = work / f"upstream-{events}-{delay_ms}-{int(tls)}.port"
    command = [sys.executable, str(HERE / "upstream.py"), str(port_file), "--events", str(events), "--delay-ms", str(delay_ms)]
    if tls:
        cert, key = make_certificate(work)
        command += ["--tls-cert", str(cert), "--tls-key", str(key)]
    proc = subprocess.Popen(command)
    return proc, int(wait_for(lambda: read_text(port_file).strip(), "the upstream port"))


def start_gateway(binary, work, upstream_port, ledger, tls=False):
    config = work / f"config-{upstream_port}-{'on' if ledger else 'off'}-{pathlib.Path(binary).parent.parent.parent.name}.jsonc"
    scheme = "https" if tls else "http"
    config.write_text(json.dumps({"listen": "127.0.0.1:0", "upstream": {"base_url": f"{scheme}://127.0.0.1:{upstream_port}", "messages_path": "/v1/messages"},
                                  "models": {"sonnet": "bench-sonnet"}, "static_token_env": "LLMGATE_TEST_TOKEN", "client_key_env": "LLMGATE_CLIENT_KEY",
                                  "usage": {"ledger_file": str(work / f"usage-{upstream_port}.jsonl") if ledger else ""}}))
    log = work / f"gateway-{config.stem}.log"
    env = dict(os.environ, LLMGATE_TEST_TOKEN="bench-upstream-token", LLMGATE_CLIENT_KEY=KEY)
    if tls:
        env["SSL_CERT_FILE"] = str(work / "cert.pem")
    proc = subprocess.Popen([str(binary), "--config", str(config)], env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT)
    port = wait_for(lambda: re.search(r"listening on 127\.0\.0\.1:(\d+)", read_text(log)), "the gateway to listen")
    return proc, int(port.group(1))


def process_numbers(pid):
    out = subprocess.run(["ps", "-o", "rss=,cputime=", "-p", str(pid)], capture_output=True, text=True).stdout.split()
    if len(out) != 2:
        return 0, 0.0
    seconds = 0.0
    for part in out[1].split(":"):
        seconds = seconds * 60 + float(part)
    return int(out[0]), seconds


def time_wait_count():
    out = subprocess.run(["netstat", "-an"], capture_output=True, text=True).stdout
    return out.count("TIME_WAIT")


def settle_ports(limit=3000, seconds=90.0):
    deadline = time.time() + seconds
    while time_wait_count() > limit and time.time() < deadline:
        time.sleep(1)


def gateway_counters(url):
    request = urllib.request.Request(url.rsplit("/anthropic/", 1)[0] + "/llmgate/stats", headers={"x-api-key": KEY})
    for attempt in range(30):
        try:
            with urllib.request.urlopen(request, timeout=5) as reply:
                stats = json.load(reply)
            return stats["connections_opened"], stats["connections_reused"]
        except OSError:
            time.sleep(2)
    raise SystemExit("the gateway did not answer /llmgate/stats within a minute")


def measure(label, url, pid, args, concurrency, stream, kb, warmup):
    if pid:
        settle_ports()
    counters = gateway_counters(url) if pid else (0, 0)
    before = process_numbers(pid) if pid else (0, 0.0)
    result = asyncio.run(load.run(url, KEY, concurrency, stream, kb, args.seconds, 0, warmup, args.cafile))
    after = process_numbers(pid) if pid else (0, 0.0)
    result.update(label=label, rss_mb=round(after[0] / 1024, 1))
    if pid:
        end = gateway_counters(url)
        result.update(upstream_opened=end[0] - counters[0], upstream_reused=end[1] - counters[1])
    if pid and result["requests"]:
        result["cpu_ms_per_request"] = round((after[1] - before[1]) * 1000 / result["requests"], 3)
    return result


def table(results):
    print(f"{'target':<22}{'conc':>5}{'KB':>5}{'rps':>9}{'ttfb p50':>10}{'p99':>8}{'delta p50':>11}{'total p50':>11}{'p99':>8}{'err':>5}{'cpu ms/req':>12}{'rss MB':>8}{'reused/opened':>14}")
    for r in results:
        delta = r["first_delta_ms"]["p50"] if r["first_delta_ms"] else float("nan")
        print(f"{r['label']:<22}{r['concurrency']:>5}{r['body_kb']:>5}{r['rps']:>9}{r['ttfb_ms']['p50']:>10}{r['ttfb_ms']['p99']:>8}{delta:>11}"
              f"{r['total_ms']['p50']:>11}{r['total_ms']['p99']:>8}{r['errors']:>5}{r.get('cpu_ms_per_request', ''):>12}{r['rss_mb'] or '':>8}"
              f"{(str(r['upstream_reused']) + '/' + str(r['upstream_opened'])) if 'upstream_opened' in r else '':>14}")


def overhead(results):
    print("\nadded latency through the gateway (total p50 / p99, milliseconds)")
    direct = {(r["concurrency"], r["body_kb"], r["scenario"]): r for r in results if r["label"] == "direct"}
    for r in results:
        base = direct.get((r["concurrency"], r["body_kb"], r["scenario"]))
        if r["label"] != "direct" and base:
            print(f"  {r['scenario']:<10}{r['label']:<22} conc {r['concurrency']:>3}  {r['body_kb']:>4} KB   "
                  f"+{r['total_ms']['p50'] - base['total_ms']['p50']:.2f} / +{r['total_ms']['p99'] - base['total_ms']['p99']:.2f}   "
                  f"ttfb +{r['ttfb_ms']['p50'] - base['ttfb_ms']['p50']:.2f} / +{r['ttfb_ms']['p99'] - base['ttfb_ms']['p99']:.2f}")


def main():
    parser = argparse.ArgumentParser(description="Measure what the gateway adds to a streamed Messages request")
    parser.add_argument("--binary", default=str(ROOT / "build/host/llmgate"))
    parser.add_argument("--seconds", type=float, default=5.0)
    parser.add_argument("--warmup", type=int, default=50)
    parser.add_argument("--concurrency", type=int, nargs="+", default=[1, 8, 32])
    parser.add_argument("--body-kb", type=int, nargs="+", default=[2, 100])
    parser.add_argument("--paced", action="store_true", help="also run a paced upstream (20 ms per event) to check the gateway streams instead of buffering")
    parser.add_argument("--tls", action="store_true", help="also run against an HTTPS upstream with a self-signed certificate (needs openssl)")
    parser.add_argument("--before-binary", help="a second llmgate binary to compare in the TLS scenario, for example one built before a fix")
    parser.add_argument("--out", default=str(ROOT / "build/bench"))
    args = parser.parse_args()
    if not pathlib.Path(args.binary).exists():
        raise SystemExit(f"{args.binary} does not exist; build it first")
    work = pathlib.Path(tempfile.mkdtemp(prefix="llmgate-bench-"))
    procs, results = [], []
    args.cafile = None
    scenarios = [("burst", 30, 0.0, args.concurrency, args.body_kb, args.warmup, False)]
    if args.paced:
        scenarios.append(("paced", 30, 20.0, [1, 8], [2], 1, False))
    if args.tls:
        scenarios.append(("tls", 30, 0.0, [1, 8], [2, 100], args.warmup, True))
    try:
        for scenario, events, delay, concs, sizes, warmup, tls in scenarios:
            upstream, up_port = start_upstream(work, events, delay, tls)
            procs.append(upstream)
            args.cafile = str(work / "cert.pem") if tls else None
            targets = [("direct", f"{'https' if tls else 'http'}://127.0.0.1:{up_port}/v1/messages", None)]
            variants = [("gateway", args.binary, True)] + ([("gateway before fix", args.before_binary, True)] if tls and args.before_binary else [])
            if not tls:
                variants = [("gateway ledger off", args.binary, False), ("gateway ledger on", args.binary, True)]
            for name, binary, ledger in variants:
                gateway, gw_port = start_gateway(binary, work, up_port, ledger, tls)
                procs.append(gateway)
                targets.append((name, f"http://127.0.0.1:{gw_port}/anthropic/v1/messages", gateway.pid))
            for conc in concs:
                for kb in sizes:
                    for label, url, pid in targets:
                        result = measure(label, url, pid, args, conc, True, kb, warmup)
                        result["scenario"] = scenario
                        results.append(result)
                        print(f"  {scenario} {label} conc={conc} {kb}KB: {result['rps']} rps, errors {result['errors']} {result['error_kinds'] or ''}", file=sys.stderr, flush=True)
    finally:
        for proc in procs:
            proc.terminate()
        for proc in procs:
            proc.wait()
    for scenario in dict.fromkeys(r["scenario"] for r in results):
        print(f"\n== {scenario} upstream")
        table([r for r in results if r["scenario"] == scenario])
    overhead(results)
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%dT%H%M%S")
    (out / f"{stamp}.json").write_text(json.dumps({"platform": sys.platform, "seconds": args.seconds, "results": results}, indent=1))
    print(f"\nsaved {out / (stamp + '.json')}")
    return 1 if any(r["errors"] for r in results) else 0


if __name__ == "__main__":
    sys.exit(main())
