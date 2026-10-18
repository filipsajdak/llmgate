#!/usr/bin/env python3
import argparse
import asyncio
import json
import os
import pathlib
import random
import re
import signal
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
import bench
import load

POOL_IDLE_MAX = 8
FD_SLACK = 2
TIMELINE_POINTS = 12
KINDS = (("stream", 62), ("plain", 10), ("vanish", 12), ("half", 5), ("malformed", 6), ("wrong_key", 5))


def head_of(path, key, length, host):
    return (f"POST {path} HTTP/1.1\r\nHost: {host}\r\nx-api-key: {key}\r\ncontent-type: application/json\r\n"
            f"anthropic-version: 2023-06-01\r\ncontent-length: {length}\r\n\r\n").encode()


async def status_only(reader):
    head = await reader.readuntil(b"\r\n\r\n")
    return int(head.split(b" ", 2)[1])


async def one(kind, host, port, key, kb, rng):
    path = "/anthropic/v1/messages"
    where = f"{host}:{port}"
    reader, writer = await asyncio.open_connection(host, port)
    try:
        if kind in ("stream", "plain"):
            body = load.request_body(kind == "stream", kb)
            payload = head_of(path, key, len(body), where) + body
            for _ in range(rng.randint(1, 3)):
                status = (await load.one_request(reader, writer, payload))[0]
            return status
        if kind == "vanish":
            body = load.request_body(True, kb)
            writer.write(head_of(path, key, len(body), where) + body)
            await writer.drain()
            await reader.readexactly(rng.randint(1, 600))
            return "vanished"
        if kind == "half":
            writer.write(head_of(path, key, 5000, where) + b'{"model":"sonnet"')
            await writer.drain()
            await asyncio.sleep(rng.random() * 0.05)
            return "half"
        body = b"{not json" if kind == "malformed" else load.request_body(True, 1)
        used = key if kind == "malformed" else "wrong-" + key
        writer.write(head_of(path, used, len(body), where) + body)
        await writer.drain()
        return await status_only(reader)
    finally:
        writer.close()


async def worker(deadline, host, port, key, pause, outcomes, rng):
    kinds = [name for name, weight in KINDS for _ in range(weight)]
    while time.time() < deadline:
        kind = rng.choice(kinds)
        try:
            outcome = await one(kind, host, port, key, rng.choice((2, 20, 100)), rng)
        except (asyncio.IncompleteReadError, OSError, ValueError) as error:
            outcome = type(error).__name__
        outcomes[(kind, outcome)] = outcomes.get((kind, outcome), 0) + 1
        await asyncio.sleep(pause)


def open_files(pid):
    proc = pathlib.Path(f"/proc/{pid}/fd")
    if proc.exists():
        return len(list(proc.iterdir()))
    out = subprocess.run(["lsof", "-p", str(pid)], capture_output=True, text=True).stdout
    return max(0, len(out.splitlines()) - 1)


def rss_kb(pid):
    status = pathlib.Path(f"/proc/{pid}/status")
    if status.exists():
        found = re.search(r"VmRSS:\s+(\d+)", status.read_text())
        return int(found.group(1)) if found else 0
    out = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
    return int(out) if out else 0


async def sampler(pid, deadline, interval, samples, started):
    while time.time() < deadline:
        samples.append((round(time.time() - started, 1), rss_kb(pid), open_files(pid)))
        await asyncio.sleep(interval)


def slope_kb_per_minute(samples):
    points = [(t, r) for t, r, _ in samples]
    n = len(points)
    if n < 3:
        return 0.0
    mean_t = sum(t for t, _ in points) / n
    mean_r = sum(r for _, r in points) / n
    denominator = sum((t - mean_t) ** 2 for t, _ in points)
    return 60 * sum((t - mean_t) * (r - mean_r) for t, r in points) / denominator if denominator else 0.0


