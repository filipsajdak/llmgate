#!/usr/bin/env python3
import argparse
import asyncio
import json
import ssl
import sys
import time
from urllib.parse import urlsplit


def request_body(stream, kb):
    padding = "x" * max(0, kb * 1024 - 200)
    return json.dumps({"model": "sonnet", "max_tokens": 64, "stream": stream,
                       "messages": [{"role": "user", "content": "hi " + padding}]}, separators=(",", ":")).encode()


def percentile(sorted_values, fraction):
    if not sorted_values:
        return 0.0
    return sorted_values[min(len(sorted_values) - 1, int(fraction * len(sorted_values)))]


def summarise(values):
    ordered = sorted(values)
    return {"p50": round(percentile(ordered, 0.50) * 1000, 3), "p95": round(percentile(ordered, 0.95) * 1000, 3),
            "p99": round(percentile(ordered, 0.99) * 1000, 3), "max": round((ordered[-1] if ordered else 0.0) * 1000, 3)}


async def one_request(reader, writer, payload):
    started = time.perf_counter()
    writer.write(payload)
    await writer.drain()
    first = await reader.readexactly(1)
    first_byte = time.perf_counter()
    head = first + await reader.readuntil(b"\r\n\r\n")
    status = int(head.split(b" ", 2)[1])
    lowered = head.lower()
    first_delta = None
    if b"transfer-encoding: chunked" in lowered:
        while True:
            size = int((await reader.readline()).strip() or b"0", 16)
            data = await reader.readexactly(size + 2)
            if first_delta is None and b"content_block_delta" in data:
                first_delta = time.perf_counter()
            if size == 0:
                break
    else:
        length = 0
        for line in lowered.split(b"\r\n"):
            if line.startswith(b"content-length:"):
                length = int(line.split(b":", 1)[1])
        await reader.readexactly(length)
    done = time.perf_counter()
    return status, first_byte - started, (first_delta - started) if first_delta else None, done - started, b"connection: close" in lowered


async def worker(host, port, payload, state, context):
    reader = writer = None
    while state["remaining"] > 0 or state["deadline"] > time.perf_counter():
        if state["remaining"] <= 0 and state["count_mode"]:
            break
        if state["count_mode"]:
            state["remaining"] -= 1
        try:
            if writer is None:
                reader, writer = await asyncio.open_connection(host, port, ssl=context, server_hostname=host if context else None)
            status, ttfb, tdelta, total, closing = await one_request(reader, writer, payload)
        except (asyncio.IncompleteReadError, OSError, ValueError) as error:
            state["errors"] += 1
            state["error_kinds"][type(error).__name__] = state["error_kinds"].get(type(error).__name__, 0) + 1
            if writer is not None:
                writer.close()
            reader = writer = None
            await asyncio.sleep(0.01)
            continue
        if status != 200:
            state["errors"] += 1
            state["error_kinds"][f"HTTP {status}"] = state["error_kinds"].get(f"HTTP {status}", 0) + 1
        elif state["seen"] >= state["warmup"]:
            state["ttfb"].append(ttfb)
            state["total"].append(total)
            if tdelta is not None:
                state["delta"].append(tdelta)
        state["seen"] += 1
        if closing:
            writer.close()
            reader = writer = None
    if writer is not None:
        writer.close()


async def run(url, key, concurrency, stream, kb, seconds, requests, warmup, cafile=None):
    parts = urlsplit(url)
    context = ssl.create_default_context(cafile=cafile) if parts.scheme == "https" else None
    host, port, path = parts.hostname, parts.port, parts.path or "/"
    body = request_body(stream, kb)
    payload = (f"POST {path} HTTP/1.1\r\nHost: {host}:{port}\r\nx-api-key: {key}\r\ncontent-type: application/json\r\n"
               f"anthropic-version: 2023-06-01\r\ncontent-length: {len(body)}\r\n\r\n").encode() + body
    state = {"remaining": requests if requests else 0, "count_mode": bool(requests), "deadline": time.perf_counter() + seconds,
             "errors": 0, "error_kinds": {}, "seen": 0, "warmup": warmup, "ttfb": [], "total": [], "delta": []}
    begun = time.perf_counter()
    await asyncio.gather(*(worker(host, port, payload, state, context) for _ in range(concurrency)))
    elapsed = time.perf_counter() - begun
    measured = len(state["total"])
    return {"concurrency": concurrency, "stream": stream, "body_kb": kb, "requests": state["seen"], "errors": state["errors"], "error_kinds": state["error_kinds"],
            "seconds": round(elapsed, 3), "rps": round(state["seen"] / elapsed, 1) if elapsed else 0.0,
            "ttfb_ms": summarise(state["ttfb"]), "first_delta_ms": summarise(state["delta"]) if state["delta"] else None,
            "total_ms": summarise(state["total"]), "measured": measured}


def main():
    parser = argparse.ArgumentParser(description="Closed-loop keep-alive load driver for POST /v1/messages")
    parser.add_argument("url")
    parser.add_argument("--key", default="bench-client-key")
    parser.add_argument("--concurrency", type=int, default=1)
    parser.add_argument("--no-stream", action="store_true")
    parser.add_argument("--body-kb", type=int, default=2)
    parser.add_argument("--seconds", type=float, default=5.0)
    parser.add_argument("--requests", type=int, default=0, help="stop after this many requests instead of after --seconds")
    parser.add_argument("--warmup", type=int, default=20)
    parser.add_argument("--cafile", help="trust this PEM certificate for an https url")
    args = parser.parse_args()
    result = asyncio.run(run(args.url, args.key, args.concurrency, not args.no_stream, args.body_kb, args.seconds, args.requests, args.warmup, args.cafile))
    json.dump(result, sys.stdout, indent=2)
    print()
    return 1 if result["errors"] or not result["measured"] else 0


if __name__ == "__main__":
    sys.exit(main())
