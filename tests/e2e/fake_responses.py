import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import fast_http  # noqa: F401

PORT_FILE = sys.argv[1]
LOG = sys.argv[2]
MODE = sys.argv[3] if len(sys.argv) > 3 else "text"
EXPECTED_AUTHORIZATION = os.environ.get("FAKE_REQUIRE_AUTHORIZATION", "")
TOOL_COMMAND = os.environ.get("FAKE_TOOL_COMMAND", "echo tool-ran")
REJECTED = ["background", "conversation", "max_output_tokens", "max_tool_calls", "metadata", "moderation", "multi_agent", "prompt",
            "prompt_cache_retention", "safety_identifier", "temperature", "top_logprobs", "top_p", "truncation", "user", "previous_response_id"]
USAGE = {"input_tokens": 120, "input_tokens_details": {"cached_tokens": 100}, "output_tokens": 9, "output_tokens_details": {"reasoning_tokens": 0}, "total_tokens": 129}


def sse(data):
    return f"event: {data['type']}\ndata: {json.dumps(data)}\n\n".encode()


def response(status, extra=None):
    body = {"id": "resp_fake", "model": "gpt-fake", "status": status}
    body.update(extra or {})
    return body


def text_events(text):
    item = {"type": "message", "id": "msg_fake", "role": "assistant", "content": []}
    return [
        {"type": "response.created", "response": response("in_progress")},
        {"type": "response.in_progress", "response": response("in_progress")},
        {"type": "response.output_item.added", "output_index": 0, "item": item},
        {"type": "response.content_part.added", "output_index": 0, "content_index": 0, "part": {"type": "output_text", "text": ""}},
        {"type": "response.output_text.delta", "output_index": 0, "content_index": 0, "delta": text[:5]},
        {"type": "response.output_text.delta", "output_index": 0, "content_index": 0, "delta": text[5:]},
        {"type": "response.output_text.done", "output_index": 0, "content_index": 0, "text": text},
        {"type": "response.content_part.done", "output_index": 0, "content_index": 0},
        {"type": "response.output_item.done", "output_index": 0, "item": item},
        {"type": "response.completed", "response": response("completed", {"usage": USAGE})},
    ]


def tool_events():
    arguments = json.dumps({"command": TOOL_COMMAND})
    item = {"type": "function_call", "id": "fc_fake", "call_id": "call_fake", "name": "Bash", "arguments": ""}
    done = dict(item, arguments=arguments)
    return [
        {"type": "response.created", "response": response("in_progress")},
        {"type": "response.output_item.added", "output_index": 0, "item": item},
        {"type": "response.function_call_arguments.delta", "output_index": 0, "item_id": "fc_fake", "delta": arguments[:10]},
        {"type": "response.function_call_arguments.delta", "output_index": 0, "item_id": "fc_fake", "delta": arguments[10:]},
        {"type": "response.function_call_arguments.done", "output_index": 0, "item_id": "fc_fake", "arguments": arguments},
        {"type": "response.output_item.done", "output_index": 0, "item": done},
        {"type": "response.completed", "response": response("completed", {"usage": USAGE})},
    ]


def answers_a_tool(request):
    items = request.get("input") or []
    return bool(items) and isinstance(items[-1], dict) and items[-1].get("type") == "function_call_output"


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
        if self.path != "/responses":
            return self.fail(404, "unknown path " + self.path)
        if EXPECTED_AUTHORIZATION and self.headers.get("authorization") != EXPECTED_AUTHORIZATION:
            return self.fail(401, "bad token")
        if request.get("store") is not False or request.get("stream") is not True:
            return self.fail(400, "store must be false and stream must be true")
        rejected = [name for name in REJECTED if name in request]
        if rejected:
            return self.fail(400, "unsupported fields: " + ", ".join(rejected))
        if any(isinstance(item, dict) and item.get("role") == "system" for item in request.get("input") or []):
            return self.fail(400, "system role items are not accepted")
        if MODE == "rate_limited":
            return self.fail(429, "Rate limit reached")
        events = tool_events() if MODE == "tool" and not answers_a_tool(request) else text_events("Tool finished: all good" if MODE == "tool" else "Hello from the fake responses upstream")
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.send_header("transfer-encoding", "chunked")
        self.end_headers()
        for event in events:
            chunk = sse(event)
            self.wfile.write(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
            self.wfile.flush()
        self.wfile.write(b"0\r\n\r\n")


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
server.daemon_threads = True
with open(PORT_FILE, "w") as port_file:
    port_file.write(str(server.server_address[1]))
server.serve_forever()
