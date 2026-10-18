#!/usr/bin/env python3
import argparse
import asyncio
import json

parser = argparse.ArgumentParser(description="Fast fake Anthropic upstream for benchmarking: no logging, prebuilt replies")
parser.add_argument("port_file")
parser.add_argument("--events", type=int, default=30, help="content_block_delta events per streamed reply")
parser.add_argument("--delay-ms", type=float, default=0.0, help="pause before each event; 0 sends the whole reply in one write")
args = parser.parse_args()


def sse(name, data):
    return f"event: {name}\ndata: {json.dumps(data, separators=(',', ':'))}\n\n".encode()


def chunk(data):
    return f"{len(data):x}\r\n".encode() + data + b"\r\n"


start = {"type": "message_start", "message": {"id": "msg_bench", "type": "message", "role": "assistant", "model": "bench",
         "content": [], "stop_reason": None, "stop_sequence": None, "usage": {"input_tokens": 120, "output_tokens": 1}}}
EVENTS = [sse("message_start", start), sse("content_block_start", {"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}})]
EVENTS += [sse("content_block_delta", {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": "word "}})] * args.events
EVENTS += [sse("content_block_stop", {"type": "content_block_stop", "index": 0}),
           sse("message_delta", {"type": "message_delta", "delta": {"stop_reason": "end_turn", "stop_sequence": None}, "usage": {"output_tokens": args.events}}),
           sse("message_stop", {"type": "message_stop"})]
STREAM_HEAD = b"HTTP/1.1 200 OK\r\ncontent-type: text/event-stream\r\ncache-control: no-cache\r\ntransfer-encoding: chunked\r\n\r\n"
STREAM_ALL = STREAM_HEAD + b"".join(chunk(event) for event in EVENTS) + b"0\r\n\r\n"
BODY = json.dumps({"id": "msg_bench", "type": "message", "role": "assistant", "model": "bench", "content": [{"type": "text", "text": "word " * args.events}],
                   "stop_reason": "end_turn", "stop_sequence": None, "usage": {"input_tokens": 120, "output_tokens": args.events}}, separators=(",", ":")).encode()
PLAIN = b"HTTP/1.1 200 OK\r\ncontent-type: application/json\r\ncontent-length: " + str(len(BODY)).encode() + b"\r\n\r\n" + BODY


async def serve(reader, writer):
    try:
        while True:
            head = await reader.readuntil(b"\r\n\r\n")
            length = 0
            for line in head.split(b"\r\n"):
                if line.lower().startswith(b"content-length:"):
                    length = int(line.split(b":", 1)[1])
            body = await reader.readexactly(length) if length else b""
            if b'"stream":true' not in body.replace(b'"stream": true', b'"stream":true'):
                writer.write(PLAIN)
            elif args.delay_ms <= 0:
                writer.write(STREAM_ALL)
            else:
                writer.write(STREAM_HEAD)
                for event in EVENTS:
                    await writer.drain()
                    await asyncio.sleep(args.delay_ms / 1000)
                    writer.write(chunk(event))
                writer.write(b"0\r\n\r\n")
            await writer.drain()
    except (asyncio.IncompleteReadError, ConnectionError):
        pass
    finally:
        writer.close()


async def main():
    server = await asyncio.start_server(serve, "127.0.0.1", 0, backlog=1024)
    with open(args.port_file, "w") as handle:
        handle.write(str(server.sockets[0].getsockname()[1]))
    async with server:
        await server.serve_forever()


asyncio.run(main())