async def run(args, port, pid, stats_url, started):
    outcomes, samples = {}, []
    rng = random.Random(args.seed)
    deadline = time.time() + args.minutes * 60
    workers = [worker(deadline, "127.0.0.1", port, bench.KEY, args.pause, outcomes, random.Random(rng.random())) for _ in range(args.concurrency)]
    await asyncio.gather(sampler(pid, deadline, args.interval, samples, started), *workers)
    await asyncio.sleep(args.settle)
    return outcomes, samples


def main():
    parser = argparse.ArgumentParser(description="Hostile-mix soak: leaks in memory and file descriptors, and a clean shutdown")
    parser.add_argument("--binary", default=str(ROOT / "build/host/llmgate"))
    parser.add_argument("--minutes", type=float, default=10.0)
    parser.add_argument("--concurrency", type=int, default=8)
    parser.add_argument("--pause", type=float, default=0.005)
    parser.add_argument("--interval", type=float, default=5.0)
    parser.add_argument("--settle", type=float, default=5.0)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--warmup-fraction", type=float, default=0.2, help="ignore this share of the samples when fitting the memory slope")
    args = parser.parse_args()
    work = pathlib.Path(tempfile.mkdtemp(prefix="llmgate-soak-"))
    upstream, up_port = bench.start_upstream(work, 30, 2.0)
    gateway, port = bench.start_gateway(args.binary, work, up_port, True)
    stats_url = f"http://127.0.0.1:{port}/anthropic/v1/messages"
    started = time.time()
    idle_fds = open_files(gateway.pid)
    try:
        outcomes, samples = asyncio.run(run(args, port, gateway.pid, stats_url, started))
        final_fds = open_files(gateway.pid)
        final_rss = rss_kb(gateway.pid)
        counters = json.loads(__import__("urllib.request").request.urlopen(__import__("urllib.request").request.Request(
            stats_url.rsplit("/anthropic/", 1)[0] + "/llmgate/stats", headers={"x-api-key": bench.KEY}), timeout=10).read())
    finally:
        gateway.send_signal(signal.SIGTERM)
        try:
            code = gateway.wait(timeout=30)
        except subprocess.TimeoutExpired:
            gateway.kill()
            code = "killed after 30 s"
        upstream.terminate()
        upstream.wait()
    log = bench.read_text(next(work.glob("gateway-*.log")))
    ledger = sum(p.stat().st_size for p in work.glob("usage-*.jsonl"))
    kept = samples[int(len(samples) * args.warmup_fraction):]
    total = sum(outcomes.values())
    print(f"soak: {args.minutes} min, {args.concurrency} workers, {total} connections, seed {args.seed}")
    for (kind, outcome), count in sorted(outcomes.items()):
        print(f"  {kind:<10} {str(outcome):<22} {count}")
    print(f"idle file descriptors {idle_fds}; after the load {final_fds} (the pool may keep {POOL_IDLE_MAX} idle upstream connections)")
    print(f"rss: first {samples[0][1] // 1024} MB, max {max(r for _, r, _ in samples) // 1024} MB, final {final_rss // 1024} MB; "
          f"fit after warmup {slope_kb_per_minute(kept):+.0f} KB/min over {len(kept)} samples")
    step = max(1, len(samples) // TIMELINE_POINTS)
    print("rss MB over time:", " ".join(f"{r // 1024}" for _, r, _ in samples[::step]))
    print(f"fd max {max(f for _, _, f in samples)}; requests {counters['requests']}, streamed {counters['streamed']}, client_disconnects {counters['client_disconnects']}, "
          f"opened {counters['connections_opened']}, reused {counters['connections_reused']}")
    print(f"ledger {ledger // 1024} KB; gateway exit code {code}")
    leak = re.search(r"LeakSanitizer|AddressSanitizer|ThreadSanitizer|runtime error", log)
    print("sanitizer report in the gateway log:", "YES" if leak else "no")
    (work / "samples.json").write_text(json.dumps(samples))
    print(f"samples: {work / 'samples.json'}")
    problems = [final_fds > idle_fds + POOL_IDLE_MAX + FD_SLACK, code != 0, bool(leak)]
    return 1 if any(problems) else 0


if __name__ == "__main__":
    sys.exit(main())
