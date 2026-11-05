import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import fast_http  # noqa: F401

PORT_FILE = sys.argv[1]
LOG = sys.argv[2]
EXPECTED_AUTHORIZATION = os.environ.get("FAKE_REQUIRE_AUTHORIZATION", "")
EXPECTED_PATH = os.environ.get("FAKE_VERTEX_PATH", "")
TOOL_COMMAND = os.environ.get("FAKE_TOOL_COMMAND", "echo tool-ran")
KNOWN_BETAS = {"interleaved-thinking-2025-05-14", "context-1m-2025-08-07"}
USAGE = {"input_tokens": 20, "cache_read_input_tokens": 100, "cache_creation_input_tokens": 0, "output_tokens": 9}


def sse(event, data):
    return f"event: {event}\ndata: {json.dumps(data)}\n\n".encode()


def start():
    return sse("message_start", {"type": "message_start", "message": {"id": "msg_vrtx_fake", "type": "message", "role": "assistant", "model": "claude-fake", "content": [], "stop_reason": None, "stop_sequence": None, "usage": USAGE}})


def text_events(text):
    return [start(), sse("ping", {"type": "ping"}),
            sse("content_block_start", {"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}}),
            sse("content_block_delta", {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": text}}),
            sse("content_block_stop", {"type": "content_block_stop", "index": 0}),
            sse("message_delta", {"type": "message_delta", "delta": {"stop_reason": "end_turn", "stop_sequence": None}, "usage": USAGE}),
            sse("message_stop", {"type": "message_stop"})]


def tool_events():
    arguments = json.dumps({"command": TOOL_COMMAND})
    return [start(), sse("content_block_start", {"type": "content_block_start", "index": 0, "content_block": {"type": "tool_use", "id": "toolu_vrtx_fake", "name": "Bash", "input": {}}}),
            sse("content_block_delta", {"type": "content_block_delta", "index": 0, "delta": {"type": "input_json_delta", "partial_json": arguments[:10]}}),
            sse("content_block_delta", {"type": "content_block_delta", "index": 0, "delta": {"type": "input_json_delta", "partial_json": arguments[10:]}}),
            sse("content_block_stop", {"type": "content_block_stop", "index": 0}),
            sse("message_delta", {"type": "message_delta", "delta": {"stop_reason": "tool_use", "stop_sequence": None}, "usage": USAGE}),
            sse("message_stop", {"type": "message_stop"})]


def answers_a_tool(request):
    content = (request.get("messages") or [{}])[-1].get("content")
    return isinstance(content, list) and any(isinstance(b, dict) and b.get("type") == "tool_result" for b in content)


def problem(request):
    if "model" in request:
        return "model: Extra inputs are not permitted"
    if request.get("anthropic_version") != "vertex-2023-10-16":
        return "anthropic_version: Field required"
    if "context_management" in request:
        return "context_management: Extra inputs are not permitted"
    unknown = [token for token in request.get("anthropic_beta", []) if token not in KNOWN_BETAS]
    if unknown:
        return f"Unexpected value(s) `{unknown[0]}` for the `anthropic_beta` parameter."
    if (request.get("tool_choice") or {}).get("type") in ("any", "tool"):
        return 'tool_choice: type "tool" and "any" are not supported for this model.'
    if any(message.get("role") == "system" for message in request.get("messages") or []):
        return "messages: role 'system' must follow a 'user' message"
    return ""


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def fail(self, status, message):
        body = json.dumps({"type": "error", "error": {"type": "invalid_request_error", "message": message}}).encode()
        self.send_response(status)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def whole(self, request):
        body = json.dumps({"id": "msg_vrtx_whole", "type": "message", "role": "assistant", "model": "claude-fake", "content": [{"type": "text", "text": "whole answer"}],
                           "stop_reason": "end_turn", "stop_sequence": None, "usage": USAGE}).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("content-length", "0")))
        request = json.loads(raw or b"{}")
        with open(LOG, "a") as log:
            log.write(json.dumps({"path": self.path, "authorization": self.headers.get("authorization", ""), "anthropic_beta_header": self.headers.get("anthropic-beta", ""), "body": request}) + "\n")
        if EXPECTED_AUTHORIZATION and self.headers.get("authorization") != EXPECTED_AUTHORIZATION:
            return self.fail(401, "the bearer key is wrong")
        if EXPECTED_PATH and not (self.path == EXPECTED_PATH or self.path == EXPECTED_PATH.replace(":streamRawPredict", ":rawPredict")):
            return self.fail(404, f"unexpected path {self.path}")
        if message := problem(request):
            return self.fail(400, message)
        events = text_events("tool done") if answers_a_tool(request) else tool_events()
        if self.path.endswith(":rawPredict"):
            if request.get("stream"):
                return self.fail(400, "stream is not allowed on rawPredict in this fake")
            return self.whole(request)
        if not request.get("stream"):
            return self.fail(400, "streamRawPredict needs stream true in this fake")
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.send_header("transfer-encoding", "chunked")
        self.end_headers()
        for event in events:
            self.wfile.write(f"{len(event):x}\r\n".encode() + event + b"\r\n")
        self.wfile.write(b"0\r\n\r\n")


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
open(PORT_FILE, "w").write(str(server.server_address[1]))
server.serve_forever()
