import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import fast_http  # noqa: F401

PORT_FILE = sys.argv[1]
LOG = sys.argv[2]
MODE = sys.argv[3] if len(sys.argv) > 3 else "text"
EXPECTED_KEY = os.environ.get("FAKE_REQUIRE_KEY", "")
TOOL_COMMAND = os.environ.get("FAKE_TOOL_COMMAND", "echo tool-ran")
REFUSED = ["max_tokens", "stop", "temperature", "top_p"]
USAGE = {"prompt_tokens": 120, "completion_tokens": 9, "total_tokens": 129, "prompt_tokens_details": {"cached_tokens": 100}}


def chunk(delta, reason=None, usage=None, choices=True):
    body = {"id": "chatcmpl-fake", "object": "chat.completion.chunk", "model": "gpt-fake", "choices": [{"index": 0, "delta": delta, "finish_reason": reason}] if choices else []}
    if usage is not None:
        body["usage"] = usage
    return json.dumps(body)


def text_chunks(text):
    return [chunk({"role": "assistant", "content": ""}), chunk({"content": text[:5]}), chunk({"content": text[5:]}), chunk({}, "stop"), chunk({}, None, USAGE, choices=False), "[DONE]"]


def tool_chunks(name):
    arguments = json.dumps({"command": TOOL_COMMAND})
    first = {"tool_calls": [{"index": 0, "id": "call_fake", "type": "function", "function": {"name": name, "arguments": ""}}]}
    return [chunk({"role": "assistant", "content": None}), chunk(first), chunk({"tool_calls": [{"index": 0, "function": {"arguments": arguments[:10]}}]}),
            chunk({"tool_calls": [{"index": 0, "function": {"arguments": arguments[10:]}}]}), chunk({}, "tool_calls"), chunk({}, None, USAGE, choices=False), "[DONE]"]


def answers_a_tool(request):
    messages = request.get("messages") or []
    return bool(messages) and messages[-1].get("role") == "tool"


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def fail(self, status, message):
        body = json.dumps({"error": {"message": message, "type": "invalid_request_error"}}).encode()
        self.send_response(status)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers.get("content-length", 0))) or b"{}")
        with open(LOG, "a") as log:
            log.write(json.dumps({"path": self.path, "headers": {k.lower(): v for k, v in self.headers.items()}, "request": request}) + "\n")
        if not self.path.startswith("/openai/deployments/gpt-fake/chat/completions?api-version="):
            return self.fail(404, "unknown path " + self.path)
        if EXPECTED_KEY and self.headers.get("api-key") != EXPECTED_KEY:
            return self.fail(401, "bad key")
        if "FAKE_400" in json.dumps(request.get("messages")):
            return self.fail(400, "bad field xyz")
        if MODE == "rate_limited":
            return self.fail(429, "Rate limit reached")
        refused = [name for name in REFUSED if name in request]
        if refused:
            return self.fail(400, "Unsupported parameter: " + ", ".join(refused))
        if request.get("tools") and "reasoning_effort" in request:
            return self.fail(400, "Function tools with reasoning_effort are not supported")
        if request.get("stream") is not True or (request.get("stream_options") or {}).get("include_usage") is not True:
            return self.fail(400, "stream and stream_options.include_usage must be set")
        if "FAKE_ERROR" in json.dumps(request.get("messages")):
            events = ["not json", json.dumps({"error": {"message": "overloaded", "code": "rate_limit_exceeded"}})]
        elif MODE == "tool" and request.get("tools") and not answers_a_tool(request):
            events = tool_chunks(request["tools"][-1]["function"]["name"])
        else:
            events = text_chunks("Tool finished: all good" if MODE == "tool" else "Hello from the fake chat upstream")
        self.send_response(200)
        self.send_header("content-type", "text/event-stream; charset=utf-8")
        self.send_header("transfer-encoding", "chunked")
        self.end_headers()
        for data in events:
            frame = f"data: {data}\n\n".encode()
            self.wfile.write(f"{len(frame):x}\r\n".encode() + frame + b"\r\n")
            self.wfile.flush()
        self.wfile.write(b"0\r\n\r\n")


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
server.daemon_threads = True
with open(PORT_FILE, "w") as port_file:
    port_file.write(str(server.server_address[1]))
server.serve_forever()
