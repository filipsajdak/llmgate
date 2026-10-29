import base64
import json
import os
import struct
import sys
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import fast_http  # noqa: F401

PORT_FILE = sys.argv[1]
LOG = sys.argv[2]
MODE = sys.argv[3] if len(sys.argv) > 3 else "text"
EXPECTED_AUTHORIZATION = os.environ.get("FAKE_REQUIRE_AUTHORIZATION", "")
TOOL_COMMAND = os.environ.get("FAKE_TOOL_COMMAND", "echo tool-ran")
ALLOWED_BETAS = {"interleaved-thinking-2025-05-14", "context-management-2025-06-27"}
USAGE = {"input_tokens": 20, "cache_read_input_tokens": 100, "cache_creation_input_tokens": 0, "output_tokens": 9}
PATH_PREFIX = "/model/anthropic.claude-sonnet-4-6-v1%3A0/invoke-with-response-stream"


def frame(headers, payload, corrupt=False):
    block = b"".join(bytes([len(name)]) + name.encode() + b"\x07" + struct.pack(">H", len(value)) + value.encode() for name, value in headers)
    total = 12 + len(block) + len(payload) + 4
    prelude = struct.pack(">II", total, len(block))
    prelude += struct.pack(">I", zlib.crc32(prelude))
    message = prelude + block + payload
    crc = zlib.crc32(message) ^ (1 if corrupt else 0)
    return message + struct.pack(">I", crc)


def chunk(event, corrupt=False):
    wrapper = json.dumps({"bytes": base64.b64encode(json.dumps(event).encode()).decode()}).encode()
    return frame([(":event-type", "chunk"), (":content-type", "application/json"), (":message-type", "event")], wrapper, corrupt)


def exception(kind, message):
    return frame([(":exception-type", kind), (":content-type", "application/json"), (":message-type", "exception")], json.dumps({"message": message}).encode())


def start():
    return chunk({"type": "message_start", "message": {"id": "msg_bdrk_fake", "type": "message", "role": "assistant", "model": "claude-fake", "content": [], "usage": {"input_tokens": 0, "output_tokens": 0}}})


def text_frames(text):
    return [start(), chunk({"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}}),
            chunk({"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": text[:5]}}),
            chunk({"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": text[5:]}}), chunk({"type": "content_block_stop", "index": 0}),
            chunk({"type": "message_delta", "delta": {"stop_reason": "end_turn", "stop_sequence": None}, "usage": USAGE}),
            chunk({"type": "message_stop", "amazon-bedrock-invocationMetrics": {"inputTokenCount": 120, "outputTokenCount": 9}})]


def tool_frames():
    arguments = json.dumps({"command": TOOL_COMMAND})
    return [start(), chunk({"type": "content_block_start", "index": 0, "content_block": {"type": "tool_use", "id": "toolu_fake", "name": "Bash", "input": {}}}),
            chunk({"type": "content_block_delta", "index": 0, "delta": {"type": "input_json_delta", "partial_json": arguments[:10]}}),
            chunk({"type": "content_block_delta", "index": 0, "delta": {"type": "input_json_delta", "partial_json": arguments[10:]}}), chunk({"type": "content_block_stop", "index": 0}),
            chunk({"type": "message_delta", "delta": {"stop_reason": "tool_use", "stop_sequence": None}, "usage": USAGE}), chunk({"type": "message_stop"})]


def answers_a_tool(request):
    last = (request.get("messages") or [{}])[-1]
    content = last.get("content")
    return isinstance(content, list) and any(isinstance(b, dict) and b.get("type") == "tool_result" for b in content)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def fail(self, status, message):
        body = json.dumps({"message": message}).encode()
        self.send_response(status)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers.get("content-length", 0))) or b"{}")
        with open(LOG, "a") as log:
            log.write(json.dumps({"path": self.path, "headers": {k.lower(): v for k, v in self.headers.items()}, "request": request}) + "\n")
        if self.path != PATH_PREFIX:
            return self.fail(404, "unknown path " + self.path)
        if EXPECTED_AUTHORIZATION and self.headers.get("authorization") != EXPECTED_AUTHORIZATION:
            return self.fail(403, "bad key")
        if "FAKE_400" in json.dumps(request.get("messages")):
            return self.fail(400, "messages.1.output_config: Extra inputs are not permitted")
        if MODE == "rate_limited":
            return self.fail(429, "Too many requests, please wait before trying again.")
        if "model" in request or "stream" in request or request.get("anthropic_version") != "bedrock-2023-05-31":
            return self.fail(400, "model and stream belong in the URL and anthropic_version is required")
        if any(m.get("role") not in ("user", "assistant") for m in request.get("messages") or []):
            return self.fail(400, "messages.1.output_config: Extra inputs are not permitted")
        if not set(request.get("anthropic_beta") or []) <= ALLOWED_BETAS:
            return self.fail(400, "invalid beta flag")
        text = json.dumps(request.get("messages"))
        if "FAKE_EXCEPTION" in text:
            frames = [start(), exception("throttlingException", "Too many tokens, slow down")]
        elif "FAKE_CORRUPT" in text:
            frames = [start(), chunk({"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}}, corrupt=True)]
        elif MODE == "tool" and request.get("tools") and not answers_a_tool(request):
            frames = tool_frames()
        else:
            frames = text_frames("Tool finished: all good" if MODE == "tool" else "Hello from the fake Bedrock")
        self.send_response(200)
        self.send_header("content-type", "application/vnd.amazon.eventstream")
        self.send_header("transfer-encoding", "chunked")
        self.end_headers()
        for item in frames:
            self.wfile.write(f"{len(item):x}\r\n".encode() + item + b"\r\n")
            self.wfile.flush()
        self.wfile.write(b"0\r\n\r\n")


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
server.daemon_threads = True
with open(PORT_FILE, "w") as port_file:
    port_file.write(str(server.server_address[1]))
server.serve_forever()
