import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOG = sys.argv[2]
DEFECT = sys.argv[3] if len(sys.argv) > 3 else "none"
TEXT = "Tool finished: all good" if DEFECT == "tool" else "Hello from the fake upstream"


def sse(name, data):
    return f"event: {name}\ndata: {json.dumps(data)}\n\n".encode()


def stream_events(model):
    start = {"type": "message_start", "message": {"id": "msg_fake", "type": "message", "role": "assistant", "model": model,
             "content": [], "stop_reason": None, "stop_sequence": None, "usage": {"input_tokens": 5, "output_tokens": 1}}}
    events = [("message_start", start)]
    if DEFECT == "duplicate_start":
        events += [("message_start", start)] * 3
    events += [
        ("content_block_start", {"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}}),
        ("ping", {"type": "ping"}),
        ("content_block_delta", {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": TEXT}}),
    ]
    if DEFECT != "unclosed_block":
        events.append(("content_block_stop", {"type": "content_block_stop", "index": 0}))
    events += [
        ("message_delta", {"type": "message_delta", "delta": {"stop_reason": "end_turn", "stop_sequence": None}, "usage": {"output_tokens": 7}}),
        ("message_stop", {"type": "message_stop"}),
    ]
    return events


def tool_events(model):
    start = {"type": "message_start", "message": {"id": "msg_fake_tool", "type": "message", "role": "assistant", "model": model,
             "content": [], "stop_reason": None, "stop_sequence": None, "usage": {"input_tokens": 9, "output_tokens": 1}}}
    delta = lambda text: ("content_block_delta", {"type": "content_block_delta", "index": 0, "delta": {"type": "input_json_delta", "partial_json": text}})
    return [
        ("message_start", start),
        ("content_block_start", {"type": "content_block_start", "index": 0, "content_block": {"type": "tool_use", "id": "toolu_fake1", "name": "Bash", "input": {}}}),
        delta('{"command":'),
        delta('"echo tool-ran"}'),
        ("content_block_stop", {"type": "content_block_stop", "index": 0}),
        ("message_delta", {"type": "message_delta", "delta": {"stop_reason": "tool_use", "stop_sequence": None}, "usage": {"output_tokens": 12}}),
        ("message_stop", {"type": "message_stop"}),
    ]


def answers_a_tool(request):
    messages = [m for m in request.get("messages") or [] if isinstance(m, dict) and m.get("role") != "system"]
    last = messages[-1].get("content") if messages else None
    return isinstance(last, list) and any(isinstance(block, dict) and block.get("type") == "tool_result" for block in last)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def record(self, body, extra=None):
        entry = {"method": self.command, "path": self.path, "headers": {k.lower(): v for k, v in self.headers.items()},
                 "body": body.decode(errors="replace")[:2000], **(extra or {})}
        with open(LOG, "a") as handle:
            handle.write(json.dumps(entry) + "\n")

    def do_POST(self):
        length = int(self.headers.get("content-length", 0))
        body = self.rfile.read(length)
        try:
            request = json.loads(body)
        except ValueError:
            request = {}
        has_result = answers_a_tool(request)
        self.record(body, {"tools": len(request.get("tools") or []), "has_tool_result": has_result})
        model = request.get("model", "unknown")
        if request.get("stream"):
            self.send_response(200)
            self.send_header("content-type", "text/event-stream")
            self.send_header("cache-control", "no-cache")
            self.send_header("transfer-encoding", "chunked")
            self.end_headers()
            wants_tool = DEFECT == "tool" and request.get("tools") and not has_result
            for name, data in (tool_events(model) if wants_tool else stream_events(model)):
                if DEFECT == "unclosed_block" and name == "content_block_stop":
                    continue
                chunk = sse(name, data)
                self.wfile.write(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
                self.wfile.flush()
            self.wfile.write(b"0\r\n\r\n")
            return
        reply = json.dumps({"id": "msg_fake", "type": "message", "role": "assistant", "model": model,
                            "content": [{"type": "text", "text": TEXT}], "stop_reason": "end_turn", "stop_sequence": None,
                            "usage": {"input_tokens": 5, "output_tokens": 7}}).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)

    def do_GET(self):
        self.record(b"")
        reply = b'{"data":[],"has_more":false}'
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
with open(sys.argv[1], "w") as handle:
    handle.write(str(server.server_address[1]))
server.serve_forever()
